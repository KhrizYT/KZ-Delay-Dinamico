/*
 * Broadcast Delay - OBS source properties + defaults (see dse-properties.hpp).
 */
#include "dse-properties.hpp"

#include "../core/dse-internal.hpp"
#include "audio/mixer-state.hpp"

#include <obs-module.h>
#include <cstring>
#include <string>

namespace {

struct EnumCtx {
	obs_property_t *list;
	const char *self_name;
};

static bool add_source_option(void *param, obs_source_t *source)
{
	auto *ctx = static_cast<EnumCtx *>(param);

	/* Source Mode: skip scenes and our own type to avoid loops. */
	if (obs_source_is_scene(source))
		return true;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0)
		return true;

	const char *name = obs_source_get_name(source);
	if (name && ctx->self_name && strcmp(name, ctx->self_name) == 0)
		return true;
	if (strcmp(obs_source_get_id(source), "delayed_source_engine") == 0)
		return true;

	if (name)
		obs_property_list_add_string(ctx->list, name, name);
	return true;
}

static bool add_scene_option(void *param, obs_source_t *source)
{
	auto *ctx = static_cast<EnumCtx *>(param);
	const char *name = obs_source_get_name(source);
	if (name)
		obs_property_list_add_string(ctx->list, name, name);
	return true;
}

struct CanvasEnumCtx {
	obs_property_t *list;
	std::string canvas_name;
};

static bool add_canvas_scene_option(void *param, obs_source_t *source)
{
	auto *ctx = static_cast<CanvasEnumCtx *>(param);
	const char *scene_name = obs_source_get_name(source);
	if (!scene_name || !*scene_name)
		return true;

	/* Keep a friendly label in the UI, but store enough information to
	 * resolve the scene through its owning OBS canvas later. */
	const std::string label = ctx->canvas_name + " / " + scene_name;
	const std::string value =
		"@canvas:" + ctx->canvas_name + "|" + scene_name;
	obs_property_list_add_string(ctx->list, label.c_str(), value.c_str());
	return true;
}

static bool add_canvas_options(void *param, obs_canvas_t *canvas)
{
	auto *list = static_cast<obs_property_t *>(param);
	const char *canvas_name = obs_canvas_get_name(canvas);
	if (!canvas_name || !*canvas_name)
		return true;

	CanvasEnumCtx ctx{list, canvas_name};
	obs_canvas_enum_scenes(canvas, add_canvas_scene_option, &ctx);
	return true;
}

static void populate_targets(obs_property_t *list, long long mode,
			     const char *self_name)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("Target.None"), "");

	EnumCtx ctx{list, self_name};
	if (mode == 1)
		obs_enum_scenes(add_scene_option, &ctx);
	else if (mode == MODE_CANVAS_SCENE)
		obs_enum_canvases(add_canvas_options, list);
	else
		obs_enum_sources(add_source_option, &ctx);
}

/* priv is the DelayedSource*. Refreshes the target list; hides it in Docks mode
 * (the target is then driven by the dock's scene selection). */
static bool mode_modified(void *priv, obs_properties_t *props, obs_property_t *,
			  obs_data_t *settings)
{
	auto *s = static_cast<DelayedSource *>(priv);
	const long long mode = obs_data_get_int(settings, "mode");
	obs_property_t *target = obs_properties_get(props, "target");
	obs_property_set_visible(target, mode != MODE_DOCKS);
	if (mode != MODE_DOCKS)
		populate_targets(target, mode,
				 s ? obs_source_get_name(s->self) : nullptr);
	return true;
}

/* Inside the advanced group: show Budget (RAM) vs Compression (Disk). */
static void apply_disk_visibility(obs_properties_t *props, long long storage)
{
	const bool disk = (storage == 1);
	const bool vram = (storage == 2);

	obs_property_t *budget = obs_properties_get(props, "budget_gb");
	obs_property_set_visible(budget, !disk);
	obs_property_set_description(budget, obs_module_text(
		vram ? "Budget.VRAM" : "Budget.RAM"));

	obs_property_set_visible(obs_properties_get(props, "disk_compression"),
				 disk);
}

