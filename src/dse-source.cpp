/*
 * Broadcast Delay - OBS source
 *
 * A native OBS input source that mirrors another source (Source Mode) or a
 * whole scene (Scene Mode) and plays it back with a synchronized audio+video
 * delay. Video frames live in a GPU texture ring (VideoRing); audio lives in a
 * RAM delay line (AudioDelay). Both reference a single OS monotonic clock and
 * the same delay value, so they stay in sync without per-stream correction.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/threading.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <QMessageBox>

#include "delay/video-ring.hpp"
#include "delay/vram-ring.hpp"
#include "audio/audio-ring.hpp"
#include "audio/dse-audio.hpp"
#include "capture/dse-capture.hpp"
#include "config/dse-hotkeys.hpp"
#include "config/dse-properties.hpp"
#include "core/dse-internal.hpp"
#include "render/dse-render.hpp"
#include "delay/disk-buffer.hpp"
#include "core/dse-constants.hpp"
#include "gfx/gfx-util.hpp"
#include "warp/warp-control.hpp"
#include "audio/device-capture.hpp"
#include "audio/mixer-state.hpp"

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0.0"
#endif

/* Cross-cutting tuning constants live in dse-constants.hpp (namespace dse).
 * Short aliases keep the call sites here readable. */
static constexpr size_t SAFE_HEAD_MARGIN = dse::kSafeHeadMargin;

namespace {

/* DelayedSource + DiskArgs now live in core/dse-internal.hpp so the engine can
 * be split across modules that share one definition. */

/* g_reg_mutex, g_registry, g_trans_dur_ms, is_docks(), active_fps() are the
 * shared engine spine -- declared in core/dse-internal.hpp, defined in
 * core/dse-globals.cpp. The AI master list (g_ai_*) lives in ai/ai-models.hpp. */

/* Dock scene + shared transition state/functions live in warp/dse-dock-scene.cpp.
 * g_dock_mutex + g_pause_scene + g_trans_dur_ms are the spine
 * (core/dse-globals.cpp). */

/* -------------------------------------------------------------------------- */
/* Target management                                                           */
/* -------------------------------------------------------------------------- */

/* Audio capture + delayed mix/emit (audio_capture_cb, feed_hw_audio_direct,
 * reconcile_hw, gather/detach_audio_locked) live in audio/dse-audio.cpp. */

/* Target attach/detach + set_target are in capture/dse-buffers.cpp.
 * MODE_ and STORAGE_ enums are in core/dse-internal.hpp; WARP_ in
 * warp-control.hpp. */

/* -------------------------------------------------------------------------- */
/* Capacity / memory helpers                                                   */
/* -------------------------------------------------------------------------- */

/* active_fps() is now inline in core/dse-internal.hpp. */

/* required_capacity() is in capture/dse-buffers.cpp. */

/* -------------------------------------------------------------------------- */
/* OBS source callbacks                                                        */
/* -------------------------------------------------------------------------- */

static const char *dse_get_name(void *)
{
	return obs_module_text("DelayedSourceEngine");
}

/* auto_ram_budget() + safe_ram_limit() are in capture/dse-buffers.cpp. */

static void dse_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<DelayedSource *>(data);
	const double GB = 1024.0 * 1024.0 * 1024.0;

	s->enabled.store(obs_data_get_bool(settings, "enabled"),
			 std::memory_order_relaxed);

	const double delay_sec = obs_data_get_double(settings, "delay_sec");
	const uint64_t dly = (uint64_t)(delay_sec * 1000000000.0);
	s->delay_ns.store(dly, std::memory_order_relaxed);

	const bool budget_auto = obs_data_get_bool(settings, "budget_auto");

	s->storage.store((int)obs_data_get_int(settings, "storage"),
			 std::memory_order_relaxed);
	s->audio_storage.store((int)obs_data_get_int(settings, "audio_storage"),
			       std::memory_order_relaxed);

	/* budget_auto is the "Advanced options" checkbox (ON = manual controls,
	 * OFF = automatic optimal). When OFF on disk, default to MJPEG - a
	 * compressed, light codec that works reliably at high resolution (RAW is
	 * pixel-perfect but ~300-490 MB/s at 2560x1600 and stalls the GPU; HLS/DASH
	 * stay available as manual choices for long, segmented delays). When ON,
	 * use the chosen codec + bitrate. */
	const int storage_val = (int)obs_data_get_int(settings, "storage");
	const bool auto_disk = (!budget_auto && storage_val == 1);
	const int compression_raw =
		auto_disk ? 2 /* MJPEG */
			  : (int)obs_data_get_int(settings, "disk_compression");
	s->disk_compression.store(compression_raw, std::memory_order_relaxed);
	s->disk_auto_q.store(auto_disk, std::memory_order_relaxed);

