/*
 * Broadcast Delay - OBS plugin module entry point
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "ui/kz-delay-dock.hpp"
#include "warp/warp-control.hpp"

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0.0"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("kz-delay-dinamico", "es-ES")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Broadcast Delay - time-shifted audio/video broadcast.\n"
	       "WebSocket API (obs-websocket 30+) vendor broadcast_delay :\n"
	       "  set_state, set_redirect, set_dock_scene,\n"
	       "  seek, seek_step, set_factors, set_seek_step,\n"
	       "  set_pause_scene, get_status.\n"
	       "Compatible: Stream Deck, Steam Deck, Touch Portal, scripts.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "KZ Delay Dinámico";
}

void register_delayed_source(void);
void register_warp_dock(void);
void register_warp_preview_dock(void);
void register_mixer_dock(void);
void warp_shutdown(void);

/* ================================================================== */
/*  Scene-redirect interception                                        */
/* ================================================================== */

static void on_obs_event(enum obs_frontend_event event, void *)
{
	/* Drive the shared transition from the OBS program scene when redirect is
	 * on, OR when a Scene-mode source is present (Scene mode follows the
	 * program with native transitions). */
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED &&
	    (warp_get_redirect_scene() || warp_any_scene_mode())) {
		obs_source_t *cur = obs_frontend_get_current_scene();
		if (cur) {
			const char *name = obs_source_get_name(cur);
			if (name && *name)
				warp_set_dock_scene(name);
			obs_source_release(cur);
		}
	}
	if (event == OBS_FRONTEND_EVENT_TRANSITION_CHANGED)
		warp_refresh_transition();
}

/* ================================================================== */
/*  obs-websocket vendor requests (degrade gracefully if not present)   */
/* ================================================================== */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HMODULE ws_dll = nullptr;
#else
static void *ws_dll = nullptr;
#endif

/* Function pointers resolved at runtime. */
typedef void (*ws_vendor_register_t)(const char *vendor, const char *request,
				     void *cb, void *priv);
typedef void (*ws_vendor_unregister_t)(const char *vendor, const char *request);
typedef void (*ws_vendor_emit_t)(const char *vendor, const char *event,
				 void *data);

static ws_vendor_register_t   ws_register   = nullptr;
static ws_vendor_unregister_t ws_unregister = nullptr;
static ws_vendor_emit_t       ws_emit       = nullptr;

static bool load_websocket_api()
{
	if (ws_register) return true;
#ifdef _WIN32
	ws_dll = GetModuleHandleW(L"obs-websocket");
#else
	/* On Linux obs-websocket is statically linked into the binary;
	 * the symbols are already exported. */
	ws_register   = (ws_vendor_register_t)dlsym(RTLD_DEFAULT,
						    "obs_websocket_vendor_register_request");
	ws_unregister = (ws_vendor_unregister_t)dlsym(RTLD_DEFAULT,
						      "obs_websocket_vendor_unregister_request");
	ws_emit       = (ws_vendor_emit_t)dlsym(RTLD_DEFAULT,
						"obs_websocket_vendor_emit_event");
	return ws_register != nullptr;
#endif
	if (!ws_dll) return false;

	ws_register = (ws_vendor_register_t)GetProcAddress(
		ws_dll, "obs_websocket_vendor_register_request");
	ws_unregister = (ws_vendor_unregister_t)GetProcAddress(
		ws_dll, "obs_websocket_vendor_unregister_request");
	ws_emit = (ws_vendor_emit_t)GetProcAddress(
		ws_dll, "obs_websocket_vendor_emit_event");
	return ws_register != nullptr;
}

/* ------------------------------------------------------------------ */
/*  Request callbacks                                                   */
/* ------------------------------------------------------------------ */

static const char *VENDOR = "kz_delay_dinamico";

/* set_state  { "state": "live"|"delayed"|"pause"|"play" } */
static void req_set_state(void *req, void *, void *)
{
	const char *s = obs_data_get_string((obs_data_t *)req, "state");
	if (!s) return;
	if (strcmp(s, "live") == 0)    warp_set_state(WARP_LIVE);
	if (strcmp(s, "delayed") == 0) warp_set_state(WARP_DELAYED);
	if (strcmp(s, "pause") == 0)   warp_toggle_pause();
	if (strcmp(s, "play") == 0)    warp_play_current();
}

