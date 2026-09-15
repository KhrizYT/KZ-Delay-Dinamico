/*
 * KZ Delay Dinámico - controlador simple para OBS + Aitum Vertical.
 *
 * V2.4 mejora la fidelidad del audio automático. El motor horizontal captura
 * únicamente las fuentes que realmente estaban asignadas a alguna pista de OBS
 * antes del retardo, evitando mezclar fuentes ocultas/no asignadas. También
 * conserva la máscara real de pistas usadas en lugar de enviar KZ a todas.
 */
#include "kz-delay-dock.hpp"

#include "../audio/dse-audio.hpp"
#include "../core/dse-internal.hpp"
#include "../warp/warp-control.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <callback/proc.h>

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *DELAY_SOURCE_ID = "kz_delay_dinamico";
constexpr const char *DOCK_ID = "KZDelayDinamicoDock";

QPointer<QWidget> g_dock;
QPointer<QLabel> g_state_label;
QPointer<QLabel> g_target_label;
QPointer<QSpinBox> g_delay_spin;
QPointer<QPushButton> g_live_btn;
QPointer<QPushButton> g_delay_btn;
QPointer<QPushButton> g_recover_btn;
QPointer<QTimer> g_timer;

std::string g_live_main_scene;
std::string g_live_vertical_scene;
std::string g_vertical_canvas_name;

obs_source_t *g_main_delay_source = nullptr;
obs_source_t *g_vertical_delay_source = nullptr;
obs_scene_t *g_main_output_scene = nullptr;
obs_canvas_t *g_vertical_canvas = nullptr;

bool g_delay_output_active = false;
bool g_recovering = false;
bool g_registered = false;

struct SavedAudioRoute {
	obs_source_t *source = nullptr;
	uint32_t mixers = 0;
};

std::vector<SavedAudioRoute> g_saved_audio_routes;
bool g_audio_direct_silenced = false;

size_t g_program_mix_idx = 0; /* Track 1 en directo */
size_t g_delay_bus_mix_idx = 5; /* preferimos Track 6 como bus oculto */
int current_delay_seconds()
{
	return g_delay_spin ? g_delay_spin->value() : 30;
}

std::string current_main_scene()
{
	obs_source_t *cur = obs_frontend_get_current_scene();
	if (!cur)
		return {};
	const char *name = obs_source_get_name(cur);
	std::string out = name ? name : "";
	obs_source_release(cur);
	return out;
}

std::string aitum_current_scene()
{
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return {};

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_int(&cd, "width", 0);
	calldata_set_int(&cd, "height", 0);
	const bool ok = proc_handler_call(ph, "aitum_vertical_get_scene", &cd);
	const char *scene = ok ? calldata_string(&cd, "scene") : nullptr;
	std::string out = scene ? scene : "";
	calldata_free(&cd);
	return out;
}

struct CanvasRefLookup {
	std::string scene_name;
	std::string canvas_name;
	obs_canvas_t *canvas = nullptr;
};

bool find_canvas_ref_for_scene(void *param, obs_canvas_t *canvas)
{
	auto *lookup = static_cast<CanvasRefLookup *>(param);
	obs_source_t *src =
		obs_canvas_get_source_by_name(canvas, lookup->scene_name.c_str());
	if (!src)
		return true;

	obs_source_release(src);
	lookup->canvas = obs_canvas_get_ref(canvas);
	const char *name = obs_canvas_get_name(canvas);
	lookup->canvas_name = name ? name : "";
	return false;
}

bool refresh_vertical_canvas(const std::string &scene)
{
	if (scene.empty())
		return false;

	CanvasRefLookup lookup{scene, {}, nullptr};
	obs_enum_canvases(find_canvas_ref_for_scene, &lookup);
	if (!lookup.canvas)
		return false;

	if (g_vertical_canvas)
		obs_canvas_release(g_vertical_canvas);
	g_vertical_canvas = lookup.canvas;
	g_vertical_canvas_name = lookup.canvas_name;
	return true;
}

bool is_kz_delay_source(obs_source_t *source)
{
	if (!source)
		return false;
	const char *id = obs_source_get_unversioned_id(source);
	return id && std::strcmp(id, DELAY_SOURCE_ID) == 0;
}

bool is_legacy_delay_source(obs_source_t *source)
{
	if (!source)
		return false;
	const char *id = obs_source_get_unversioned_id(source);
	return id && std::strcmp(id, "delayed_source_engine") == 0;
}

