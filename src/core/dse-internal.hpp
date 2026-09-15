/*
 * Broadcast Delay - shared internal definitions.
 *
 * The DelayedSource instance struct (and its DiskArgs helper) that every engine
 * module operates on. It lives here, at namespace scope, so the source can be
 * split across translation units (capture / render / warp / audio / config) that
 * all share one definition instead of one 4000-line file.
 *
 * This is the engine's PRIVATE header (not a public API). The file-scope globals
 * (g_registry, g_ai_*, g_dock_*) are promoted to `extern` here only as the
 * modules that need them are split out.
 */
#pragma once

#include <obs-module.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio-ring.hpp"
#include "delay/disk-buffer.hpp"
#include "audio/mixer-state.hpp"
#include "delay/video-ring.hpp"
#include "delay/vram-ring.hpp"
#include "audio/device-capture.hpp"

/* Capture mode (settings "mode" value). */
enum { MODE_SOURCE = 0, MODE_SCENE = 1, MODE_DOCKS = 2, MODE_CANVAS_SCENE = 3 };
/* Video delay storage backend (settings "storage" value). */
enum { STORAGE_RAM = 0, STORAGE_DISK = 1, STORAGE_VRAM = 2 };

/* Shared engine helpers (tiny + pure -> inline so every module shares one
 * definition without a link dependency). */

/* Default disk-buffer scratch folder under the OS temp dir. */
inline std::string default_temp_folder()
{
	const char *tmp = getenv("TEMP");
	if (!tmp)
		tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/tmp";
	return std::string(tmp) + "\\BroadcastDelay";
}

/* Overrides from advanced options (DiskArgs). */
struct DiskArgs {
	std::string folder;
	int codec = -1;    /* 0 raw, 1 png, 2 mjpeg */
	int quality = -1;  /* mjpeg q */
	int seconds = -1;  /* disk buffer length override */
	double ram_gb = -1.0;
};

struct DelayedSource {
	obs_source_t *self = nullptr;

	/* Target (the source/scene/program we delay). Guarded by target_mutex. */
	std::mutex target_mutex;
	obs_source_t *target = nullptr; /* strong ref, or null */
	int mode_val = 0;               /* 0 Source, 1 Scene, 2 Docks */
	std::atomic<int> mode_atomic{0};
	std::string target_name;
	std::string target_key;      /* mode+name, to detect changes */
	bool showing_target = false; /* did we inc_showing on target? */

	/* Audio sources we captured + delay (under target_mutex). Scene
	 * pull in the global audio devices; Source uses just the target. */
	std::vector<obs_source_t *> audio_sources;
	/* Manual audio-source selection (under target_mutex). When audio_auto is
	 * false, capture exactly audio_sel by name instead of the auto gather --
	 * lets the user exclude the desktop loopback that feeds back when the
	 * delayed output is monitored. */
	bool audio_auto = false;
	std::vector<std::string> audio_sel;
	std::string audio_sel_key; /* detects selection changes */

	obs_hotkey_id toggle_hotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_live = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_delay = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_accel = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_decel = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_pause = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_back5 = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_fwd5 = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_redirect = OBS_INVALID_HOTKEY_ID;

	/* Config */
	std::atomic<bool> enabled{true};
	std::atomic<uint64_t> delay_ns{0};
	std::atomic<uint64_t> budget_bytes{8ULL << 30}; /* RAM cap, default 8 GiB */
	std::atomic<bool> show_countdown{true};

	/* Time-warp playhead + state machine (video thread). playhead_ns is the
	 * capture-time currently shown; distance-behind-live = now - playhead. */
	std::atomic<int> warp_state{0}; /* WARP_LIVE by default */
	std::atomic<int64_t> playhead_ns{0};
	std::atomic<int64_t> seek_pending_ns{0};   /* +/- jumps to apply */
	std::atomic<int64_t> play_distance_ns{0};  /* held distance in WARP_PLAY */
	std::atomic<double> play_speed{1.0};       /* WARP_PLAY rate: 0.5/1/2x */
	std::atomic<double> audio_speed{1.0};      /* playback speed for audio */
	std::atomic<int64_t> audio_snap_ns{0};     /* >=0 snap audio lag; -1 free */
	std::atomic<int64_t> buffered_ns{0};       /* actual buffer depth */
	std::atomic<int64_t> last_now_ns{0};       /* clock at last tick (status) */
	std::atomic<double> accel_factor{2.0};
	std::atomic<double> decel_factor{0.5};
	std::atomic<int64_t> seek_step_ns{5LL * 1000000000LL};
	bool warp_inited = false;

