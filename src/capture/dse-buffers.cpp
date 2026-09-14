/*
 * Broadcast Delay - target attach + GPU/RAM/disk buffer setup (see
 * dse-capture.hpp). The capture path (dse-capture.cpp) builds on these.
 */
#include "dse-capture.hpp"

#include "../audio/dse-audio.hpp"
#include "../core/dse-internal.hpp"
#include "core/dse-constants.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <cmath>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

void detach_target_locked(DelayedSource *s)
{
	detach_audio_locked(s);

	if (s->target) {
		if (s->showing_target) {
			obs_source_dec_showing(s->target);
			s->showing_target = false;
		}
		obs_source_release(s->target);
		s->target = nullptr;
	}
}

namespace {

struct CanvasTargetLookup {
	std::string canvas_name;
	std::string scene_name;
	obs_source_t *source = nullptr;
};

static bool find_canvas_scene(void *param, obs_canvas_t *canvas)
{
	auto *lookup = static_cast<CanvasTargetLookup *>(param);
	const char *name = obs_canvas_get_name(canvas);
	if (!name || lookup->canvas_name != name)
		return true;

	/* obs_canvas_get_source_by_name returns a strong source reference. */
	lookup->source =
		obs_canvas_get_source_by_name(canvas, lookup->scene_name.c_str());
	return false;
}

static obs_source_t *resolve_canvas_scene(const std::string &encoded)
{
	static const char prefix[] = "@canvas:";
	if (encoded.compare(0, sizeof(prefix) - 1, prefix) != 0)
		return nullptr;

	const size_t start = sizeof(prefix) - 1;
	const size_t separator = encoded.find('|', start);
	if (separator == std::string::npos || separator == start ||
	    separator + 1 >= encoded.size())
		return nullptr;

	CanvasTargetLookup lookup{
		encoded.substr(start, separator - start),
		encoded.substr(separator + 1),
		nullptr,
	};
	obs_enum_canvases(find_canvas_scene, &lookup);
	return lookup.source;
}

} // namespace

/* Resolve s->target_name and attach. Safe to call repeatedly: a no-op once
 * attached, and simply returns (to be retried) if the target is not loaded yet
 * -- which is how the startup load-order race self-heals. Lock held. */
void attach_target_locked(DelayedSource *s)
{
	if (s->target || s->target_name.empty())
		return;

	obs_source_t *t =
		s->mode_val == MODE_CANVAS_SCENE
			? resolve_canvas_scene(s->target_name)
			: obs_get_source_by_name(s->target_name.c_str());
	if (!t)
		return; /* not loaded yet; retried next frame */
	if (t == s->self) { /* never delay ourselves */
		obs_source_release(t);
		return;
	}

	s->target = t; /* keep the strong ref */
	/* Keep the target rendering live even when it is not the active scene,
	 * so scene switching does not freeze the delayed output. */
	obs_source_inc_showing(t);
	s->showing_target = true;

	gather_audio_locked(s);

	blog(LOG_INFO,
	     "[broadcast-delay] mode %d target '%s' (%zu audio source(s))",
	     s->mode_val, s->target_name.c_str(), s->audio_sources.size());
}

void set_target(DelayedSource *s, int mode, const char *name)
{
	std::lock_guard<std::mutex> lock(s->target_mutex);

	std::string key = std::to_string(mode) + "|" + (name ? name : "");
	if (s->target_key == key && s->target)
		return; /* unchanged */

	detach_target_locked(s);
	s->mode_val = mode;
	s->target_name = name ? name : "";
	s->target_key = key;
	attach_target_locked(s);
}

size_t required_capacity(uint64_t delay_ns, double fps)
{
	const double delay_sec = (double)delay_ns / 1000000000.0;
	const double frames = std::ceil(delay_sec * fps);
	return (size_t)frames + dse::kSafeHeadMargin + 1;
}

/* A stable automatic RAM budget (used when "Advanced" is off). */
uint64_t auto_ram_budget()
{
#ifdef _WIN32
	MEMORYSTATUSEX ms;
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms)) {
		uint64_t b = ms.ullTotalPhys / 4; /* 25% of physical RAM */
		return b < (2ULL << 30) ? (2ULL << 30) : b;
	}
#endif
	return 8ULL << 30;
}

/*
 * Hard ceiling for the RAM ring so it can never starve the system / OBS (OOM).
 * Whatever the user asks, the effective budget is clamped to leave headroom:
 * min(70% of physical, available - 3 GiB).
 */
uint64_t safe_ram_limit()
{
#ifdef _WIN32
	MEMORYSTATUSEX ms;
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms)) {
		const uint64_t reserve = 3ULL << 30; /* keep 3 GiB free */
		uint64_t avail = ms.ullAvailPhys > reserve
					 ? ms.ullAvailPhys - reserve
					 : ms.ullAvailPhys / 2;
		uint64_t cap70 = ms.ullTotalPhys / 10 * 7;
		uint64_t lim = avail < cap70 ? avail : cap70;
		return lim < (1ULL << 30) ? (1ULL << 30) : lim;
	}
#endif
	return 16ULL << 30;
}