void add_audio_source_unique(std::vector<obs_source_t *> &out,
			     obs_source_t *source, bool already_refed)
{
	if (!source)
		return;
	if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
		if (already_refed)
			obs_source_release(source);
		return;
	}
	for (obs_source_t *existing : out) {
		if (existing == source) {
			if (already_refed)
				obs_source_release(source);
			return;
		}
	}
	if (!already_refed)
		source = obs_source_get_ref(source);
	if (source)
		out.push_back(source);
}

std::vector<obs_source_t *> collect_obs_audio_sources()
{
	std::vector<obs_source_t *> out;

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *list =
				static_cast<std::vector<obs_source_t *> *>(param);
			add_audio_source_unique(*list, source, false);
			return true;
		},
		&out);

	/* Desktop Audio / Mic-Aux globales pueden vivir en canales de salida
	 * aunque no aparezcan como items de una escena. */
	for (uint32_t ch = 1; ch <= 8; ch++) {
		obs_source_t *source = obs_get_output_source(ch); /* strong ref */
		if (source)
			add_audio_source_unique(out, source, true);
	}

	return out;
}

void release_audio_source_list(std::vector<obs_source_t *> &sources)
{
	for (obs_source_t *source : sources)
		obs_source_release(source);
	sources.clear();
}

void configure_automatic_audio_selection(obs_data_t *settings)
{
	if (!settings)
		return;

	obs_data_set_bool(settings, "audio_auto", true);
	obs_data_set_int(settings, "audio_storage", 1);

	auto sources = collect_obs_audio_sources();
	for (obs_source_t *source : sources) {
		const char *name = obs_source_get_name(source);
		if (!name || !*name)
			continue;

		/* Limpia selecciones viejas primero: obs_source_get_settings() conserva
		 * las claves anteriores entre actualizaciones del motor privado. */
		obs_data_set_bool(settings, name, false);

		/* Nunca realimentar KZ ni el Broadcast Delay antiguo dentro de KZ. */
		if (is_kz_delay_source(source) || is_legacy_delay_source(source))
			continue;

		/* Solo capturar lo que OBS realmente estaba enviando a alguna pista.
		 * La V2.3 seleccionaba TODAS las fuentes con audio, incluso ocultas o
		 * no asignadas, y eso podía cambiar mucho el timbre/mezcla respecto
		 * del audio nativo de OBS. */
		if (obs_source_get_audio_mixers(source) == 0)
			continue;

		obs_data_set_bool(settings, name, true);
	}
	release_audio_source_list(sources);
}

DelayedSource *main_engine_instance()
{
	std::lock_guard<std::mutex> lock(g_reg_mutex);
	for (DelayedSource *s : g_registry) {
		if (s && s->self == g_main_delay_source)
			return s;
	}
	return nullptr;
}

void set_program_mix_input(size_t mix_idx)
{
	DelayedSource *s = main_engine_instance();
	if (!s)
		return;
	set_program_mix_track(s, mix_idx);
	g_program_mix_idx = mix_idx;
}

/* Busca una pista libre para el bus interno. Evitamos Track 1 porque es el
 * programa que usan normalmente stream/recording. */