	/* Storage: 0 = RAM (default), 1 = Disk. Disk settings are read by the
	 * disk backend. */
	std::atomic<bool> advanced{false};
	std::atomic<int> storage{0};
	std::atomic<int> audio_storage{0}; /* 0=RAM, 1=Disk/AAC */
	std::atomic<int> disk_compression{0}; /* 0 raw, 1 png, 2 mjpeg, 3 hls */
	std::atomic<bool> disk_auto_q{false}; /* auto mode: pick HLS bitrate from resolution */
	std::mutex disk_cfg_mutex;
	DiskArgs disk_args; /* parsed --flags (guarded by disk_cfg_mutex) */

	/* Video (graphics thread only) */
	VideoRing ring;
	VramRing vram;                       /* GPU texture ring (storage=VRAM) */
	gs_texture_t *last_vram = nullptr;   /* freeze frame for VRAM underflow */
	/* Live<->Delay crossfade: a frozen snapshot of the frame shown just
	 * before the jump, faded out over the OBS transition duration. */
	gs_texture_t *warp_from_tex = nullptr;
	uint32_t warp_from_w = 0, warp_from_h = 0;
	std::atomic<bool> warp_trans_arm{false};   /* set by warp_set_state */
	std::atomic<int64_t> warp_trans_start{0};  /* 0 = inactive */
	std::atomic<int64_t> warp_trans_dur{0};
	size_t vram_ceiling = SIZE_MAX;      /* clamp if VRAM alloc fell short */
	gs_texrender_t *texrender = nullptr;
	gs_texrender_t *blur_texrender = nullptr; /* countdown number scratch */
	gs_stagesurf_t *stage[2] = {nullptr, nullptr}; /* GPU->RAM readback */
	uint64_t stage_ts[2] = {0, 0};
	int stage_cur = 0;
	bool stage_primed = false;
	gs_texture_t *upload_tex = nullptr;  /* RAM->GPU for playback */
	gs_texture_t *upload_tex2 = nullptr; /* 2nd frame for interpolation */
	bool has_uploaded = false;
	uint32_t buf_cx = 0;
	uint32_t buf_cy = 0;
	size_t cap_ceiling = SIZE_MAX; /* clamp if RAM alloc fell short */

	/* Disk backend (graphics thread only). */
	std::unique_ptr<DiskBuffer> disk;
	uint32_t disk_cx = 0, disk_cy = 0;
	int disk_fps = 0, disk_q = -1, disk_codec = -1;
	size_t disk_cap = 0;
	std::string disk_started_folder;
	std::atomic<bool> disk_failed{false}; /* disk buffer create failed */
	std::atomic<uint64_t> disk_queue{0};   /* writer queue depth (dock) */
	std::atomic<uint64_t> disk_dropped{0}; /* frames dropped on overflow */
	std::vector<uint8_t> disk_play_buf;
	uint64_t disk_last_upload_ts = 0; /* skip re-uploading an unchanged frame */
	const uint8_t *ram_last_a = nullptr; /* RAM: last uploaded frame (skip dup) */
	const uint8_t *ram_last_b = nullptr;
	float ram_last_frac = -1.f;
	std::atomic<uint32_t> cx{0};
	std::atomic<uint32_t> cy{0};
	std::atomic<bool> rendering{false}; /* recursion guard */

