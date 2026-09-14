/*
 * Broadcast Delay - public C API
 * ==============================
 *
 * A stable C contract other OBS plugins can use to discover Broadcast Delay
 * instances and read their live state (delay, buffer depth, playhead, ...).
 * It lets a SEPARATE plugin (e.g. a standalone censure plugin) detect whether
 * Broadcast Delay is loaded and, if so, light up extra features - without
 * forking or hard-linking against this plugin.
 *
 * VERSIONING: BD_API_VERSION (this header) + bd_api_version() (runtime) form the
 * handshake. The contract is additive-only - new functions/fields are appended
 * and existing ones never change meaning, so a higher runtime version stays
 * backward-compatible. The major version is bumped ONLY on a breaking change; a
 * consumer can compare bd_api_version() against the BD_API_VERSION it built with.
 *
 * HOW TO CONSUME (from another plugin)
 * ------------------------------------
 * Broadcast Delay exports these symbols from its module DLL/so. A consumer
 * resolves them at runtime so it degrades gracefully when Broadcast Delay is
 * absent (auto-detect): keep your own function pointers, bind them with
 * os_dlsym() on the loaded module, and treat "symbol not found" as
 * "Broadcast Delay not installed".
 *
 *   typedef int (*bd_is_available_fn)(void);
 *   bd_is_available_fn avail = (bd_is_available_fn)os_dlsym(mod, "bd_is_available");
 *   bool boosted = avail && avail();
 *
 * THREADING: all functions here are safe to call from any thread (they lock the
 * instance registry internally and only read atomics). Handles are validated
 * against the live registry on every call, so a stale handle returns 0 rather
 * than dereferencing freed memory.
 */
#ifndef BROADCAST_DELAY_API_H
#define BROADCAST_DELAY_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define BD_EXPORT __declspec(dllexport)
#else
#define BD_EXPORT __attribute__((visibility("default")))
#endif

/* Frozen API major version. Bumped only on a breaking change (never for additive
 * functions/fields). Compare against the runtime bd_api_version() below. */
#define BD_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

struct obs_source;
typedef struct obs_source obs_source_t;

/* Opaque handle to one Broadcast Delay instance (a DelayedSource). Only valid
 * while that source exists; always re-validated by the API on use. */
typedef void *bd_handle_t;

/* Storage backend (matches the source's "storage" setting). */
enum {
	BD_STORAGE_RAM = 0,
	BD_STORAGE_DISK = 1,
	BD_STORAGE_VRAM = 2,
};

/* Time-warp transport state (matches the dock). */
enum {
	BD_WARP_LIVE = 0,
	BD_WARP_DELAYED = 1,
	BD_WARP_ACCEL = 2,
	BD_WARP_DECEL = 3,
	BD_WARP_PAUSED = 4,
	BD_WARP_PLAY = 5,
};

/* A snapshot of one instance's live state. Treat the timeline as: the buffer
 * holds [now - buffered_ns .. now]; the program output is read at the playhead
 * (distance_behind_live_ns behind now). A future "tap" can read any point in
 * that window. */
typedef struct bd_instance_info {
	char source_name[256];
	uint64_t delay_ns;             /* configured target delay */
	uint64_t buffered_ns;          /* real buffer depth available */
	uint64_t playhead_capture_ts;  /* capture timestamp shown on program */
	int64_t distance_behind_live_ns; /* now - playhead (>=0) */
	int warp_state;                /* BD_WARP_* */
	int storage;                   /* BD_STORAGE_* */
	uint32_t width, height;
	double fps;
	uint64_t underflows;
	int disk_error;                /* 1 if the disk backend failed to start */
} bd_instance_info_t;

/* True (non-zero) if Broadcast Delay is present. The mere presence of this
 * symbol is the detection primitive; it always returns 1 when callable. */
BD_EXPORT int bd_is_available(void);

/* Runtime API major version (== BD_API_VERSION the DLL was built with). A
 * consumer can os_dlsym this and check compatibility; if the symbol is absent,
 * treat the API as version 1 (it predates the handshake). */
BD_EXPORT int bd_api_version(void);

/* Fill `out` (up to `cap` entries) with handles to every live instance and
 * return the TOTAL count (which may exceed `cap` - re-call with a bigger
 * buffer). Pass out=NULL, cap=0 to just count. */
BD_EXPORT size_t bd_list_instances(bd_handle_t *out, size_t cap);

/* Handle for the Broadcast Delay source `src`, or NULL if it is not one. */
BD_EXPORT bd_handle_t bd_from_obs_source(obs_source_t *src);

/* Read a live snapshot. Returns 1 on success, 0 if the handle is stale/invalid
 * or info is NULL. */
BD_EXPORT int bd_get_info(bd_handle_t h, bd_instance_info_t *info);

/* ===========================================================================
 * Frame taps - read the shared buffer timeline at a chosen offset from live.
 * ---------------------------------------------------------------------------
 * The delay buffer is a timeline [now - buffered_ns .. now]. A tap is a second
 * read point on it: 0 = live edge, the playhead = program (what air sees), or
 * any fixed offset (e.g. 30 s inside a 60 s delay). The engine samples the ring
 * (sample(timestamp)) each captured frame and calls your callback.
 *
 * v1 = VIDEO, RGBA, RAM storage. The callback runs on the OBS GRAPHICS thread;
 * the rgba pointer is valid only for the duration of the call (copy if needed).
 * (Audio taps + GPU-texture / disk / VRAM frames are additive next steps; the
 * on_audio field already exists in the contract.)
 */
typedef enum {
	BD_TAP_LIVE = 0,    /* live edge (newest captured frame) */
	BD_TAP_PROGRAM = 1, /* the program playhead (what air sees) */
	BD_TAP_OFFSET = 2,  /* a fixed offset_ns behind live */
} bd_tap_mode_t;

typedef struct bd_video_frame {
	const uint8_t *rgba;          /* tightly comparable: linesize = width*4 */
	uint32_t width, height, linesize;
	uint64_t capture_ts_ns;       /* this frame's capture timestamp */
	int64_t offset_from_live_ns;  /* now - capture_ts (>=0) */
} bd_video_frame_t;

typedef struct bd_audio_block {
	const float *const *planar;
	uint32_t frames, channels, rate;
	uint64_t timestamp_ns;
	int64_t offset_from_live_ns;
} bd_audio_block_t;

typedef struct bd_tap {
	const char *extension_id;     /* unique, e.g. "com.myext.recorder" */
	int mode;                     /* bd_tap_mode_t */
	int64_t offset_ns;            /* used when mode == BD_TAP_OFFSET */
	void (*on_video)(void *ctx, bd_handle_t h, const bd_video_frame_t *f);
	void (*on_audio)(void *ctx, bd_handle_t h, const bd_audio_block_t *a);
	void *ctx;
} bd_tap_t;

/* Register/replace a tap on instance `h` (keyed by extension_id). Returns 1 on
 * success, 0 if the handle is stale or tap/extension_id is NULL. */
BD_EXPORT int bd_register_tap(bd_handle_t h, const bd_tap_t *tap);

/* Remove the tap with this extension_id from instance `h`. */
BD_EXPORT void bd_unregister_tap(bd_handle_t h, const char *extension_id);

/* Update a registered tap's offset (for BD_TAP_OFFSET). Returns 1 on success. */
BD_EXPORT int bd_set_tap_offset(bd_handle_t h, const char *extension_id,
				int64_t offset_ns);

#ifdef __cplusplus
}
#endif

#endif /* BROADCAST_DELAY_API_H */