size_t choose_delay_bus_mix()
{
	uint32_t used = 0;
	auto sources = collect_obs_audio_sources();
	for (obs_source_t *source : sources) {
		if (!is_kz_delay_source(source))
			used |= obs_source_get_audio_mixers(source) & 0x3F;
	}
	release_audio_source_list(sources);

	for (int i = 5; i >= 1; --i) {
		if ((used & (1u << i)) == 0)
			return (size_t)i;
	}

	blog(LOG_WARNING,
	     "[kz-delay-dinamico] no hay pista libre; usando Track 6 como bus");
	return 5;
}
void restore_direct_audio()
{
	/* Primero apaga la salida retardada para que Track 1 quede limpio. */
	if (g_main_delay_source)
		obs_source_set_audio_mixers(g_main_delay_source, 0);

	for (SavedAudioRoute &route : g_saved_audio_routes) {
		if (route.source) {
			obs_source_set_audio_mixers(route.source, route.mixers);
			obs_source_release(route.source);
			route.source = nullptr;
		}
	}
	g_saved_audio_routes.clear();
	g_audio_direct_silenced = false;

	/* Ya sin KZ en Track 1, volvemos a capturar exactamente la mezcla
	 * nativa de Track 1 y seguimos llenando el ring para el prÃ³ximo salto. */
	set_program_mix_input(0);
}
void silence_direct_audio()
{
	restore_direct_audio();

	const uint32_t program_bit = 0x01; /* Track 1 */
	g_delay_bus_mix_idx = choose_delay_bus_mix();
	const uint32_t bus_bit = 1u << g_delay_bus_mix_idx;

	auto sources = collect_obs_audio_sources();
	for (obs_source_t *source : sources) {
		if (is_kz_delay_source(source))
			continue;

		const uint32_t mixers = obs_source_get_audio_mixers(source);
		if ((mixers & program_bit) == 0)
			continue;

		SavedAudioRoute route;
		route.source = obs_source_get_ref(source);
		route.mixers = mixers;
		if (route.source)
			g_saved_audio_routes.push_back(route);

		if (is_legacy_delay_source(source)) {
			/* El Broadcast Delay antiguo no entra al bus y tampoco sale
			 * directo por Track 1 durante la prueba. */
			obs_source_set_audio_mixers(source, mixers & ~program_bit);
		} else {
			/* Conserva cualquier otra pista que ya tuviera la fuente,
			 * quita solo Track 1 y aÃ±ade el bus interno. */
			obs_source_set_audio_mixers(
				source, (mixers & ~program_bit) | bus_bit);
		}
	}
	release_audio_source_list(sources);

	/* El bus contiene la MISMA mezcla nativa que antes salÃ­a por Track 1.
	 * Cambiamos el punto de captura sin resetear el ring. */
	set_program_mix_input(g_delay_bus_mix_idx);

	/* Ahora sÃ­: KZ es lo Ãºnico que sale por Track 1. */
	if (g_main_delay_source)
		obs_source_set_audio_mixers(g_main_delay_source, program_bit);

	g_audio_direct_silenced = true;

	blog(LOG_INFO,
	     "[kz-delay-dinamico] retardo audio: Track 1 -> bus Track %zu -> KZ",
	     g_delay_bus_mix_idx + 1);
}
obs_source_t *create_delay_source(const char *name, int mode, int seconds)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "enabled", true);
	obs_data_set_int(settings, "mode", mode);
	obs_data_set_double(settings, "delay_sec", (double)seconds);
	obs_data_set_int(settings, "storage", STORAGE_DISK);
	obs_data_set_int(settings, "audio_storage", 1);
	obs_data_set_bool(settings, "audio_auto", mode == MODE_DOCKS);

	obs_source_t *src =
		obs_source_create_private(DELAY_SOURCE_ID, name, settings);
	obs_data_release(settings);
	return src;
}

bool ensure_internal_graph()
{
	const int seconds = current_delay_seconds();

	if (!g_main_delay_source) {
		g_main_delay_source =
			create_delay_source("__KZDD Motor Horizontal",
					    MODE_DOCKS, seconds);
	}
	if (!g_main_delay_source)
		return false;

	if (!g_main_output_scene) {
		g_main_output_scene =
			obs_scene_create_private("__KZDD Salida Horizontal");
		if (g_main_output_scene)
			obs_scene_add(g_main_output_scene, g_main_delay_source);
	}
	if (!g_main_output_scene)
		return false;

	if (g_live_vertical_scene.empty())
		return false;

	if (!g_vertical_canvas ||
	    g_vertical_canvas_name.empty()) {
		if (!refresh_vertical_canvas(g_live_vertical_scene))
			return false;
	} else {
		/* Si Aitum cambia de canvas, vuelve a localizarlo automáticamente. */
		obs_source_t *probe = obs_canvas_get_source_by_name(
			g_vertical_canvas, g_live_vertical_scene.c_str());
		if (!probe) {
			if (!refresh_vertical_canvas(g_live_vertical_scene))
				return false;
		} else {
			obs_source_release(probe);
		}
	}

	if (!g_vertical_delay_source) {
		g_vertical_delay_source =
			create_delay_source("__KZDD Motor Vertical",
					    MODE_CANVAS_SCENE, seconds);
		if (g_vertical_delay_source)
			obs_source_set_muted(g_vertical_delay_source, true);
	}
	if (!g_vertical_delay_source)
		return false;

	return true;
}

