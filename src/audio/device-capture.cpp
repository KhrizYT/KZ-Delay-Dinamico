/*
 * Broadcast Delay - audio capture implementation (miniaudio backend).
 *
 * Replaces the hand-rolled WASAPI client with miniaudio, which handles device
 * enumeration, loopback capture, format/rate negotiation and resampling on
 * Windows (WASAPI), macOS (CoreAudio) and Linux (PulseAudio/ALSA). The public
 * DeviceCapture interface is unchanged so the rest of the plugin is untouched.
 */
#include "device-capture.hpp"

#include "../vendor/miniaudio/miniaudio.h"

#include <util/platform.h>

#include <mutex>
#include <cstring>

namespace {

/* One shared miniaudio context. Device IDs returned by enumeration are only
 * valid for the context that produced them, so we keep it alive process-wide
 * and resolve string IDs -> ma_device_id through a cache built at enum time. */
struct DevEntry {
	std::string id;     /* stable string key = friendly name */
	bool is_input;      /* capture (mic) vs render (loopback target) */
	ma_device_id maid;  /* opaque miniaudio handle */
};

std::mutex g_mtx;
ma_context g_ctx;
bool g_ctx_ok = false;
std::vector<DevEntry> g_cache;

bool ensure_ctx()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	if (g_ctx_ok)
		return true;
	if (ma_context_init(nullptr, 0, nullptr, &g_ctx) != MA_SUCCESS) {
		blog(LOG_ERROR, "[capture] ma_context_init failed");
		return false;
	}
	g_ctx_ok = true;
	return true;
}

/* Per-device runtime state attached to a started ma_device. */
struct CapCtx {
	std::function<void(const float *data[], size_t frames, uint64_t ts)> on_data;
	size_t channels = 2;
	uint32_t rate = 48000;
	uint64_t start_ns = 0;     /* wall clock at first block */
	uint64_t total_frames = 0; /* monotonic frame counter */
	std::vector<std::vector<float>> planar;
	std::vector<const float *> ptrs;
};

void data_cb(ma_device *dev, void *, const void *input, ma_uint32 nframes)
{
	auto *c = static_cast<CapCtx *>(dev->pUserData);
	if (!c || !input || nframes == 0)
		return;
	const float *src = static_cast<const float *>(input);
	const size_t ch = c->channels;
	for (size_t k = 0; k < ch; k++) {
		c->planar[k].resize(nframes);
		c->ptrs[k] = c->planar[k].data();
	}
	/* miniaudio delivers interleaved f32 -> deinterleave to planar. */
	for (ma_uint32 i = 0; i < nframes; i++)
		for (size_t k = 0; k < ch; k++)
			c->planar[k][i] = src[(size_t)i * ch + k];

	/* Monotonic timestamp from a frame counter (NOT wall-clock-at-callback):
	 * consecutive blocks are exactly contiguous, so the ring never sees the
	 * scheduling jitter that produces clicks/crackle. Anchored once to the
	 * real clock so the delay stays aligned. */
	if (c->start_ns == 0)
		c->start_ns = (uint64_t)os_gettime_ns();
	const uint64_t ts = c->start_ns +
			    (c->total_frames * 1000000000ULL) / c->rate;
	c->total_frames += nframes;
	c->on_data(c->ptrs.data(), nframes, ts);
}

/* ---- live monitoring playback (default output) ---- */
struct MonState {
	std::mutex mtx;
	ma_device dev;
	bool active = false;
	std::vector<float> ring; /* interleaved, cap frames * ch */
	size_t cap = 0, ch = 2;
	uint64_t readf = 0, writef = 0;
};
MonState g_mon;