static bool storage_modified(void *, obs_properties_t *props, obs_property_t *,
			     obs_data_t *settings)
{
	const long long storage = obs_data_get_int(settings, "storage");
	apply_disk_visibility(props, storage);

	const bool ba = obs_data_get_bool(settings, "budget_auto");
	const bool disk = (storage == 1);
	const int codec = (int)obs_data_get_int(settings, "disk_compression");
	obs_property_set_visible(obs_properties_get(props, "budget_gb"), ba && !disk);
	obs_property_set_visible(obs_properties_get(props, "disk_compression"), ba && disk);
	obs_property_set_visible(obs_properties_get(props, "hls_bitrate"), disk && ba && codec > 0);
	obs_property_set_visible(obs_properties_get(props, "dash_encoder"), disk && ba && codec == 4);
	obs_property_set_visible(obs_properties_get(props, "disk_folder"), disk && ba);
	obs_property_set_visible(obs_properties_get(props, "disk_seconds"), disk && ba);
	obs_property_set_visible(obs_properties_get(props, "audio_storage"),
				 disk && (!ba || codec >= 3));
	return true;
}

static bool budget_auto_modified(void *, obs_properties_t *props,
				 obs_property_t *, obs_data_t *settings)
{
	const bool ba = obs_data_get_bool(settings, "budget_auto");
	const bool disk = obs_data_get_int(settings, "storage") == 1;
	const int codec = (int)obs_data_get_int(settings, "disk_compression");
	const bool showQual = disk && ba && codec > 0;

	obs_property_set_visible(obs_properties_get(props, "budget_gb"), ba && !disk);
	obs_property_set_visible(obs_properties_get(props, "disk_compression"), ba && disk);
	obs_property_set_visible(obs_properties_get(props, "hls_bitrate"), showQual);
	obs_property_set_visible(obs_properties_get(props, "dash_encoder"), disk && ba && codec == 4);
	obs_property_set_visible(obs_properties_get(props, "disk_folder"), disk && ba);
	obs_property_set_visible(obs_properties_get(props, "disk_seconds"), disk && ba);
	obs_property_set_visible(obs_properties_get(props, "audio_storage"),
				 disk && (!ba || codec >= 3));
	return true;
}

static bool compression_modified(void *, obs_properties_t *props,
				 obs_property_t *, obs_data_t *settings)
{
	const int codec = (int)obs_data_get_int(settings, "disk_compression");
	const bool disk = obs_data_get_int(settings, "storage") == 1;
	const bool ba = obs_data_get_bool(settings, "budget_auto");
	const bool vis = disk && ba && codec > 0;

	obs_property_t *q = obs_properties_get(props, "hls_bitrate");
	obs_property_set_visible(q, vis);
	if (vis) {
		if (codec == 1) {
			obs_property_float_set_limits(q, 0.0, 9.0, 1.0);
			obs_property_float_set_suffix(q, "");
			obs_property_set_description(q,
				obs_module_text("Compression.PngLevel"));
		} else if (codec == 2) {
			obs_property_float_set_limits(q, 1.0, 31.0, 1.0);
			obs_property_float_set_suffix(q, " (bas = meilleur)");
			obs_property_set_description(q,
				obs_module_text("Compression.JpegQuality"));
		} else {
			obs_property_float_set_limits(q, 1.0, 50.0, 1.0);
			obs_property_float_set_suffix(q, " Mbps");
			obs_property_set_description(q,
				obs_module_text("Compression.Bitrate"));
		}
	}
	obs_property_set_visible(
		obs_properties_get(props, "dash_encoder"),
		disk && ba && codec == 4);
	obs_property_set_visible(obs_properties_get(props, "audio_storage"),
				 disk && (!ba || codec >= 3));
	return true;
}

/* Show OBS source list when auto is ON, mixer when OFF. */
static bool audio_auto_modified(void *, obs_properties_t *props,
				obs_property_t *, obs_data_t *settings)
{
	const bool a = obs_data_get_bool(settings, "audio_auto");
	obs_property_set_visible(obs_properties_get(props, "grp_audio_list"), a);
	obs_property_set_visible(obs_properties_get(props, "open_mixer"), !a);
	return true;
}

} // namespace

