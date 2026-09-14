/*
 * Broadcast Delay - dock scene + shared transition management.
 *
 * The dock's live-scene pick: a private clone of OBS's current transition wrapped
 * in a hidden scene, driven so the delayed feed + preview show the real scene
 * transition (fade/stinger). Plus the "redirect all scene changes" toggle and
 * the pause hold-scene name. The capture renders the hidden scene via
 * warp_acquire_dock_render(). Entry points declared in warp-control.hpp.
 */
#include "../capture/dse-capture.hpp" /* set_target */
#include "../core/dse-internal.hpp"
#include "warp-control.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <cstring>
#include <mutex>
#include <string>

/* The live scene selected in the dock; drives every Docks-mode source. */
static std::string g_dock_scene;
static bool g_redirect_scene = true; /* redirect all scene changes to dock scene */
/* A private clone of OBS's current transition. The dock's scene picks drive it,
 * so both the delayed capture and the preview show the scene transition (fade,
 * stinger, ...) instead of a hard cut. Guarded by g_dock_mutex. */
static obs_source_t *g_dock_transition = nullptr;
/* Hidden scene wrapping the transition so OBS ticks it (fade/stinger work). */
static obs_source_t *g_hidden_scene = nullptr;
static obs_sceneitem_t *g_hidden_item = nullptr;
static bool g_dock_tr_primed = false; /* first scene = instant; later = animated */
/* Name of the transition currently cloned into g_dock_transition (the global one
 * or a per-scene override). Lets us detect when a scene wants a different one. */
static std::string g_dock_tr_name;

/* Find a configured frontend transition by name (for per-scene overrides), or
 * null. Returns a new ref the caller must release. UI thread only. */
static obs_source_t *find_frontend_transition(const char *name)
{
	if (!name || !*name)
		return nullptr;
	struct obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
	obs_source_t *found = nullptr;
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *t = list.sources.array[i];
		const char *n = obs_source_get_name(t);
		if (n && strcmp(n, name) == 0) {
			found = obs_source_get_ref(t);
			break;
		}
	}
	obs_frontend_source_list_free(&list);
	return found;
}

/* Tear down the cloned transition + its hidden wrapper scene. Caller holds
 * g_dock_mutex. */
static void teardown_dock_transition_locked()
{
	if (g_hidden_item) {
		obs_sceneitem_remove(g_hidden_item);
		g_hidden_item = nullptr;
	}
	if (g_hidden_scene) {
		obs_source_dec_active(g_hidden_scene);
		obs_source_release(g_hidden_scene);
		g_hidden_scene = nullptr;
	}
	if (g_dock_transition) {
		obs_source_release(g_dock_transition);
		g_dock_transition = nullptr;
	}
	g_dock_tr_name.clear();
}

/* Lazily clone a transition (same type + settings, including stingers) so the
 * dock can drive it. clone_from is a per-scene override transition, or null to
 * clone OBS's current frontend transition. Caller holds g_dock_mutex. Must run
 * on the UI thread (uses the frontend API). */
static void ensure_dock_transition_locked(obs_source_t *clone_from)
{
	if (g_dock_transition)
		return;

	obs_source_t *cur = clone_from ? obs_source_get_ref(clone_from)
				       : obs_frontend_get_current_transition();
	const char *id = cur ? obs_source_get_id(cur) : "fade_transition";
	const char *nm = cur ? obs_source_get_name(cur) : nullptr;
	g_dock_tr_name = nm ? nm : "";
	obs_data_t *st = cur ? obs_source_get_settings(cur) : nullptr;
	g_dock_transition = obs_source_create_private(
		id ? id : "fade_transition", "Delay Dock Transition", st);
	if (st) obs_data_release(st);
	if (cur) obs_source_release(cur);
	if (!g_dock_transition) return;

	/* Wrap the transition in a hidden scene so OBS ticks it. */
	obs_scene_t *sc = obs_scene_create_private("Delay Dock Scene");
	g_hidden_scene = obs_scene_get_source(sc);
	obs_source_inc_active(g_hidden_scene);

	/* Add the transition as a scene item (fills viewport naturally). */
	g_hidden_item = obs_scene_add(sc, g_dock_transition);
}

/* A ref to the source the dock should render (the hidden scene wrapping the
 * transition), or null. The capture and preview both render it. */
obs_source_t *warp_acquire_dock_render()
{
	std::lock_guard<std::mutex> lock(g_dock_mutex);
	return g_hidden_scene ? obs_source_get_ref(g_hidden_scene) : nullptr;
}

/* Release the dock transition (module unload). */
void warp_shutdown()
{
	std::lock_guard<std::mutex> lock(g_dock_mutex);
	teardown_dock_transition_locked();
}

