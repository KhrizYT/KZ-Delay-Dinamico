/*
 * Broadcast Delay - shared engine globals (the registry spine).
 *
 * One definition of the cross-module globals declared extern in dse-internal.hpp,
 * so the source modules (lifecycle, transport, capture, render) share a single
 * registry instead of one giant translation unit.
 */
#include "dse-internal.hpp"

std::mutex g_reg_mutex;
std::vector<DelayedSource *> g_registry;
std::atomic<int> g_trans_dur_ms{300};

std::mutex g_dock_mutex;
std::string g_pause_scene;