/* set_redirect  { "on": bool } */
static void req_set_redirect(void *req, void *, void *)
{
	warp_set_redirect_scene(obs_data_get_bool((obs_data_t *)req, "on"));
}

/* set_dock_scene  { "name": "SceneName" } */
static void req_set_dock_scene(void *req, void *, void *)
{
	const char *nm = obs_data_get_string((obs_data_t *)req, "name");
	warp_set_dock_scene(nm && *nm ? nm : nullptr);
}

/* seek  { "seconds": 5.0 } */
static void req_seek(void *req, void *, void *)
{
	double sec = obs_data_get_double((obs_data_t *)req, "seconds");
	warp_seek((int64_t)(sec * 1.0e9));
}

/* seek_step  { "direction": 1 }  1 = forward, -1 = backward */
static void req_seek_step(void *req, void *, void *)
{
	warp_seek_step((int)obs_data_get_int((obs_data_t *)req, "direction"));
}

/* set_factors  { "accel": 2.0, "decel": 0.5 } */
static void req_set_factors(void *req, void *, void *)
{
	warp_set_factors(
		obs_data_get_double((obs_data_t *)req, "accel"),
		obs_data_get_double((obs_data_t *)req, "decel"));
}

/* set_seek_step  { "seconds": 5.0 } */
static void req_set_seek_step(void *req, void *, void *)
{
	warp_set_seek_step(obs_data_get_double((obs_data_t *)req, "seconds"));
}


/* set_pause_scene  { "name": "SceneName" }  empty = black */
static void req_set_pause_scene(void *req, void *, void *)
{
	const char *nm = obs_data_get_string((obs_data_t *)req, "name");
	warp_set_pause_scene(nm && *nm ? nm : nullptr);
}

/* get_status -> fills response with full state */
static void req_get_status(void *, void *res, void *)
{
	auto *r = (obs_data_t *)res;
	WarpStatus st;
	if (!warp_get_status(st)) {
		obs_data_set_bool(r, "ok", false);
		return;
	}
	obs_data_set_bool(r, "ok", true);
	obs_data_set_int(r, "state", st.state);
	obs_data_set_double(r, "distance_sec", st.distance_s);
	obs_data_set_double(r, "target_sec", st.target_s);
	obs_data_set_bool(r, "buffer_filling", st.buffer_filling);
	obs_data_set_int(r, "storage", st.storage);
	obs_data_set_bool(r, "disk_error", st.disk_error);
	obs_data_set_bool(r, "has_scene", st.has_scene);
	obs_data_set_double(r, "accel", st.accel);
	obs_data_set_double(r, "decel", st.decel);
	obs_data_set_double(r, "seek_sec", st.seek_s);
}

static void register_ws_requests()
{
	if (!load_websocket_api()) return;

#define REG(n, cb) ws_register(VENDOR, n, (void(*)(void*,void*,void*))(cb), nullptr)
	REG("set_state",               req_set_state);
	REG("set_redirect",            req_set_redirect);
	REG("set_dock_scene",          req_set_dock_scene);
	REG("seek",                    req_seek);
	REG("seek_step",               req_seek_step);
	REG("set_factors",             req_set_factors);
	REG("set_seek_step",           req_set_seek_step);
	REG("set_pause_scene",         req_set_pause_scene);
	REG("get_status",              req_get_status);
#undef REG
}

static void unregister_ws_requests()
{
	if (!ws_unregister) return;
#define UNREG(n) ws_unregister(VENDOR, n)
	UNREG("set_state");
	UNREG("set_redirect");
	UNREG("set_dock_scene");
	UNREG("seek");
	UNREG("seek_step");
	UNREG("set_factors");
	UNREG("set_seek_step");
	UNREG("set_pause_scene");
	UNREG("get_status");
#undef UNREG
}

/* ================================================================== */
/*  Module lifecycle                                                    */
/* ================================================================== */

bool obs_module_load(void)
{
	register_delayed_source();
	register_warp_dock();
	register_warp_preview_dock();
	register_mixer_dock();
	register_consumption_dock();
	register_kz_delay_dock();
	obs_frontend_add_event_callback(on_obs_event, nullptr);
	register_ws_requests();
	blog(LOG_INFO, "[kz-delay-dinamico] loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	unregister_ws_requests();
	obs_frontend_remove_event_callback(on_obs_event, nullptr);
	unregister_kz_delay_dock();
	warp_shutdown();
	blog(LOG_INFO, "[kz-delay-dinamico] unloaded");
}
