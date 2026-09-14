/*
 * Broadcast Delay - capture engine (target + buffers + the capture/tick path).
 *
 * The pre-fill pipeline: resolve + attach the delayed target, (re)allocate the
 * GPU / RAM / disk buffers, capture every frame off the OBS render thread, run
 * the time-warp tick state machine, and drive AI detection. Split across
 * dse-buffers.cpp (target + buffer setup) and dse-capture.cpp (capture_frame +
 * main_render_cb + dse_video_tick). The lifecycle in dse-source.cpp calls
 * the entry points declared here.
 */
#pragma once

#include <cstddef>
#include <cstdint>

struct DelayedSource;
struct obs_data;
typedef struct obs_data obs_data_t;

/* Target attach/detach (the _locked ones require target_mutex held). */
void detach_target_locked(DelayedSource *s);
void attach_target_locked(DelayedSource *s);
void set_target(DelayedSource *s, int mode, const char *name);

/* Capacity / RAM budget helpers. */
size_t required_capacity(uint64_t delay_ns, double fps);
uint64_t auto_ram_budget();
uint64_t safe_ram_limit();

/* GPU + disk buffer (re)allocation (graphics thread). */
void ensure_gpu_buffers(DelayedSource *s, uint32_t cx, uint32_t cy);
void free_video_buffers(DelayedSource *s);
void stop_disk(DelayedSource *s);
void ensure_disk(DelayedSource *s, uint32_t cx, uint32_t cy, uint64_t delay_ns);

/* Per-frame capture (off the render thread) + the time-warp tick state machine.
 * Registered by the lifecycle (obs_add_main_render_callback / obs_source_info). */
void main_render_cb(void *data, uint32_t cx, uint32_t cy);
void dse_video_tick(void *data, float seconds);