void mon_playback_cb(ma_device *dev, void *output, const void *, ma_uint32 nframes)
{
	auto *m = static_cast<MonState *>(dev->pUserData);
	float *o = static_cast<float *>(output);
	const size_t ch = m->ch;
	std::lock_guard<std::mutex> lk(m->mtx);
	for (ma_uint32 i = 0; i < nframes; i++) {
		if (m->readf < m->writef && m->cap) {
			const size_t b = (size_t)(m->readf % m->cap) * ch;
			for (size_t c = 0; c < ch; c++) {
				o[(size_t)i * ch + c] = m->ring[b + c];
				m->ring[b + c] = 0.0f; /* consumed -> ready to re-sum */
			}
			m->readf++;
		} else {
			for (size_t c = 0; c < ch; c++)
				o[(size_t)i * ch + c] = 0.0f;
		}
	}
}

} // namespace

namespace AudioMonitor {

void set_active(bool on, size_t channels, size_t rate)
{
	std::lock_guard<std::mutex> lk(g_mon.mtx);
	if (on == g_mon.active)
		return;
	if (on) {
		g_mon.ch = channels ? channels : 2;
		g_mon.cap = rate ? rate : 48000; /* 1 s mixing ring */
		g_mon.ring.assign(g_mon.cap * g_mon.ch, 0.0f);
		g_mon.readf = g_mon.writef = 0;
		ma_device_config cfg =
			ma_device_config_init(ma_device_type_playback);
		cfg.playback.format = ma_format_f32;
		cfg.playback.channels = (ma_uint32)g_mon.ch;
		cfg.sampleRate = (ma_uint32)g_mon.cap;
		cfg.dataCallback = mon_playback_cb;
		cfg.pUserData = &g_mon;
		if (ma_device_init(nullptr, &cfg, &g_mon.dev) != MA_SUCCESS) {
			blog(LOG_ERROR, "[monitor] playback init failed");
			return;
		}
		if (ma_device_start(&g_mon.dev) != MA_SUCCESS) {
			blog(LOG_ERROR, "[monitor] playback start failed");
			ma_device_uninit(&g_mon.dev);
			return;
		}
		g_mon.active = true;
		blog(LOG_INFO, "[monitor] output started %zuch %zuHz",
		     g_mon.ch, g_mon.cap);
	} else {
		ma_device_uninit(&g_mon.dev); /* stops + joins */
		g_mon.active = false;
		blog(LOG_INFO, "[monitor] output stopped");
	}
}

void push(const float *const *planar, size_t channels, size_t frames, float gain)
{
	std::lock_guard<std::mutex> lk(g_mon.mtx);
	if (!g_mon.active || !g_mon.cap || frames == 0)
		return;
	const size_t ch = g_mon.ch;
	const size_t cushion = frames * 2; /* ~20 ms ahead of the play head */
	uint64_t start = g_mon.readf + cushion;
	/* Drop oldest if we would lap the read head (playback underrun). */
	if (start + frames > g_mon.readf + g_mon.cap)
		start = g_mon.readf + g_mon.cap - frames;
	for (size_t i = 0; i < frames; i++) {
		const size_t b = (size_t)((start + i) % g_mon.cap) * ch;
		for (size_t c = 0; c < ch; c++) {
			const float v =
				(c < channels ? planar[c][i] : planar[0][i]) *
				gain;
			g_mon.ring[b + c] += v;
		}
	}
	if (start + frames > g_mon.writef)
		g_mon.writef = start + frames;
}

} // namespace AudioMonitor