/* Re-create the dock transition when OBS's transition type changes.
 * Called from the frontend event callback in plugin-main.cpp. */
void warp_refresh_transition()
{
	int dur = obs_frontend_get_transition_duration();
	if (dur > 0)
		g_trans_dur_ms.store(dur, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(g_dock_mutex);
	teardown_dock_transition_locked();
	g_dock_tr_primed = false;
	/* Will be re-created on next dock scene change via ensure_dock_transition_locked. */
}

/* True if any registered source is in Scene mode (so program scene changes
 * should drive the shared transition, like the dock does). */
bool warp_any_scene_mode()
{
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry)
		if (s->mode_atomic.load(std::memory_order_relaxed) == MODE_SCENE)
			return true;
	return false;
}

/* Dock picked a live scene: transition to it (so the delayed feed + preview show
 * the scene transition) and retarget every Docks-mode source. */
void warp_set_dock_scene(const char *name)
{
	/* Store previous scene (the crossfade's "from") and set the new one. */
	std::string prev_scene;
	{
		std::lock_guard<std::mutex> lock(g_dock_mutex);
		prev_scene = g_dock_scene;
		g_dock_scene = name ? name : "";
	}

	obs_source_t *to = (name && *name) ? obs_get_source_by_name(name) : nullptr;

	/* Per-scene transition override: if the target scene has one set (right-
	 * click scene -> Transition Override), use that transition + duration
	 * instead of the global one, like native OBS. Stored in the scene's private
	 * settings under "transition" / "transition_duration". */
	obs_source_t *override_tr = nullptr;
	int dur = obs_frontend_get_transition_duration();
	if (to) {
		obs_data_t *ps = obs_source_get_private_settings(to);
		const char *ov = obs_data_get_string(ps, "transition");
		int ovdur = (int)obs_data_get_int(ps, "transition_duration");
		if (ov && *ov) {
			override_tr = find_frontend_transition(ov);
			if (override_tr && ovdur > 0)
				dur = ovdur;
		}
		obs_data_release(ps);
	}
	if (dur <= 0)
		dur = 300;
	g_trans_dur_ms.store(dur, std::memory_order_relaxed);

	{
		std::lock_guard<std::mutex> lock(g_dock_mutex);

		/* Which transition do we want? The scene's override, or the global. */
		std::string desired;
		if (override_tr) {
			const char *n = obs_source_get_name(override_tr);
			desired = n ? n : "";
		} else {
			obs_source_t *cur = obs_frontend_get_current_transition();
			const char *n = cur ? obs_source_get_name(cur) : nullptr;
			desired = n ? n : "";
			if (cur)
				obs_source_release(cur);
		}

		/* If the wanted transition differs from the cloned one, rebuild it.
		 * Seed the new clone with the previously shown scene so the swap still
		 * animates from old -> new instead of snapping. */
		if (g_dock_transition && g_dock_tr_name != desired) {
			teardown_dock_transition_locked();
			ensure_dock_transition_locked(override_tr);
			if (!prev_scene.empty() && g_dock_transition) {
				obs_source_t *from =
					obs_get_source_by_name(prev_scene.c_str());
				if (from) {
					obs_transition_set(g_dock_transition, from);
					obs_source_release(from);
					g_dock_tr_primed = true;
				}
			}
		} else {
			ensure_dock_transition_locked(override_tr);
		}

		/* Drive the transition. The first scene primes it instantly; later
		 * changes run the transition (fade/stinger/...) for the set duration,
		 * so the delayed feed shows the native transition. */
		if (to) {
			if (!g_dock_tr_primed) {
				obs_transition_set(g_dock_transition, to);
				g_dock_tr_primed = true;
			} else {
				obs_transition_start(g_dock_transition,
						     OBS_TRANSITION_MODE_AUTO,
						     dur, to);
			}
		}
	}

	if (override_tr)
		obs_source_release(override_tr);
	if (to)
		obs_source_release(to);

	/* Retarget every Docks-mode source to the new scene. */
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry)
		if (is_docks(s))
			set_target(s, MODE_DOCKS, name ? name : "");
}

void warp_set_redirect_scene(bool on)
{
	g_redirect_scene = on;
}

bool warp_get_redirect_scene()
{
	return g_redirect_scene;
}

void warp_set_pause_scene(const char *name)
{
	std::lock_guard<std::mutex> lock(g_dock_mutex);
	g_pause_scene = name ? name : "";
}

/* Current dock scene name (the lifecycle reads it for the Docks-mode retarget). */
std::string warp_current_dock_scene()
{
	std::lock_guard<std::mutex> lock(g_dock_mutex);
	return g_dock_scene;
}
