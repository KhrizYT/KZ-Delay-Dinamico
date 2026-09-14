/*
 * Broadcast Delay - per-source hotkeys (bindable in Settings -> Hotkeys).
 *
 * Registers/unregisters every transport hotkey for one DelayedSource and holds
 * their callbacks. dse_create/dse_destroy just call these two.
 */
#pragma once

struct DelayedSource;

void register_dse_hotkeys(DelayedSource *s);
void unregister_dse_hotkeys(DelayedSource *s);
