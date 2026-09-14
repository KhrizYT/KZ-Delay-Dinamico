/*
 * Broadcast Delay - time-warp control interface
 *
 * Bridges the Qt docks (ui/dock.cpp) and the Broadcast Delays (dse-source.cpp).
 * The dock calls these to drive every active Broadcast Delay instance; the source
 * side implements them against a small global registry.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;

enum {
	WARP_LIVE = 0,	  /* at the live edge (delay -> 0) */
	WARP_DELAYED = 1, /* held at the target delay */
	WARP_ACCEL = 2,	  /* speeding up to catch the live edge */
	WARP_DECEL = 3,	  /* slowing down to build the target delay */
	WARP_PAUSED = 4,  /* frozen (auto-resumes once the target is built) */
	WARP_PLAY = 5,	  /* play 1x at the CURRENT delay (any distance) */
};

struct WarpStatus {
	int state = WARP_LIVE;
	double distance_s = 0.0; /* seconds behind live */
	double target_s = 0.0;   /* configured delay */
	uint64_t underflows = 0;
	int storage = 0; /* 0 RAM, 1 Disk, 2 VRAM */
	bool disk_error = false; /* disk buffer failed to start */
	double accel = 2.0;
	double decel = 0.5;
	double play_speed = 1.0; /* current WARP_PLAY rate */
	bool countdown_overlay = true;
	bool buffer_filling = false;
	bool has_scene = false;
	bool dock_picked = false;
	bool active_has_dse = false;
	double seek_s = 5.0;	    /* +/- scrub step (seconds) */
	char pause_scene[128] = {0}; /* hold scene shown while paused ("" = none) */
};

/* Apply to every active Broadcast Delay instance. */
void warp_set_state(int state);
void warp_toggle_pause();
void warp_seek(int64_t delta_ns);
void warp_seek_live(int64_t delta_ns); /* scrub while still playing (WARP_PLAY) */
void warp_play_current(); /* "Play Delay Time": play 1x at the current distance */
void warp_set_play_speed(double speed); /* 0.5/1/2x playback, starts playing */

/* Read status from the primary (first) instance. Returns false if none. */
bool warp_get_status(WarpStatus &out);

/* The dock's live-scene pick: retargets every Docks-mode source. */
void warp_set_dock_scene(const char *name);
std::string warp_current_dock_scene(); /* current dock scene name */
void warp_set_redirect_scene(bool on);
bool warp_get_redirect_scene();
/* True if any source is in Scene mode (program changes drive the transition). */
bool warp_any_scene_mode();

/* Scrub by the configured step (dir = +1 forward / -1 backward). */
void warp_seek_step(int dir);

/* Settings (applied to every Docks-mode source). */
void warp_set_countdown_overlay(bool on);
void warp_set_factors(double accel, double decel);
void warp_set_seek_step(double seconds);
void warp_set_pause_scene(const char *name); /* "" = black + countdown */

/* The source the dock should render (the scene-transition); ref'd, or null.
 * Used by both the delayed capture and the preview dock. Release when done. */
obs_source_t *warp_acquire_dock_render();

/* Release the shared dock transition (called from obs_module_unload). */
void warp_shutdown();
/* Re-create the dock transition when OBS's transition type changes. */
void warp_refresh_transition();

/* Registers the Qt dock with the OBS frontend (called from obs_module_load). */
void register_warp_dock();
void register_warp_preview_dock();
void register_mixer_dock();
void register_consumption_dock();

/* Detailed resource consumption (one line per element), for the Consommation
 * dock. Only gathered when reading is enabled (the dock is visible + its
 * checkbox on) so it costs nothing otherwise. */
struct ConsumptionItem {
	std::string label;
	std::string value;
	bool header = false; /* section title (no value) */
};
std::vector<ConsumptionItem> warp_get_consumption();
void warp_set_consumption_reading(bool on);
/* Per-source mixer state + popup are declared in mixer-state.hpp. */