void set_source_delay(obs_source_t *src, int seconds)
{
	if (!src)
		return;

	obs_data_t *settings = obs_source_get_settings(src);
	obs_data_set_double(settings, "delay_sec", (double)seconds);
	obs_data_set_int(settings, "storage", STORAGE_DISK);
	obs_source_update(src, settings);
	obs_data_release(settings);
}

void configure_main_source(int seconds)
{
	if (!g_main_delay_source)
		return;

	obs_data_t *settings = obs_source_get_settings(g_main_delay_source);
	obs_data_set_int(settings, "mode", MODE_DOCKS);
	obs_data_set_double(settings, "delay_sec", (double)seconds);
	obs_data_set_int(settings, "storage", STORAGE_DISK);
	configure_automatic_audio_selection(settings);
	obs_source_update(g_main_delay_source, settings);
	obs_data_release(settings);
}

void configure_vertical_source(const std::string &scene, int seconds)
{
	if (scene.empty() || !g_vertical_delay_source)
		return;

	if (!refresh_vertical_canvas(scene))
		return;

	const std::string target =
		"@canvas:" + g_vertical_canvas_name + "|" + scene;

	obs_data_t *settings = obs_source_get_settings(g_vertical_delay_source);
	obs_data_set_int(settings, "mode", MODE_CANVAS_SCENE);
	obs_data_set_string(settings, "target", target.c_str());
	obs_data_set_double(settings, "delay_sec", (double)seconds);
	obs_data_set_int(settings, "storage", STORAGE_DISK);
	obs_source_update(g_vertical_delay_source, settings);
	obs_data_release(settings);

	/* El audio sale únicamente por el motor horizontal. */
	obs_source_set_muted(g_vertical_delay_source, true);
}

void set_delay_seconds(int seconds)
{
	if (!ensure_internal_graph())
		return;

	configure_main_source(seconds);
	set_source_delay(g_vertical_delay_source, seconds);
	if (!g_live_vertical_scene.empty())
		configure_vertical_source(g_live_vertical_scene, seconds);
}

bool setup_ready()
{
	return g_main_delay_source && g_vertical_delay_source &&
	       g_main_output_scene && g_vertical_canvas;
}

void set_vertical_canvas_output(obs_source_t *source)
{
	if (!source || !g_vertical_canvas)
		return;
	obs_canvas_set_channel(g_vertical_canvas, 0, source);
}

bool switch_main_scene_frontend(const std::string &name)
{
	if (name.empty())
		return false;

	obs_source_t *scene = obs_get_source_by_name(name.c_str());
	if (!scene)
		return false;

	const bool is_scene = obs_source_is_scene(scene);
	if (is_scene)
		obs_frontend_set_current_scene(scene);

	obs_source_release(scene);
	return is_scene;
}

void activate_delay_outputs()
{
	if (!setup_ready())
		return;

	/*
	 * MUY IMPORTANTE:
	 * No reemplazamos el canal 0 del canvas principal. En OBS ese canal
	 * contiene la TRANSICION activa; sustituirla rompe el cambio normal de
	 * escenas. En su lugar, le pedimos al frontend que transicione a una
	 * escena PRIVADA que contiene el motor de retardo. La escena no aparece
	 * en la lista del usuario.
	 */
	obs_frontend_set_current_scene(obs_scene_get_source(g_main_output_scene));

	/* Aitum no usa la transición principal de OBS, así que su canvas sí puede
	 * apuntar temporalmente al motor vertical. Al volver a directo se restaura
	 * mediante la propia API de Aitum para no dejar su estado desincronizado. */
	set_vertical_canvas_output(g_vertical_delay_source);
}

void restore_live_outputs()
{
	const std::string main = g_live_main_scene;
	const std::string vertical = g_live_vertical_scene;

	if (!main.empty())
		switch_main_scene_frontend(main);

	/* La transición horizontal puede disparar el mapeo normal de Aitum.
	 * Después forzamos la escena vertical exacta que estaba activa antes
	 * del retardo, ya usando la API de Aitum. */
	QTimer::singleShot(120, [vertical] {
		if (!vertical.empty()) {
			proc_handler_t *ph = obs_get_proc_handler();
			if (!ph)
				return;

			calldata_t cd;
			calldata_init(&cd);
			calldata_set_int(&cd, "width", 0);
			calldata_set_int(&cd, "height", 0);
			calldata_set_string(&cd, "scene", vertical.c_str());
			proc_handler_call(ph, "aitum_vertical_switch_scene", &cd);
			calldata_free(&cd);
		}
	});
}