obs_properties_t *dse_get_properties(void *data)
{
	auto *s = static_cast<DelayedSource *>(data);
	obs_properties_t *props = obs_properties_create();

	long long mode_val = 0;
	long long storage_val = 0;
	bool audio_auto_val = true;
	const char *self_name = nullptr;
	if (s && s->self) {
		obs_data_t *settings = obs_source_get_settings(s->self);
		mode_val = obs_data_get_int(settings, "mode");
		storage_val = obs_data_get_int(settings, "storage");
		audio_auto_val = obs_data_get_bool(settings, "audio_auto");
		obs_data_release(settings);
		self_name = obs_source_get_name(s->self);
	}

	/* --- Section: Status --- */
	obs_properties_t *gstatus = obs_properties_create();
	obs_properties_add_bool(gstatus, "enabled", obs_module_text("Enabled"));
	obs_properties_add_group(props, "grp_status",
				 obs_module_text("Sec.Status"),
				 OBS_GROUP_NORMAL, gstatus);

	/* --- Section: Delay settings --- */
	obs_properties_t *gmain = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(
		gmain, "mode", obs_module_text("Mode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Mode.Docks"), 2);
	obs_property_list_add_int(mode, "Canvas scene (Aitum / secondary canvas)",
				  MODE_CANVAS_SCENE);
	obs_property_list_add_int(mode, obs_module_text("Mode.Scene"), 1);
	obs_property_list_add_int(mode, obs_module_text("Mode.Source"), 0);
	obs_property_set_modified_callback2(mode, mode_modified, s);

	obs_property_t *target = obs_properties_add_list(
		gmain, "target", obs_module_text("Target"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_set_visible(target, mode_val != MODE_DOCKS);
	if (mode_val != MODE_DOCKS)
		populate_targets(target, mode_val, self_name);

	obs_property_t *delay = obs_properties_add_int_slider(
		gmain, "delay_sec", obs_module_text("Delay"), 0, 300, 1);
	obs_property_int_set_suffix(delay, " s");

	obs_property_t *storage = obs_properties_add_list(
		gmain, "storage", obs_module_text("Storage"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(storage, obs_module_text("Storage.Disk"), 1);
	obs_property_list_add_int(storage, obs_module_text("Storage.RAM"), 0);
	obs_property_list_add_int(storage, obs_module_text("Storage.VRAM"), 2);
	obs_property_set_modified_callback2(storage, storage_modified, s);
	obs_property_t *astorage = obs_properties_add_list(
		gmain, "audio_storage", obs_module_text("Audio.Storage"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(astorage, obs_module_text("Audio.Storage.RAM"), 0);
	obs_property_list_add_int(astorage, obs_module_text("Audio.Storage.Disk"), 1);
	obs_properties_add_group(props, "grp_main",
				 obs_module_text("Sec.Settings"),
				 OBS_GROUP_NORMAL, gmain);

	/* --- Section: Audio settings --- */
	obs_properties_t *gaudio = obs_properties_create();

	/* Open mixer button (visible when audio_auto is OFF). */
	obs_property_t *wbtn = obs_properties_add_button2(
		gaudio, "open_mixer", obs_module_text("Audio.OpenMixer"),
		[](obs_properties_t *, obs_property_t *, void *data) -> bool {
			auto *ds = static_cast<DelayedSource *>(data);
			if (ds) {
				/* Each source has its OWN mixer -> a per-source dock
				 * (id = source name) but the same friendly title. */
				const char *nm = ds->self
							 ? obs_source_get_name(ds->self)
							 : nullptr;
				mixer_show_dialog(ds->mixer_state,
						  obs_module_text("Mixer.Title"),
						  nm ? nm : "mixer");
			}
			return false;
		},
		s);
	obs_property_set_visible(wbtn, !audio_auto_val);

	obs_property_t *aauto = obs_properties_add_bool(
		gaudio, "audio_auto", obs_module_text("Audio.Auto"));
	obs_property_set_modified_callback2(aauto, audio_auto_modified, s);

	/* Per-source checklist (visible when audio_auto is ON). */
	obs_properties_t *glist = obs_properties_create();
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			auto *p = (obs_properties_t *)param;
			if (!(obs_source_get_output_flags(src) &
			      OBS_SOURCE_AUDIO))
				return true;
			const char *nm = obs_source_get_name(src);
			if (nm)
				obs_properties_add_bool(p, nm, nm);
			return true;
		},
		glist);
	obs_properties_add_group(gaudio, "grp_audio_list",
				 obs_module_text("Audio.Sources"),
				 OBS_GROUP_NORMAL, glist);
	obs_property_set_visible(
		obs_properties_get(gaudio, "grp_audio_list"), audio_auto_val);

	obs_properties_add_group(props, "grp_audio",
				 obs_module_text("Sec.Audio"),
				 OBS_GROUP_NORMAL, gaudio);

	/* --- Advanced --- */
	obs_properties_t *adv = obs_properties_create();
	obs_property_t *bauto = obs_properties_add_bool(
		adv, "budget_auto",
		obs_module_text("Budget.Auto.Enable"));
	obs_property_set_modified_callback2(bauto, budget_auto_modified, s);
	obs_property_t *budget = obs_properties_add_float_slider(
		adv, "budget_gb",
		obs_module_text(storage_val == 2 ? "Budget.VRAM"
						: "Budget.RAM"),
		0.5, 64.0, 0.5);
	obs_property_float_set_suffix(budget, " GB");
	obs_property_t *comp = obs_properties_add_list(
		adv, "disk_compression", obs_module_text("Compression"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(comp, obs_module_text("Compression.HLS"), 3);
	obs_property_list_add_int(comp, obs_module_text("Compression.DASH"), 4);
	obs_property_list_add_int(comp, obs_module_text("Compression.Raw"), 0);
	obs_property_list_add_int(comp, obs_module_text("Compression.Png"), 1);
	obs_property_list_add_int(comp, obs_module_text("Compression.Mjpeg"), 2);
	obs_property_set_modified_callback2(comp, compression_modified, s);
	obs_property_t *bitrate = obs_properties_add_float_slider(
		adv, "hls_bitrate", obs_module_text("Compression.Bitrate"),
		1.0, 50.0, 1.0);
	obs_property_float_set_suffix(bitrate, " Mbps");
	obs_property_t *dash_enc = obs_properties_add_list(
		adv, "dash_encoder", obs_module_text("DASH.Encoder"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(dash_enc, "H.264", 0);
	obs_property_list_add_int(dash_enc, "H.265 / HEVC", 1);
	obs_property_t *folder = obs_properties_add_path(
		adv, "disk_folder", obs_module_text("Disk.Folder"),
		OBS_PATH_DIRECTORY, nullptr, nullptr);
	obs_property_t *duration = obs_properties_add_int_slider(
		adv, "disk_seconds", obs_module_text("Disk.Duration"),
		10, 86400, 10);
	obs_property_int_set_suffix(duration, " s");
	obs_properties_add_group(props, "advanced", obs_module_text("Advanced"),
				 OBS_GROUP_NORMAL, adv);
	{
		const bool ba = s ? obs_data_get_bool(
			obs_source_get_settings(s->self), "budget_auto") : false;
		const bool disk = (storage_val == 1);
		const int codec = s ? (int)obs_data_get_int(
			obs_source_get_settings(s->self), "disk_compression") : 0;
		const bool showQual = disk && ba && codec > 0;
		obs_property_set_visible(obs_properties_get(props, "budget_gb"), ba && !disk);
		obs_property_set_visible(obs_properties_get(props, "disk_compression"), ba && disk);
		obs_property_set_visible(obs_properties_get(props, "hls_bitrate"), showQual);
		obs_property_set_visible(obs_properties_get(props, "dash_encoder"), disk && ba && codec == 4);
		obs_property_set_visible(obs_properties_get(props, "disk_folder"), disk && ba);
		obs_property_set_visible(obs_properties_get(props, "disk_seconds"), disk && ba);
		obs_property_set_visible(obs_properties_get(props, "audio_storage"),
					 disk && (!ba || codec >= 3));
	}

	/* --- Section: Description --- */
	obs_properties_t *gdesc = obs_properties_create();
	obs_properties_add_text(gdesc, "info", obs_module_text("Info"),
				OBS_TEXT_INFO);
	obs_properties_add_text(gdesc, "ws_info",
		obs_module_text("Dock.WSInfo"), OBS_TEXT_INFO);
	obs_properties_add_group(props, "grp_desc",
				 obs_module_text("Sec.Description"),
				 OBS_GROUP_NORMAL, gdesc);
	return props;
}

void dse_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "enabled", true);
	obs_data_set_default_int(settings, "mode", 2);
	obs_data_set_default_int(settings, "delay_sec", 30);
	obs_data_set_default_bool(settings, "audio_auto", false);
	obs_data_set_default_bool(settings, "budget_auto", false);
	obs_data_set_default_int(settings, "storage", 1);
	obs_data_set_default_int(settings, "audio_storage", 1);
	obs_data_set_default_bool(settings, "advanced", false);
	obs_data_set_default_double(settings, "budget_gb", 8.0);
	obs_data_set_default_int(settings, "disk_compression", 0);
	obs_data_set_default_double(settings, "hls_bitrate", 4.0);
	obs_data_set_default_int(settings, "disk_seconds", 60);
	obs_data_set_default_string(settings, "disk_folder",
				     default_temp_folder().c_str());
}
