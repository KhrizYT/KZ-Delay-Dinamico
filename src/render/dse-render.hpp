/*
 * Broadcast Delay - source video render (the on-screen output).
 *
 * Draws the delayed output every frame: samples the VRAM/RAM/disk buffer at the
 * playhead, the Live<->Delay crossfade, and the pause / buffer-fill countdown
 * over the hold scene. Wired into the obs_source_info in dse-source.cpp.
 */
#pragma once

struct gs_effect;
typedef struct gs_effect gs_effect_t;

void dse_video_render(void *data, gs_effect_t *effect);
