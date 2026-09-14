/*
 * Broadcast Delay - status + consumption reporting (read-only).
 *
 * Gathers the dock's transport status (warp_get_status) and the
 * per-element resource readout for the Consommation dock (warp_get_consumption)
 * from every registered instance. Pure reads of the registry + spine globals;
 * declared in warp-control.hpp.
 */
#include "dse-internal.hpp"

#include "warp/warp-control.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* Set by the Consommation dock: only gather the (cheap) readout when visible. */
static std::atomic<bool> g_consumption_reading{false};

void warp_set_consumption_reading(bool on)
{
	g_consumption_reading.store(on, std::memory_order_relaxed);
}

static std::string fmt_bytes(uint64_t b)
{
	char buf[64];
	if (b >= (1ULL << 30))
		snprintf(buf, sizeof buf, "%.2f Go",
			 (double)b / (1024.0 * 1024.0 * 1024.0));
	else
		snprintf(buf, sizeof buf, "%.1f Mo",
			 (double)b / (1024.0 * 1024.0));
	return buf;
}

std::vector<ConsumptionItem> warp_get_consumption()
{
	std::vector<ConsumptionItem> out;
	if (!g_consumption_reading.load(std::memory_order_relaxed))
		return out;

	{
		std::lock_guard<std::mutex> lock(g_reg_mutex);
		for (DelayedSource *s : g_registry) {
			if (!s->enabled.load(std::memory_order_relaxed))
				continue;
			const char *nm = obs_source_get_name(s->self);
			out.push_back({nm ? nm : "Broadcast Delay", "", true});

			const int st = s->storage.load(std::memory_order_relaxed);
			out.push_back({obs_module_text("Consumption.Storage"),
				       st == 2 ? "VRAM" : st == 1 ? obs_module_text("Consumption.Disk")
								  : "RAM",
				       false});
			if (st == 1) {
				/* Disk: surface free space (and any start error) so
				 * the operator sees a full disk before underflows. */
				if (s->disk_failed.load(std::memory_order_relaxed))
					out.push_back(
						{obs_module_text("Consumption.Disk"),
						 obs_module_text("Dock.DiskError"),
						 false});
				else if (!s->disk_started_folder.empty())
					out.push_back(
						{obs_module_text(
							 "Consumption.DiskFree"),
						 fmt_bytes(os_get_free_disk_space(
							 s->disk_started_folder
								 .c_str())),
						 false});
				char dq[32];
				snprintf(dq, sizeof dq, "%llu",
					 (unsigned long long)s->disk_queue.load(
						 std::memory_order_relaxed));
				out.push_back(
					{obs_module_text("Consumption.DiskQueue"),
					 dq, false});
				uint64_t dropped = s->disk_dropped.load(
					std::memory_order_relaxed);
				if (dropped) {
					char dd[48];
					snprintf(dd, sizeof dd, "\xE2\x9A\xA0 %llu",
						 (unsigned long long)dropped);
					out.push_back(
						{obs_module_text(
							 "Consumption.DiskDropped"),
						 dd, false});
				}
			}
			char res[48];
			snprintf(res, sizeof res, "%ux%u",
				 s->cx.load(std::memory_order_relaxed),
				 s->cy.load(std::memory_order_relaxed));
			out.push_back({obs_module_text("Consumption.Resolution"), res, false});
			out.push_back({obs_module_text("Consumption.VideoBuffer"),
				       fmt_bytes(s->used_bytes.load(
					       std::memory_order_relaxed)),
				       false});
			/* "Frames en mémoire": clamp the running capture counter
			 * to the buffer capacity so it reflects the real fill
			 * (the buffer is a ring; the raw counter grows forever). */
			size_t cap = st == 2 ? s->vram.capacity()
				   : st == 1 ? s->disk_cap
					     : s->ring.capacity();
			size_t fb = (size_t)s->frames_buffered.load(
				std::memory_order_relaxed);
			if (cap && fb > cap)
				fb = cap;
			char fr[64];
			if (cap)
				snprintf(fr, sizeof fr, "%zu / %zu", fb, cap);
			else
				snprintf(fr, sizeof fr, "%zu", fb);
			out.push_back({obs_module_text("Consumption.Frames"), fr, false});
			char dl[48];
			snprintf(dl, sizeof dl, "%.1f s",
				 (double)s->delay_ns.load(
					 std::memory_order_relaxed) /
					 1.0e9);
			out.push_back({obs_module_text("Consumption.Delay"), dl, false});
		}
	}

	return out;
}

bool warp_get_status(WarpStatus &out)
{
	/* Always fill scene / dock info, even when no Docks source exists. */
	out.dock_picked = !warp_current_dock_scene().empty();
	{
		bool found = false;
		obs_source_t *cur = obs_frontend_get_current_scene();
		if (cur) {
			obs_scene_enum_items(obs_scene_from_source(cur),
				[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
					bool *f = (bool *)p;
					obs_source_t *src = obs_sceneitem_get_source(item);
					if (src && strcmp(obs_source_get_id(src),
						"delayed_source_engine") == 0) {
						*f = true;
						return false;
					}
					return true;
				}, &found);
			obs_source_release(cur);
		}
		out.active_has_dse = found;
	}
	out.has_scene = out.dock_picked && out.active_has_dse;

	/* If no Docks source registered, still return the scene info above. */
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	DelayedSource *s = nullptr;
	for (DelayedSource *d : g_registry)
		if (is_docks(d)) {
			s = d;
			break;
		}
	if (!s)
		return false;
	out.accel = s->accel_factor.load(std::memory_order_relaxed);
	out.decel = s->decel_factor.load(std::memory_order_relaxed);
	out.play_speed = s->play_speed.load(std::memory_order_relaxed);
	out.countdown_overlay = s->show_countdown.load(std::memory_order_relaxed);
	/* Use the clock captured at the last tick (same reference as the
	 * playhead) so the displayed distance does not jitter. */
	int64_t now = s->last_now_ns.load(std::memory_order_relaxed);
	if (now == 0)
		now = (int64_t)os_gettime_ns();
	const int64_t ph = s->playhead_ns.load(std::memory_order_relaxed);
	out.state = s->warp_state.load(std::memory_order_relaxed);
	out.distance_s = (double)(now - ph) / 1.0e9;
	if (out.distance_s < 0.0)
		out.distance_s = 0.0;
	out.target_s = (double)s->delay_ns.load(std::memory_order_relaxed) / 1.0e9;
	out.underflows = s->underflows.load(std::memory_order_relaxed);
	out.storage = s->storage.load(std::memory_order_relaxed);
	out.disk_error = s->disk_failed.load(std::memory_order_relaxed);
	out.buffer_filling =
		(out.state == WARP_DELAYED &&
		 s->delay_ns.load(std::memory_order_relaxed) > 0 &&
		 s->frames_buffered.load(std::memory_order_relaxed) <
			 (size_t)std::ceil(out.target_s * active_fps()));
	out.seek_s = (double)s->seek_step_ns.load(std::memory_order_relaxed) / 1.0e9;
	{
		std::lock_guard<std::mutex> dlock(g_dock_mutex);
		const size_t n = g_pause_scene.size();
		const size_t cap = sizeof(out.pause_scene) - 1;
		const size_t cpy = n < cap ? n : cap;
		memcpy(out.pause_scene, g_pause_scene.data(), cpy);
		out.pause_scene[cpy] = '\0';
	}
	return true;
}