std::vector<DeviceCapture::Device> DeviceCapture::enumerate_devices()
{
	std::vector<Device> list;
	if (!ensure_ctx())
		return list;

	ma_device_info *play = nullptr, *cap = nullptr;
	ma_uint32 nplay = 0, ncap = 0;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		if (ma_context_get_devices(&g_ctx, &play, &nplay, &cap, &ncap) !=
		    MA_SUCCESS) {
			blog(LOG_ERROR, "[capture] get_devices failed");
			return list;
		}
		g_cache.clear();

		/* Playback devices -> captured via loopback. */
		for (ma_uint32 i = 0; i < nplay; i++) {
			Device d;
			d.name = play[i].name;
			d.id = play[i].name;
			d.is_input = false;
			d.is_default = play[i].isDefault != 0;
			list.push_back(d);
			g_cache.push_back({d.id, false, play[i].id});
		}
		/* Capture devices (microphones / virtual outputs). */
		for (ma_uint32 i = 0; i < ncap; i++) {
			Device d;
			d.name = std::string("[MIC] ") + cap[i].name;
			d.id = cap[i].name;
			d.is_input = true;
			d.is_default = cap[i].isDefault != 0;
			list.push_back(d);
			g_cache.push_back({d.id, true, cap[i].id});
		}
	}
	return list;
}

DeviceCapture::DeviceCapture() = default;
DeviceCapture::~DeviceCapture() { stop(); }

bool DeviceCapture::start(const char *device_id, bool is_input, size_t channels,
			  size_t rate,
			  std::function<void(const float *data[], size_t frames,
					     uint64_t ts)>
				  on_data)
{
	stop();
	if (!ensure_ctx())
		return false;

	ma_device_id maid;
	bool have_id = false;
	if (device_id && device_id[0]) {
		std::lock_guard<std::mutex> lk(g_mtx);
		for (const auto &e : g_cache) {
			if (e.is_input == is_input && e.id == device_id) {
				maid = e.maid;
				have_id = true;
				break;
			}
		}
		if (!have_id) {
			blog(LOG_WARNING,
			     "[capture] device not found: '%s' (input=%d)",
			     device_id, (int)is_input);
			return false;
		}
	}

	/* Mic -> capture; speaker/render -> loopback (capture what it plays). */
	ma_device_config cfg = ma_device_config_init(
		is_input ? ma_device_type_capture : ma_device_type_loopback);
	cfg.capture.pDeviceID = have_id ? &maid : nullptr;
	cfg.capture.format = ma_format_f32;
	cfg.capture.channels = (ma_uint32)channels;
	cfg.capture.shareMode = ma_share_mode_shared;
	cfg.sampleRate = (ma_uint32)rate;
	cfg.dataCallback = data_cb;

	auto *c = new CapCtx();
	c->on_data = std::move(on_data);
	c->channels = channels;
	c->rate = (uint32_t)(rate ? rate : 48000);
	c->planar.assign(channels, {});
	c->ptrs.assign(channels, nullptr);
	cfg.pUserData = c;

	auto *dev = new ma_device();
	ma_result r = ma_device_init(&g_ctx, &cfg, dev);
	if (r != MA_SUCCESS) {
		blog(LOG_ERROR, "[capture] device_init failed (%d) input=%d",
		     (int)r, (int)is_input);
		delete dev;
		delete c;
		return false;
	}
	r = ma_device_start(dev);
	if (r != MA_SUCCESS) {
		blog(LOG_ERROR, "[capture] device_start failed (%d)", (int)r);
		ma_device_uninit(dev);
		delete dev;
		delete c;
		return false;
	}

	blog(LOG_INFO,
	     "[capture] started '%s': out=%uch native=%uch %uHz input=%d (miniaudio %s)",
	     (device_id && device_id[0]) ? device_id : "(default)",
	     dev->capture.channels, dev->capture.internalChannels,
	     dev->sampleRate, (int)is_input,
	     ma_get_backend_name(dev->pContext->backend));

	client_ = dev;
	capture_ = c;
	active_.store(true, std::memory_order_relaxed);
	return true;
}

void DeviceCapture::stop()
{
	active_.store(false, std::memory_order_relaxed);

	auto *dev = static_cast<ma_device *>(client_);
	if (dev) {
		ma_device_uninit(dev); /* stops + joins the capture thread */
		delete dev;
		client_ = nullptr;
	}
	auto *c = static_cast<CapCtx *>(capture_);
	if (c) {
		delete c;
		capture_ = nullptr;
	}
}
