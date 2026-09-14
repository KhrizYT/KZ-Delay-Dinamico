/*
 * Broadcast Delay - public C API implementation (see include/broadcast_delay_api.h).
 *
 * Thin, thread-safe accessors over the instance registry. Other plugins resolve
 * these exported symbols at runtime to detect Broadcast Delay and read state.
 */
#include "broadcast_delay_api.h"

#include "dse-internal.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/* Registered video/audio taps, keyed by instance. Guarded by g_tap_mutex; the
 * callback list is copied out before dispatch so callbacks never run under the
 * lock (they may re-enter register/unregister). */
namespace {
struct RegTap {
	std::string id;
	int mode = BD_TAP_LIVE;
	int64_t offset_ns = 0;
	void (*on_video)(void *, bd_handle_t, const bd_video_frame_t *) = nullptr;
	void (*on_audio)(void *, bd_handle_t, const bd_audio_block_t *) = nullptr;
	void *ctx = nullptr;
};
std::mutex g_tap_mutex;
std::unordered_map<DelayedSource *, std::vector<RegTap>> g_taps;
} /* namespace */

extern "C" {

int bd_is_available(void)
{
	return 1;
}

int bd_api_version(void)
{
	return BD_API_VERSION;
}

size_t bd_list_instances(bd_handle_t *out, size_t cap)
{
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	size_t n = 0;
	for (DelayedSource *s : g_registry) {
		if (out && n < cap)
			out[n] = (bd_handle_t)s;
		n++;
	}
	return n;
}

bd_handle_t bd_from_obs_source(obs_source_t *src)
{
	if (!src)
		return nullptr;
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry)
		if (s->self == src)
			return (bd_handle_t)s;
	return nullptr;
}

int bd_get_info(bd_handle_t h, bd_instance_info_t *info)
{
	if (!h || !info)
		return 0;

	std::lock_guard<std::mutex> lock(g_reg_mutex);
	/* Validate the handle against the live registry (no use-after-free). */
	DelayedSource *s = nullptr;
	for (DelayedSource *d : g_registry)
		if (d == (DelayedSource *)h) {
			s = d;
			break;
		}
	if (!s)
		return 0;

	std::memset(info, 0, sizeof(*info));
	const char *nm = obs_source_get_name(s->self);
	if (nm) {
		std::strncpy(info->source_name, nm,
			     sizeof(info->source_name) - 1);
		info->source_name[sizeof(info->source_name) - 1] = '\0';
	}
	info->delay_ns = s->delay_ns.load(std::memory_order_relaxed);
	info->buffered_ns =
		(uint64_t)s->buffered_ns.load(std::memory_order_relaxed);

	const int64_t now = (int64_t)os_gettime_ns();
	const int64_t ph = s->playhead_ns.load(std::memory_order_relaxed);
	info->playhead_capture_ts = ph > 0 ? (uint64_t)ph : 0;
	int64_t dist = now - ph;
	info->distance_behind_live_ns = dist > 0 ? dist : 0;

	info->warp_state = s->warp_state.load(std::memory_order_relaxed);
	info->storage = s->storage.load(std::memory_order_relaxed);
	info->width = s->cx.load(std::memory_order_relaxed);
	info->height = s->cy.load(std::memory_order_relaxed);
	info->fps = active_fps();
	info->underflows = s->underflows.load(std::memory_order_relaxed);
	info->disk_error =
		s->disk_failed.load(std::memory_order_relaxed) ? 1 : 0;
	return 1;
}

int bd_register_tap(bd_handle_t h, const bd_tap_t *tap)
{
	if (!h || !tap || !tap->extension_id)
		return 0;
	{ /* validate the handle against the live registry */
		std::lock_guard<std::mutex> lock(g_reg_mutex);
		bool ok = false;
		for (DelayedSource *d : g_registry)
			if (d == (DelayedSource *)h) {
				ok = true;
				break;
			}
		if (!ok)
			return 0;
	}
	std::lock_guard<std::mutex> lock(g_tap_mutex);
	auto &v = g_taps[(DelayedSource *)h];
	for (RegTap &t : v) { /* replace if same id */
		if (t.id == tap->extension_id) {
			t.mode = tap->mode;
			t.offset_ns = tap->offset_ns;
			t.on_video = tap->on_video;
			t.on_audio = tap->on_audio;
			t.ctx = tap->ctx;
			return 1;
		}
	}
	RegTap rt;
	rt.id = tap->extension_id;
	rt.mode = tap->mode;
	rt.offset_ns = tap->offset_ns;
	rt.on_video = tap->on_video;
	rt.on_audio = tap->on_audio;
	rt.ctx = tap->ctx;
	v.push_back(std::move(rt));
	return 1;
}

void bd_unregister_tap(bd_handle_t h, const char *extension_id)
{
	if (!h || !extension_id)
		return;
	std::lock_guard<std::mutex> lock(g_tap_mutex);
	auto it = g_taps.find((DelayedSource *)h);
	if (it == g_taps.end())
		return;
	auto &v = it->second;
	v.erase(std::remove_if(v.begin(), v.end(),
			       [&](const RegTap &t) {
				       return t.id == extension_id;
			       }),
		v.end());
	if (v.empty())
		g_taps.erase(it);
}