void sync_live_targets()
{
	if (g_delay_output_active)
		return;

	const std::string main = current_main_scene();
	if (!main.empty())
		g_live_main_scene = main;

	const std::string vertical = aitum_current_scene();
	if (!vertical.empty())
		g_live_vertical_scene = vertical;

	if (!ensure_internal_graph())
		return;

	warp_set_redirect_scene(false);
	if (!g_live_main_scene.empty())
		warp_set_dock_scene(g_live_main_scene.c_str());

	configure_main_source(current_delay_seconds());

	/* V2.5: precarga continua con la mezcla final de Track 1. */
	set_program_mix_input(0);
	if (g_main_delay_source)
		obs_source_set_audio_mixers(g_main_delay_source, 0);

	if (!g_live_vertical_scene.empty())
		configure_vertical_source(g_live_vertical_scene,
					  current_delay_seconds());
}

void go_live()
{
	g_recovering = false;
	warp_set_state(WARP_LIVE);

	/* Evita doble audio durante la transición de regreso. */
	if (g_main_delay_source)
		obs_source_set_audio_mixers(g_main_delay_source, 0);

	restore_live_outputs();
	restore_direct_audio();
	g_delay_output_active = false;
}

void go_delay()
{
	/* Captura los objetivos reales inmediatamente antes de cambiar la salida. */
	sync_live_targets();
	if (g_live_main_scene.empty() || g_live_vertical_scene.empty() ||
	    !setup_ready())
		return;

	const int seconds = current_delay_seconds();
	set_delay_seconds(seconds);
	warp_set_redirect_scene(false);
	warp_set_dock_scene(g_live_main_scene.c_str());
	configure_main_source(seconds);
	configure_vertical_source(g_live_vertical_scene, seconds);
	warp_set_state(WARP_DELAYED);

	/* Conserva las fuentes vivas para capturarlas, pero quita su salida
	 * directa de las pistas para que solo se escuche KZ retardado. */
	silence_direct_audio();

	g_delay_output_active = true;
	g_recovering = false;
	activate_delay_outputs();
}

void recover_x2()
{
	if (!g_delay_output_active)
		return;
	g_recovering = true;
	warp_set_play_speed(2.0);
}

QString state_text()
{
	if (g_live_vertical_scene.empty())
		return QStringLiteral("Aitum Vertical no detectado todavía");

	if (!setup_ready())
		return QStringLiteral("Preparando motores internos...");

	WarpStatus st;
	if (!warp_get_status(st))
		return QStringLiteral("Preparando motor...");

	if (st.disk_error)
		return QStringLiteral("Error de búfer en disco");
	if (st.buffer_filling)
		return QStringLiteral("Preparando búfer de %1 s...")
			.arg((int)std::round(st.target_s));
	if (g_recovering)
		return QStringLiteral("RECUPERANDO x2 · %1 s detrás")
			.arg(std::fabs(st.distance_s), 0, 'f', 1);
	if (g_delay_output_active)
		return QStringLiteral("RETARDO · %1 s detrás")
			.arg(std::fabs(st.distance_s), 0, 'f', 1);
	return QStringLiteral("EN DIRECTO · búfer listo (%1 s)")
		.arg(st.target_s, 0, 'f', 0);
}

void poll()
{
	sync_live_targets();

	WarpStatus st;
	if (g_recovering && warp_get_status(st) &&
	    std::fabs(st.distance_s) <= 0.12)
		go_live();

	if (g_state_label)
		g_state_label->setText(state_text());

	if (g_target_label) {
		const QString h = QString::fromUtf8(g_live_main_scene.c_str());
		const QString v = QString::fromUtf8(g_live_vertical_scene.c_str());
		g_target_label->setText(
			QStringLiteral("Horizontal: %1\nVertical: %2")
				.arg(h.isEmpty() ? QStringLiteral("—") : h,
				     v.isEmpty() ? QStringLiteral("—") : v));
	}

	const bool ready = setup_ready();
	if (g_live_btn)
		g_live_btn->setEnabled(ready);
	if (g_delay_btn)
		g_delay_btn->setEnabled(ready &&
					!g_live_main_scene.empty() &&
					!g_live_vertical_scene.empty());
	if (g_recover_btn)
		g_recover_btn->setEnabled(ready && g_delay_output_active);
}

