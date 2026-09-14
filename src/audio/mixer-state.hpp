/*
 * Broadcast Delay - per-source native-audio mixer state
 *
 * Each Broadcast Delay owns one MixerState (it is NOT global): the native-OS
 * (WASAPI) device list it captures, with per-device volume / mute / enable.
 * The popup mixer table (opened from the source properties) binds to it, and
 * the capture/feed paths read it.
 */
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>

struct MixerChannel {
	std::string name; /* friendly device name */
	std::string id;   /* WASAPI endpoint id */
	float volume = 1.0f;
	bool muted = false;
	bool enabled = false; /* the [x] column = capture this device */
	bool is_input = false; /* true = microphone, false = speaker/loopback */
	bool is_default = false; /* OS default device for its type */
	bool monitor = false;  /* play this device live to the monitor output */
	bool mono = false;     /* downmix L/R to mono for this device */
	float balance = 0.0f;  /* L/R pan: -1 = full left, 0 = center, +1 = right */
	/* Instantaneous block peak per channel (0..1), set by the capture
	 * thread; the meter widget does its own attack/release + peak hold. */
	float level_l = 0.0f;
	float level_r = 0.0f;
};

struct MixerState {
	std::mutex mtx;
	std::vector<MixerChannel> chans;
};

/* Replace the device list, preserving volume/mute/enabled by name. */
void mixer_state_set_devices(MixerState &st,
			     const std::vector<std::string> &names,
			     const std::vector<std::string> &ids,
			     const std::vector<bool> &inputs,
			     const std::vector<bool> &defaults);

/* Endpoint id of the first enabled ([x]) device, or "" if none. */
std::string mixer_state_first_enabled_id(MixerState &st);

/* Gain for a device by name (0 if muted), 1.0 if unknown. */
float mixer_state_gain(MixerState &st, const std::string &name);

/* Per-device flags used by the feed path (false if unknown). */
bool mixer_state_monitor(MixerState &st, const std::string &name);
bool mixer_state_mono(MixerState &st, const std::string &name);
float mixer_state_balance(MixerState &st, const std::string &name); /* -1..1 */

/* Push the captured block peak (0..1 per channel) for a device, for the meter. */
void mixer_state_set_level(MixerState &st, const std::string &name, float l,
			   float r);

/* Read the last block peak for a device (0 if unknown). */
void mixer_state_get_level(MixerState &st, const std::string &name, float &l,
			   float &r);

/* Open the mixer table for this state as a NATIVE OBS dock (dockable into the
 * OBS interface, remembered across sessions). `id` is a stable per-source key.
 * Kept alive via the shared ptr so it is safe even if the source is destroyed
 * while open. */
void mixer_show_dialog(std::shared_ptr<MixerState> st, const char *title,
		       const char *id);
