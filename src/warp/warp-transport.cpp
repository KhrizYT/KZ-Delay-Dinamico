/*
 * Broadcast Delay - time-warp transport (the dock's playback control).
 *
 * The dock-facing playback API: jump Live/Delay (with a crossfade), pause, seek
 * (paused or live), play at the current distance, set speed and the accel/decel
 * factors. Each applies to every Docks-mode instance in the registry by setting
 * its playhead/state atomics; the render-thread state machine (dse_video_tick)
 * acts on them. Declared in warp-control.hpp.
 */
#include "../core/dse-internal.hpp"
#include "core/dse-constants.hpp"
#include "warp/warp-control.hpp"

#include <obs-module.h>
#include <util/platform.h>

void warp_set_state(int state)
{
	/* Crossfade duration from the OBS-selected transition (fade-style).
	 * Read the UI-thread-cached value (this runs from UI/hotkey/ws threads). */
	int trans_dur = g_trans_dur_ms.load(std::memory_order_relaxed);
	if (trans_dur <= 0)
		trans_dur = 300;

	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry) {
		if (!is_transport_controlled(s))
			continue;
		/* A direct jump into Live or Delay (from a different state) is
		 * masked by a crossfade over the OBS transition duration. */
		const int prev = s->warp_state.load(std::memory_order_relaxed);
		if ((state == WARP_LIVE || state == WARP_DELAYED) &&
		    prev != state) {
			s->warp_trans_dur.store((int64_t)trans_dur * 1000000LL,
						std::memory_order_relaxed);
			s->warp_trans_arm.store(true, std::memory_order_relaxed);
		}
		s->warp_state.store(state, std::memory_order_relaxed);
		int64_t snap = -1;
		double spd = 1.0;
		const int64_t target =
			(int64_t)s->delay_ns.load(std::memory_order_relaxed);
		switch (state) {
		case WARP_LIVE:
			snap = 50000000LL;
			spd = 1.0;
			break;
		case WARP_DELAYED:
			snap = target;
			spd = 1.0;
			break;
		case WARP_ACCEL:
			snap = -1;
			spd = s->accel_factor.load(std::memory_order_relaxed);
			break;
		case WARP_DECEL:
			snap = -1;
			spd = s->decel_factor.load(std::memory_order_relaxed);
			break;
		case WARP_PAUSED:
			snap = -1;
			spd = 0.0;
			break;
		case WARP_PLAY:
			snap = target;
			spd = 1.0;
			break;
		}
		s->audio_snap_ns.store(snap, std::memory_order_relaxed);
		s->audio_speed.store(spd, std::memory_order_relaxed);
		/* Jump playhead immediately so no live frame flashes. */
		if (snap >= 0) {
			const int64_t now = (int64_t)os_gettime_ns();
			s->playhead_ns.store(now - snap,
					     std::memory_order_relaxed);
		}
	}
}

void warp_toggle_pause()
{
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry) {
		if (!is_transport_controlled(s))
			continue;
		const int st = s->warp_state.load(std::memory_order_relaxed);
		const int nst = (st == WARP_PAUSED) ? WARP_DELAYED : WARP_PAUSED;
		s->warp_state.store(nst, std::memory_order_relaxed);
		s->audio_speed.store(nst == WARP_PAUSED ? 0.0 : 1.0,
				     std::memory_order_relaxed);
		s->audio_snap_ns.store(
			nst == WARP_DELAYED
				? (int64_t)s->delay_ns.load(std::memory_order_relaxed)
				: (int64_t)-1,
			std::memory_order_relaxed);
		if (nst == WARP_DELAYED) {
			const int64_t now = (int64_t)os_gettime_ns();
			s->playhead_ns.store(
				now - (int64_t)s->delay_ns.load(
					      std::memory_order_relaxed),
				std::memory_order_relaxed);
		}
	}
}

void warp_seek(int64_t delta_ns)
{
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry) {
		if (!is_transport_controlled(s))
			continue;
		s->warp_state.store(WARP_PAUSED, std::memory_order_relaxed);
		s->audio_snap_ns.store(-1, std::memory_order_relaxed);
		s->audio_speed.store(0.0, std::memory_order_relaxed);
		s->seek_pending_ns.fetch_add(delta_ns, std::memory_order_relaxed);
	}
}

/* Like warp_seek, but KEEPS PLAYING at 1x from the new position (WARP_PLAY)
 * instead of pausing - i.e. adjusts the live delay on the fly. delta>0 moves
 * towards live (smaller distance). Clamped to the available buffer. */