QWidget *build_dock()
{
	auto *root = new QWidget();
	auto *layout = new QVBoxLayout(root);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *title = new QLabel(QStringLiteral("KZ Delay Dinámico"));
	QFont tf = title->font();
	tf.setBold(true);
	tf.setPointSize(tf.pointSize() + 2);
	title->setFont(tf);
	layout->addWidget(title);

	auto *delay_row = new QHBoxLayout();
	delay_row->addWidget(new QLabel(QStringLiteral("Retardo")));
	g_delay_spin = new QSpinBox();
	g_delay_spin->setRange(5, 300);
	g_delay_spin->setValue(30);
	g_delay_spin->setSuffix(QStringLiteral(" s"));
	delay_row->addWidget(g_delay_spin, 1);
	layout->addLayout(delay_row);

	auto *buttons = new QHBoxLayout();
	g_live_btn = new QPushButton(QStringLiteral("EN DIRECTO"));
	g_delay_btn = new QPushButton(QStringLiteral("ACTIVAR RETARDO"));
	g_recover_btn = new QPushButton(QStringLiteral("RECUPERAR x2"));
	buttons->addWidget(g_live_btn);
	buttons->addWidget(g_delay_btn);
	buttons->addWidget(g_recover_btn);
	layout->addLayout(buttons);

	g_state_label = new QLabel(QStringLiteral("Preparando..."));
	g_state_label->setWordWrap(true);
	layout->addWidget(g_state_label);

	g_target_label = new QLabel();
	g_target_label->setWordWrap(true);
	layout->addWidget(g_target_label);

	auto *hint = new QLabel(QStringLiteral(
		"No necesitas crear escenas ni fuentes de retardo. KZ Delay Dinámico "
		"prepara sus motores internamente y mantiene sincronizados video y audio "
		"entre el canvas horizontal y Aitum Vertical."));
	hint->setWordWrap(true);
	hint->setEnabled(false);
	layout->addWidget(hint);
	layout->addStretch(1);

	QObject::connect(g_live_btn, &QPushButton::clicked,
			 [] { go_live(); });
	QObject::connect(g_delay_btn, &QPushButton::clicked,
			 [] { go_delay(); });
	QObject::connect(g_recover_btn, &QPushButton::clicked,
			 [] { recover_x2(); });
	QObject::connect(g_delay_spin,
			 QOverload<int>::of(&QSpinBox::valueChanged),
			 [](int value) {
				 if (!g_delay_output_active)
					 set_delay_seconds(value);
			 });

	return root;
}

void release_internal_graph()
{
	if (g_delay_output_active)
		restore_live_outputs();
	restore_direct_audio();

	if (g_main_output_scene) {
		obs_scene_release(g_main_output_scene);
		g_main_output_scene = nullptr;
	}
	if (g_main_delay_source) {
		obs_source_release(g_main_delay_source);
		g_main_delay_source = nullptr;
	}
	if (g_vertical_delay_source) {
		obs_source_release(g_vertical_delay_source);
		g_vertical_delay_source = nullptr;
	}
	if (g_vertical_canvas) {
		obs_canvas_release(g_vertical_canvas);
		g_vertical_canvas = nullptr;
	}
	g_vertical_canvas_name.clear();
}

} // namespace

void register_kz_delay_dock()
{
	if (g_registered)
		return;
	g_registered = true;

	g_dock = build_dock();
	obs_frontend_add_dock_by_id(
		DOCK_ID, "KZ Delay Dinámico", g_dock);

	warp_set_redirect_scene(false);

	g_timer = new QTimer(g_dock);
	QObject::connect(g_timer, &QTimer::timeout, [] { poll(); });
	g_timer->start(250);

	QTimer::singleShot(800, [] {
		sync_live_targets();
		set_delay_seconds(current_delay_seconds());
		poll();
	});
}

void unregister_kz_delay_dock()
{
	g_registered = false;
	g_recovering = false;

	if (g_timer)
		g_timer->stop();

	release_internal_graph();
	g_delay_output_active = false;

	g_timer = nullptr;
	g_dock = nullptr;
	g_state_label = nullptr;
	g_target_label = nullptr;
	g_delay_spin = nullptr;
	g_live_btn = nullptr;
	g_delay_btn = nullptr;
	g_recover_btn = nullptr;
}
