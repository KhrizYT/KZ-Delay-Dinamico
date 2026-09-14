/*
 * Broadcast Delay - OBS source properties + defaults (the settings UI).
 *
 * Builds the source's properties panel (mode/target/delay/storage/audio/
 * advanced) and its default settings. Pure UI glue over a DelayedSource; wired
 * into the obs_source_info in dse-source.cpp.
 */
#pragma once

struct obs_data;
typedef struct obs_data obs_data_t;
struct obs_properties;
typedef struct obs_properties obs_properties_t;

obs_properties_t *dse_get_properties(void *data);
void dse_get_defaults(obs_data_t *settings);