int bd_set_tap_offset(bd_handle_t h, const char *extension_id, int64_t offset_ns)
{
	if (!h || !extension_id)
		return 0;
	std::lock_guard<std::mutex> lock(g_tap_mutex);
	auto it = g_taps.find((DelayedSource *)h);
	if (it == g_taps.end())
		return 0;
	for (RegTap &t : it->second)
		if (t.id == extension_id) {
			t.offset_ns = offset_ns;
			return 1;
		}
	return 0;
}

} /* extern "C" */

/* ---- engine-internal (C++ linkage; declared in dse-internal.hpp) ---- */

/* Dispatch the RGBA video taps for `s`. v1: RAM ring only. Graphics thread. */
void bd_dispatch_video_taps(DelayedSource *s)
{
	std::vector<RegTap> taps;
	{
		std::lock_guard<std::mutex> lock(g_tap_mutex);
		auto it = g_taps.find(s);
		if (it == g_taps.end() || it->second.empty())
			return;
		taps = it->second; /* copy: don't hold the lock over callbacks */
	}
	if (s->storage.load(std::memory_order_relaxed) != STORAGE_RAM ||
	    !s->ring.ready())
		return;
	const uint32_t cx = s->cx.load(std::memory_order_relaxed);
	const uint32_t cy = s->cy.load(std::memory_order_relaxed);
	if (!cx || !cy)
		return;
	const uint64_t now = (uint64_t)os_gettime_ns();

	for (const RegTap &t : taps) {
		if (!t.on_video)
			continue;
		if (t.mode == BD_TAP_LIVE)
			continue; /* LIVE taps are fed the freshly captured frame
				   * in bd_dispatch_live_frame (works for every
				   * storage backend, not just RAM). */
		uint64_t target;
		if (t.mode == BD_TAP_PROGRAM) {
			int64_t ph = s->playhead_ns.load(std::memory_order_relaxed);
			target = ph > 0 ? (uint64_t)ph : now;
		} else if (t.mode == BD_TAP_OFFSET) {
			target = (int64_t)now > t.offset_ns
					 ? now - (uint64_t)t.offset_ns
					 : 0;
		} else { /* BD_TAP_LIVE */
			target = now;
		}
		const uint8_t *rgba = s->ring.sample(target);
		if (!rgba)
			continue;
		bd_video_frame_t f;
		std::memset(&f, 0, sizeof(f));
		f.rgba = rgba;
		f.width = cx;
		f.height = cy;
		f.linesize = cx * 4;
		f.capture_ts_ns = target;
		f.offset_from_live_ns = now > target ? (int64_t)(now - target) : 0;
		t.on_video(t.ctx, (bd_handle_t)s, &f);
	}
}

/* Feed every LIVE tap the freshly captured frame (RAM pointer valid only for the
 * call). Called from capture_frame's readback path, so it works for ALL storage
 * backends (RAM / disk / VRAM-readback), unlike the ring-sampling path above. */
void bd_dispatch_live_frame(DelayedSource *s, const uint8_t *rgba, uint32_t cx,
			    uint32_t cy, uint32_t linesize, uint64_t ts)
{
	if (!rgba || !cx || !cy)
		return;
	std::vector<RegTap> taps;
	{
		std::lock_guard<std::mutex> lock(g_tap_mutex);
		auto it = g_taps.find(s);
		if (it == g_taps.end() || it->second.empty())
			return;
		taps = it->second;
	}
	for (const RegTap &t : taps) {
		if (!t.on_video || t.mode != BD_TAP_LIVE)
			continue;
		bd_video_frame_t f;
		std::memset(&f, 0, sizeof(f));
		f.rgba = rgba;
		f.width = cx;
		f.height = cy;
		f.linesize = linesize;
		f.capture_ts_ns = ts;
		f.offset_from_live_ns = 0;
		t.on_video(t.ctx, (bd_handle_t)s, &f);
	}
}

/* Feed every LIVE audio tap the freshly captured (pre-delay) audio block. The
 * planar pointers are valid only for the call. Runs on the OBS/WASAPI audio
 * thread. Lets a consumer transcribe what BD buffers, ahead of the delayed
 * output. */
void bd_dispatch_live_audio(DelayedSource *s, const float *const *planar,
			    uint32_t frames, uint32_t channels, uint32_t rate,
			    uint64_t ts)
{
	if (!planar || !frames || !channels)
		return;
	std::vector<RegTap> taps;
	{
		std::lock_guard<std::mutex> lock(g_tap_mutex);
		auto it = g_taps.find(s);
		if (it == g_taps.end() || it->second.empty())
			return;
		bool wanted = false;
		for (const RegTap &t : it->second)
			if (t.on_audio && t.mode == BD_TAP_LIVE) {
				wanted = true;
				break;
			}
		if (!wanted)
			return; /* nobody listening: skip the copy + dispatch */
		taps = it->second;
	}
	bd_audio_block_t a;
	std::memset(&a, 0, sizeof(a));
	a.planar = planar;
	a.frames = frames;
	a.channels = channels;
	a.rate = rate;
	a.timestamp_ns = ts;
	a.offset_from_live_ns = 0;
	for (const RegTap &t : taps)
		if (t.on_audio && t.mode == BD_TAP_LIVE)
			t.on_audio(t.ctx, (bd_handle_t)s, &a);
}

/* Drop all taps for a source being destroyed (called from dse_destroy). */
void bd_forget_source(DelayedSource *s)
{
	std::lock_guard<std::mutex> lock(g_tap_mutex);
	g_taps.erase(s);
}