void ensure_gpu_buffers(DelayedSource *s, uint32_t cx, uint32_t cy)
{
	if (s->buf_cx == cx && s->buf_cy == cy && s->stage[0] && s->upload_tex)
		return;

	for (gs_stagesurf_t *&st : s->stage) {
		if (st)
			gs_stagesurface_destroy(st);
		st = nullptr;
	}
	if (s->upload_tex) {
		gs_texture_destroy(s->upload_tex);
		s->upload_tex = nullptr;
	}
	if (s->upload_tex2) {
		gs_texture_destroy(s->upload_tex2);
		s->upload_tex2 = nullptr;
	}

	s->stage[0] = gs_stagesurface_create(cx, cy, GS_RGBA);
	s->stage[1] = gs_stagesurface_create(cx, cy, GS_RGBA);
	s->upload_tex = gs_texture_create(cx, cy, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	s->upload_tex2 = gs_texture_create(cx, cy, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	s->buf_cx = cx;
	s->buf_cy = cy;
	s->stage_primed = false;
	s->has_uploaded = false;
	s->stage_cur = 0;
	s->cap_ceiling = SIZE_MAX;
}

/* Release all buffers + GPU objects (when disabled). Graphics thread. */
void free_video_buffers(DelayedSource *s)
{
	s->ring.destroy();
	s->vram.destroy();
	s->last_vram = nullptr;
	for (gs_stagesurf_t *&st : s->stage) {
		if (st)
			gs_stagesurface_destroy(st);
		st = nullptr;
	}
	if (s->upload_tex) {
		gs_texture_destroy(s->upload_tex);
		s->upload_tex = nullptr;
	}
	if (s->upload_tex2) {
		gs_texture_destroy(s->upload_tex2);
		s->upload_tex2 = nullptr;
	}
	s->buf_cx = s->buf_cy = 0;
	s->stage_primed = false;
	s->has_uploaded = false;
	s->used_bytes.store(0, std::memory_order_relaxed);
}

void stop_disk(DelayedSource *s)
{
	s->disk.reset();
	s->disk_cx = s->disk_cy = 0;
	s->disk_fps = 0;
	s->disk_q = -1;
	s->disk_codec = -1;
	s->disk_cap = 0;
	s->disk_started_folder.clear();
	s->has_uploaded = false;
}

/*
 * Resolve the effective disk config and (re)start the backend on change.
 * Advanced off -> highest quality (RAW) + auto capacity (delay + margin) in
 * Advanced: the Compression choice + dedicated sliders for each format.
 */
void ensure_disk(DelayedSource *s, uint32_t cx, uint32_t cy, uint64_t delay_ns)
{
	int fps = (int)(active_fps() + 0.5);
	if (fps <= 0)
		fps = 60;

	int codec = s->disk_compression.load(std::memory_order_relaxed);
	int quality = 5;
	if (codec > 0) {
		obs_data_t *st = obs_source_get_settings(s->self);
		double qv = obs_data_get_double(st, "hls_bitrate");
		if (codec >= 3) {
			if (s->disk_auto_q.load(std::memory_order_relaxed)) {
				/* Auto HLS bitrate ~ 0.07 bits/pixel (H.264 medium),
				 * scaled by resolution + fps, clamped 2-50 Mbps. */
				double mbps = (double)cx * (double)cy *
					      (double)fps * 0.07 / 1.0e6;
				if (mbps < 2.0) mbps = 2.0;
				if (mbps > 50.0) mbps = 50.0;
				quality = (int)(mbps * 1000.0);
			} else {
				quality = (int)(qv * 1000.0);  /* Mbps -> kbps */
			}
		} else if (codec == 1) {
			quality = (int)(qv + 0.5); /* PNG: stb level 0-9 */
			if (quality < 0)
				quality = 0;
			if (quality > 9)
				quality = 9;
		} else {
			quality = (int)qv; /* MJPEG qscale */
			if (quality < 1)
				quality = 5;
		}
		if (codec != 1 && quality < 1)
			quality = 5;
		obs_data_release(st);
	}
	/* Capacity covers the target delay (the max you can rewind to), not the
	 * current playhead distance. */
	int secs = (int)std::ceil(
			   (double)s->delay_ns.load(std::memory_order_relaxed) /
			   1000000000.0) +
		   3;
	std::string folder;
	{
		std::lock_guard<std::mutex> lock(s->disk_cfg_mutex);
		if (!s->disk_args.folder.empty())
			folder = s->disk_args.folder;
		if (s->disk_args.codec >= 0)
			codec = s->disk_args.codec;
		if (s->disk_args.quality > 0)
			quality = s->disk_args.quality;
		if (s->disk_args.seconds > 0)
			secs = s->disk_args.seconds;
	}
	if (folder.empty()) {
		obs_data_t *st = obs_source_get_settings(s->self);
		const char *f = obs_data_get_string(st, "disk_folder");
		if (f && f[0]) folder = f;
		obs_data_release(st);
	}
	if (folder.empty())
		folder = default_temp_folder();
	if (secs < 1)
		secs = 1;
	size_t cap = (size_t)secs * (size_t)fps;
	if (cap < 2)
		cap = 2;

	if (s->disk && (s->disk_cx != cx || s->disk_cy != cy ||
			s->disk_fps != fps || s->disk_codec != codec ||
			s->disk_q != quality || s->disk_cap != cap ||
			s->disk_started_folder != folder))
		stop_disk(s);

	if (!s->disk) {
		os_mkdirs(folder.c_str());
		s->disk = DiskBuffer::create(folder, cx, cy, fps, cap, codec,
					     quality);
		s->frames_buffered.store(0, std::memory_order_relaxed);
		if (s->disk) {
			s->disk_cx = cx;
			s->disk_cy = cy;
			s->disk_fps = fps;
			s->disk_codec = codec;
			s->disk_q = quality;
			s->disk_cap = cap;
			s->disk_started_folder = folder;
			s->disk_failed.store(false, std::memory_order_relaxed);
		} else {
			s->disk_failed.store(true, std::memory_order_relaxed);
			blog(LOG_ERROR,
			     "[broadcast-delay] failed to start disk buffer (folder '%s')",
			     folder.c_str());
		}
	}
	if (s->disk)
		s->disk->set_delay_ns(delay_ns);
}
