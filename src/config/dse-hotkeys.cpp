/*
 * Broadcast Delay - per-source hotkeys (see dse-hotkeys.hpp).
 */
#include "dse-hotkeys.hpp"

#include "../core/dse-internal.hpp"
#include "warp/warp-control.hpp"

#include <obs-module.h>

static void toggle_hotkey_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *s = static_cast<DelayedSource *>(data);
	const bool now = !s->enabled.load(std::memory_order_relaxed);
	s->enabled.store(now, std::memory_order_relaxed);

	/* Persist + reflect in the UI checkbox. */
	obs_data_t *settings = obs_source_get_settings(s->self);
	obs_data_set_bool(settings, "enabled", now);
	obs_data_release(settings);

	blog(LOG_INFO, "[broadcast-delay] hotkey: %s", now ? "enabled" : "disabled");
}

/* Time-warp control hotkeys (bindable in Settings -> Hotkeys). */
static void hk_set_state(void *data, bool pressed, int state)
{
	if (!pressed)
		return;
	static_cast<DelayedSource *>(data)->warp_state.store(
		state, std::memory_order_relaxed);
}
static void hk_live_cb(void *d, obs_hotkey_id, obs_hotkey_t *, bool p)
{
	hk_set_state(d, p, WARP_LIVE);
}
static void hk_delay_cb(void *d, obs_hotkey_id, obs_hotkey_t *, bool p)
{
	hk_set_state(d, p, WARP_DELAYED);
}
static void hk_accel_cb(void *d, obs_hotkey_id, obs_hotkey_t *, bool p)
{
	hk_set_state(d, p, WARP_ACCEL);
}
static void hk_decel_cb(void *d, obs_hotkey_id, obs_hotkey_t *, bool p)
{
	hk_set_state(d, p, WARP_DECEL);
}
static void hk_pause_cb(void *d, obs_hotkey_id, obs_hotkey_t *, bool p)
{
	if (!p)
		return;
	auto *s = static_cast<DelayedSource *>(d);
	const int st = s->warp_state.load(std::memory_order_relaxed);
	s->warp_state.store(st == WARP_PAUSED ? WARP_DELAYED : WARP_PAUSED,
			    std::memory_order_relaxed);
}
static void hk_seek(void *d, bool pressed, int64_t delta_ns)
{
	if (!pressed)
		return;
	auto *s = static_cast<DelayedSource *>(d);
	s->warp_state.store(WARP_PAUSED, std::memory_order_relaxed);
	s->seek_pending_ns.fetch_add(delta_ns, std::memory_order_relaxed);
}
static void hk_back5_cb(void *d, obs_hotkey_id, obs_hotkey_t *, bool p)
{
	hk_seek(d, p, -5LL * 1000000000LL);
}
static void hk_fwd5_cb(void *d, obs_hotkey_id, obs_hotkey_t *, bool p)
{
	hk_seek(d, p, 5LL * 1000000000LL);
}

/* Toggle "redirect all scene changes" (global setting). */
static void hk_redirect_cb(void *, obs_hotkey_id, obs_hotkey_t *, bool p)
{
	if (!p) return;
	warp_set_redirect_scene(!warp_get_redirect_scene());
}

void register_dse_hotkeys(DelayedSource *s)
{
	obs_source_t *source = s->self;
	s->toggle_hotkey = obs_hotkey_register_source(
		source, "delayed_source.toggle",
		obs_module_text("ToggleHotkey"), toggle_hotkey_cb, s);
	s->hk_live = obs_hotkey_register_source(source, "delayed_source.live",
						obs_module_text("Hk.Live"),
						hk_live_cb, s);
	s->hk_delay = obs_hotkey_register_source(source, "delayed_source.delay",
						 obs_module_text("Hk.Delay"),
						 hk_delay_cb, s);
	s->hk_accel = obs_hotkey_register_source(source, "delayed_source.accel",
						 obs_module_text("Hk.Accel"),
						 hk_accel_cb, s);
	s->hk_decel = obs_hotkey_register_source(source, "delayed_source.decel",
						 obs_module_text("Hk.Decel"),
						 hk_decel_cb, s);
	s->hk_pause = obs_hotkey_register_source(source, "delayed_source.pause",
						 obs_module_text("Hk.Pause"),
						 hk_pause_cb, s);
	s->hk_back5 = obs_hotkey_register_source(source, "delayed_source.back5",
						 obs_module_text("Hk.Back5"),
						 hk_back5_cb, s);
	s->hk_fwd5 = obs_hotkey_register_source(source, "delayed_source.fwd5",
						obs_module_text("Hk.Fwd5"),
						hk_fwd5_cb, s);
	s->hk_redirect = obs_hotkey_register_source(source, "delayed_source.redirect",
						     obs_module_text("Hk.RedirectScene"),
						     hk_redirect_cb, s);
}

void unregister_dse_hotkeys(DelayedSource *s)
{
	for (obs_hotkey_id hk :
	     {s->toggle_hotkey, s->hk_live, s->hk_delay, s->hk_accel,
	      s->hk_decel, s->hk_pause, s->hk_back5, s->hk_fwd5,
	      s->hk_redirect}) {
		if (hk != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hk);
	}
}