	/* Audio (audio thread, under audio_mutex) */
	std::mutex audio_mutex;
	AudioMixer mixer;
	std::vector<std::vector<float>> audio_out;
	/* The one source that drives emission; others only feed the mix. */
	std::atomic<obs_source_t *> audio_clock{nullptr};
	/* V2.5: captura la mezcla FINAL de una pista de OBS en vez de reconstruir
	 * el audio fuente por fuente. Esto conserva filtros, faders y calidad nativa. */
	bool program_mix_audio = false;
	size_t program_mix_idx = 0;
	bool program_mix_attached = false;#ifdef _WIN32
	/* WASAPI capture (direct Windows audio): one capture per non-muted
	 * device. Acquisition is automatic; muting a device stops its capture.
	 * All devices feed the same mix; ONE device emits the summed output (the
	 * clock). The clock is claimed dynamically at runtime by whichever device
	 * actually delivers audio, and re-claimed if the owner goes idle - so a
	 * silent/idle device can never stall emission. */
	struct HwCap {
		std::string id;
		std::string name;
		int uid = 0;
		std::unique_ptr<DeviceCapture> cap;
	};
	std::vector<HwCap> hw_caps;
	std::atomic<bool> hw_active{false};
	std::string hw_sel_key;       /* captured device-id set, to detect changes */
	std::atomic<int> hw_clock_owner{-1};   /* uid currently driving emission */
	std::atomic<uint64_t> hw_clock_seen{0}; /* last ns the owner emitted */
#endif
	/* This source's own native-audio mixer (per-source, not global). */
	std::shared_ptr<MixerState> mixer_state = std::make_shared<MixerState>();

	/* Stats (for logging / debug UI) */
	std::atomic<size_t> frames_buffered{0};
	std::atomic<uint64_t> underflows{0};
	std::atomic<uint64_t> used_bytes{0}; /* current RAM use, for the gauge */
};

/* ---- shared engine spine (defined in core/dse-globals.cpp) ---- */

/* Registry of live instances, so the Qt dock can drive them all. */
extern std::mutex g_reg_mutex;
extern std::vector<DelayedSource *> g_registry;
/* Cached OBS transition duration (ms), refreshed from the UI thread so the
 * Live<->Delay crossfade can read it from hotkey/websocket threads safely. */
extern std::atomic<int> g_trans_dur_ms;

/* Dock scene pick shared with the render module (guarded by g_dock_mutex):
 * the hold scene shown while paused. */
extern std::mutex g_dock_mutex;
extern std::string g_pause_scene;   /* "" = black + countdown */

/* The dock controls only Docks-mode sources (Source/Scene modes are manual). */
inline bool is_docks(DelayedSource *s)
{
	return s->mode_atomic.load(std::memory_order_relaxed) == MODE_DOCKS;
}

/* Docks-mode sources and Aitum/secondary-canvas scene companions share the
 * transport controls (Live / Delay / Pause / seek / x0.5-x2).  Canvas-scene
 * sources keep a fixed target, but move through their buffer in lock-step with
 * the normal Docks source. */
inline bool is_transport_controlled(DelayedSource *s)
{
	const int mode = s->mode_atomic.load(std::memory_order_relaxed);
	return mode == MODE_DOCKS || mode == MODE_CANVAS_SCENE;
}

/* Active OBS output frame rate (falls back to 60). */
inline double active_fps()
{
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_den)
		return (double)ovi.fps_num / (double)ovi.fps_den;
	return 60.0;
}

/* Dispatch the public-API video taps registered on this source (core/bd-api.cpp).
 * Called from capture on the graphics thread; a cheap no-op when none. */
void bd_dispatch_video_taps(DelayedSource *s);
/* Feed LIVE taps the freshly captured RAM frame (works for every storage
 * backend). Called from capture's readback path while the frame is mapped. */
void bd_dispatch_live_frame(DelayedSource *s, const uint8_t *rgba, uint32_t cx,
			    uint32_t cy, uint32_t linesize, uint64_t ts);
/* Feed LIVE audio taps the freshly captured (pre-delay) audio block. Lets a
 * consumer (e.g. Broadcast Censure) transcribe exactly what BD is buffering,
 * ahead of air. Called from the audio capture path. */
void bd_dispatch_live_audio(DelayedSource *s, const float *const *planar,
			    uint32_t frames, uint32_t channels, uint32_t rate,
			    uint64_t ts);
/* Drop all API taps for a source being destroyed (called from dse_destroy). */
void bd_forget_source(DelayedSource *s);
