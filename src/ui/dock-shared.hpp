/*
 * Broadcast Delay - shared declarations across the dock UI translation units.
 *
 * The dock UI is split one-file-per-dock: ui/dock.cpp (the Diffusion transport
 * dock + its widgets), ui/preview-dock.cpp (the scene video return) and
 * ui/consumption-dock.cpp (the per-element resource readout). These two helpers
 * cross the files.
 */
#pragma once

/* Keep a floating dock's title bar on screen (defined in dock.cpp). */
void keep_dock_on_screen(const char *id);

/* Set the scene shown in the preview dock (defined in preview-dock.cpp; the
 * Diffusion dock's scene list drives it). */
void set_preview_scene(const char *name);
