/*
 * Broadcast Delay - audio capture + delayed mix/emit.
 *
 * Owns the per-source audio path: the OBS audio-capture callback, the direct
 * WASAPI capture/mix/emit (Windows), and (re)building the captured source set.
 * The delayed mixer + WSOLA time-stretch live in audio-ring.hpp; this is the
 * glue that feeds them and emits the delayed result on the source. Target
 * management in dse-source.cpp calls gather/detach; the tick calls
 * reconcile_hw.
 */
#pragma once

struct DelayedSource;

/* (Re)build the captured audio set / drop it. Caller holds target_mutex. */
void gather_audio_locked(DelayedSource *s);
void detach_audio_locked(DelayedSource *s);

#ifdef _WIN32
/* Start/stop WASAPI device captures from the mixer-dock selection. */
void reconcile_hw(DelayedSource *s);
#endif