	/* Effective RAM budget: auto or manual. */
	uint64_t budget = budget_auto
				  ? auto_ram_budget()
				  : (uint64_t)(obs_data_get_double(settings,
								   "budget_gb") *
					       GB);
	{
		std::lock_guard<std::mutex> lock(s->disk_cfg_mutex);
		if (s->disk_args.ram_gb > 0.0)
			budget = (uint64_t)(s->disk_args.ram_gb * GB);
	}
	/* OOM guard: never exceed a safe fraction of physical RAM. */
	const uint64_t safe = safe_ram_limit();
	if (budget > safe) {
		blog(LOG_WARNING,
		     "[broadcast-delay] RAM budget %.1f GB clamped to %.1f GB to avoid OOM",
		     (double)budget / GB, (double)safe / GB);
		budget = safe;
	}
	s->budget_bytes.store(budget, std::memory_order_relaxed);

	/* Pre-init audio snap so the audio thread doesn't use the stale
	 * default (0 = live position) before the first video tick runs. */
	s->audio_snap_ns.store(50000000LL, std::memory_order_relaxed);

	/* Sync manual audio source selection. */
	s->audio_auto = obs_data_get_bool(settings, "audio_auto");
	if (s->audio_auto) {
		/* OBS mode: capture checked sources. */
		s->audio_sel.clear();
		static obs_data_t *stashed = nullptr;
		stashed = settings;
		obs_enum_sources(
			[](void *param, obs_source_t *src) {
				auto *sel = (std::vector<std::string> *)param;
				if (!(obs_source_get_output_flags(src) &
				      OBS_SOURCE_AUDIO))
					return true;
				const char *nm =
					obs_source_get_name(src);
				if (nm && obs_data_get_bool(stashed, nm))
					sel->push_back(nm);
				return true;
			},
			&s->audio_sel);
		stashed = nullptr;
	} else {
		s->audio_sel.clear();
		/* Native-OS mode: list this source's WASAPI devices in its own
		 * mixer; the popup table's [x] column drives which one is captured
		 * (see reconcile_hw in dse_video_tick). */
#ifdef _WIN32
		std::vector<std::string> wnames, wids;
		std::vector<bool> winputs, wdefaults;
		for (const auto &d : DeviceCapture::enumerate_devices()) {
			wnames.push_back(d.name);
			wids.push_back(d.id);
			winputs.push_back(d.is_input);
			wdefaults.push_back(d.is_default);
		}
		mixer_state_set_devices(*s->mixer_state, wnames, wids, winputs,
					wdefaults);
#endif
	}

	const int mode = (int)obs_data_get_int(settings, "mode");
	s->mode_atomic.store(mode, std::memory_order_relaxed);
	if (mode == MODE_DOCKS) {
		/* Target is driven by the dock's scene selection. */
		set_target(s, mode, warp_current_dock_scene().c_str());
	} else {
		set_target(s, mode, obs_data_get_string(settings, "target"));
	}
}

/* Bindable hotkey (Settings -> Hotkeys): toggle the delay on/off. */
/* Hotkey callbacks + (un)registration live in config/dse-hotkeys.cpp. */

static void *dse_create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new DelayedSource();
	s->self = source;

	{
		std::lock_guard<std::mutex> lock(g_reg_mutex);
		g_registry.push_back(s);
	}

	obs_enter_graphics();
	s->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->blur_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE); /* countdown scratch */
	obs_leave_graphics();

	dse_update(s, settings);

	register_dse_hotkeys(s);

	/* Capture continuously, even when this source is not on screen, so the
	 * buffer is always pre-filled. */
	obs_add_main_render_callback(main_render_cb, s);
	return s;
}