void warp_seek_live(int64_t delta_ns)
{
	const int64_t now = (int64_t)os_gettime_ns();
	const double fps = active_fps() > 0 ? active_fps() : 60.0;
	const int64_t MIN_LIVE = dse::kMinLiveNs; /* "at live" floor */
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry) {
		if (!is_transport_controlled(s))
			continue;
		/* Available content = frames actually captured (real fill), capped
		 * at the configured delay - you can't scrub past what's buffered. */
		const int64_t delay = s->delay_ns.load(std::memory_order_relaxed);
		int64_t avail = (int64_t)((double)s->frames_buffered.load(
						  std::memory_order_relaxed) /
					  fps * 1.0e9);
		if (delay > 0 && avail > delay)
			avail = delay;

		/* Current distance: use the logical edge for Live/edge states so
		 * coming back into the buffer is immediate (no dead zone). */
		const int prev = s->warp_state.load(std::memory_order_relaxed);
		int64_t cur_d = (prev == WARP_LIVE)
					? MIN_LIVE
					: (prev == WARP_DELAYED
						   ? avail
						   : now - s->playhead_ns.load(
								   std::memory_order_relaxed));
		int64_t d = cur_d - delta_ns; /* new distance from live */
		if (d < 0)
			d = 0;

		if (d <= MIN_LIVE) {
			/* Reached the front -> go fully Live. */
			s->warp_state.store(WARP_LIVE, std::memory_order_relaxed);
			s->audio_snap_ns.store(MIN_LIVE, std::memory_order_relaxed);
			s->audio_speed.store(1.0, std::memory_order_relaxed);
		} else if (d >= avail) {
			/* At/past the filling edge -> show the countdown (DELAYED)
			 * clamped to the oldest buffered frame; can't go further. */
			s->warp_state.store(WARP_DELAYED, std::memory_order_relaxed);
			s->audio_snap_ns.store(avail > 0 ? avail : -1,
					       std::memory_order_relaxed);
			s->audio_speed.store(1.0, std::memory_order_relaxed);
		} else {
			/* Scrub within the buffered zone, still playing at 1x. */
			s->play_distance_ns.store(d, std::memory_order_relaxed);
			s->warp_state.store(WARP_PLAY, std::memory_order_relaxed);
			s->audio_snap_ns.store(d, std::memory_order_relaxed);
			s->audio_speed.store(1.0, std::memory_order_relaxed);
		}
	}
}

void warp_play_current()
{
	const int64_t now = (int64_t)os_gettime_ns();
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry) {
		if (!is_transport_controlled(s))
			continue;
		const int64_t ph = s->playhead_ns.load(std::memory_order_relaxed);
		int64_t d = now - ph;
		if (d < 0)
			d = 0;
		const int64_t target =
			(int64_t)s->delay_ns.load(std::memory_order_relaxed);
		s->play_distance_ns.store(d, std::memory_order_relaxed);
		s->warp_state.store(WARP_PLAY, std::memory_order_relaxed);
		s->audio_snap_ns.store(target, std::memory_order_relaxed);
		s->audio_speed.store(s->play_speed.load(std::memory_order_relaxed),
				     std::memory_order_relaxed);
	}
}

/* Set the playback speed (0.5/1/2x) and start playing at the current position. */
void warp_set_play_speed(double speed)
{
	if (speed < 0.1)
		speed = 0.1;
	const int64_t now = (int64_t)os_gettime_ns();
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry) {
		if (!is_transport_controlled(s))
			continue;
		s->play_speed.store(speed, std::memory_order_relaxed);
		const int64_t ph = s->playhead_ns.load(std::memory_order_relaxed);
		int64_t d = now - ph;
		if (d < 50000000LL)
			d = 50000000LL;
		s->play_distance_ns.store(d, std::memory_order_relaxed);
		s->warp_state.store(WARP_PLAY, std::memory_order_relaxed);
		s->audio_speed.store(speed, std::memory_order_relaxed);
	}
}

void warp_set_factors(double accel, double decel)
{
	if (accel < 1.05)
		accel = 1.05;
	if (decel > 0.95)
		decel = 0.95;
	if (decel < 0.05)
		decel = 0.05;
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry)
		if (is_transport_controlled(s)) {
			s->accel_factor.store(accel, std::memory_order_relaxed);
			s->decel_factor.store(decel, std::memory_order_relaxed);
		}
}

void warp_seek_step(int dir)
{
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry) {
		if (!is_transport_controlled(s))
			continue;
		const int64_t step = s->seek_step_ns.load(std::memory_order_relaxed);
		s->warp_state.store(WARP_PAUSED, std::memory_order_relaxed);
		s->audio_snap_ns.store(-1, std::memory_order_relaxed);
		s->audio_speed.store(0.0, std::memory_order_relaxed);
		s->seek_pending_ns.fetch_add(dir < 0 ? -step : step,
					     std::memory_order_relaxed);
	}
}

void warp_set_seek_step(double seconds)
{
	if (seconds < 1.0)
		seconds = 1.0;
	if (seconds > 120.0)
		seconds = 120.0;
	const int64_t ns = (int64_t)(seconds * 1.0e9);
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry)
		if (is_transport_controlled(s))
			s->seek_step_ns.store(ns, std::memory_order_relaxed);
}