static void dse_destroy(void *data)
{
	auto *s = static_cast<DelayedSource *>(data);

#ifdef _WIN32
	/* Stop + join all WASAPI capture threads before any member is torn down,
	 * so their feed callbacks can't touch freed fields. */
	for (auto &h : s->hw_caps)
		if (h.cap) h.cap->stop();
	s->hw_caps.clear();
	s->hw_active.store(false, std::memory_order_relaxed);
	AudioMonitor::set_active(false, 0, 0);
#endif

	{
		std::lock_guard<std::mutex> lock(g_reg_mutex);
		for (size_t i = 0; i < g_registry.size(); i++) {
			if (g_registry[i] == s) {
				g_registry.erase(g_registry.begin() + i);
				break;
			}
		}
	}
	bd_forget_source(s); /* drop any public-API taps on this instance */

	/* Stop the capture callback before tearing anything down. */
	obs_remove_main_render_callback(main_render_cb, s);

	unregister_dse_hotkeys(s);

	s->disk.reset(); /* join disk threads + clean up files */

	{
		std::lock_guard<std::mutex> lock(s->target_mutex);
		detach_target_locked(s);
	}

	obs_enter_graphics();
	s->ring.destroy();
	s->vram.destroy();
	if (s->texrender)
		gs_texrender_destroy(s->texrender);
	if (s->blur_texrender)
		gs_texrender_destroy(s->blur_texrender);
	for (gs_stagesurf_t *&st : s->stage) {
		if (st)
			gs_stagesurface_destroy(st);
		st = nullptr;
	}
	if (s->upload_tex)
		gs_texture_destroy(s->upload_tex);
	if (s->upload_tex2)
		gs_texture_destroy(s->upload_tex2);
	if (s->warp_from_tex)
		gs_texture_destroy(s->warp_from_tex);
	obs_leave_graphics();

	delete s;
}

static uint32_t dse_get_width(void *data)
{
	return static_cast<DelayedSource *>(data)->cx.load(std::memory_order_relaxed);
}

static uint32_t dse_get_height(void *data)
{
	return static_cast<DelayedSource *>(data)->cy.load(std::memory_order_relaxed);
}

/* Recreate the readback/upload GPU objects when the target size changes. */
/* ensure_gpu_buffers / free_video_buffers / stop_disk / ensure_disk are in
 * capture/dse-buffers.cpp. */

/* capture_frame + main_render_cb are in capture/dse-capture.cpp. */

/*
 * Time-warp state machine: advances the playhead each frame according to the
 * current state. The playhead is the capture-time we display; distance behind
 * live = now - playhead. Audio delay only changes in the two stable states
 * (LIVE / DELAYED) to avoid pitch artefacts during accel/decel.
 */
/* dse_video_tick is in capture/dse-capture.cpp. */

/*
 * Playback half - draws the delayed frame whenever our source is visible. The
 * buffer is filled independently by capture_frame, so this just samples it.
 */
/* warp_trans_snapshot/progress + dse_video_render are in render/dse-render.cpp. */

/* -------------------------------------------------------------------------- */
/* Properties                                                                  */
/* -------------------------------------------------------------------------- */

/* dse_get_properties + dse_get_defaults live in config/dse-properties.cpp. */

} // namespace

/* -------------------------------------------------------------------------- */
/* Time-warp control (called by the Qt dock; see warp-control.hpp)             */
/* -------------------------------------------------------------------------- */

/* is_docks() is now inline in core/dse-internal.hpp. */

/* warp_set_state / toggle_pause / seek / seek_live / play_current /
 * set_play_speed / set_factors / seek_step / set_seek_step are in
 * warp/warp-transport.cpp. */

/* Dock scene + shared transition (ensure_dock_transition, warp_acquire_dock_render,
 * warp_shutdown, warp_refresh_transition, warp_any_scene_mode, warp_set_dock_scene,
 * warp_set/get_redirect_scene, warp_set_pause_scene) are in
 * warp/dse-dock-scene.cpp. */

void warp_set_countdown_overlay(bool on)
{
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry)
		if (is_docks(s))
			s->show_countdown.store(on, std::memory_order_relaxed);
}

/* warp_set_factors / seek_step / set_seek_step are in warp/warp-transport.cpp. */

/* warp_set_consumption_reading + warp_get_consumption + warp_get_status (+ the
 * fmt_bytes helper and g_consumption_reading flag) are in core/dse-status.cpp. */

/* -------------------------------------------------------------------------- */
/* Registration                                                                */
/* -------------------------------------------------------------------------- */

void register_delayed_source()
{
	struct obs_source_info info = {};
	info.id = "delayed_source_engine";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO |
			    OBS_SOURCE_CUSTOM_DRAW;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = dse_get_name;
	info.create = dse_create;
	info.destroy = dse_destroy;
	info.get_width = dse_get_width;
	info.get_height = dse_get_height;
	info.get_defaults = dse_get_defaults;
	info.get_properties = dse_get_properties;
	info.update = dse_update;
	info.video_render = dse_video_render;
	info.video_tick = dse_video_tick;
	obs_register_source(&info);
}
