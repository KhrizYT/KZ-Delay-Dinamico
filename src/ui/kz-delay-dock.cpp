/*
 * KZ Delay Dinámico - controlador simple para OBS + Aitum Vertical.
 *
 * V2 crea sus motores de retardo de forma privada. El usuario no necesita
 * crear escenas ni fuentes auxiliares: el plugin conserva las escenas normales
 * de OBS/Aitum como objetivos y conmuta directamente los canvases a los motores
 * retardados cuando se activa el retardo.
 */
#include "kz-delay-dock.hpp"

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
#include <string>

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
obs_canvas_t *g_vertical_canvas = nullptr;

bool g_delay_output_active = false;
bool g_recovering = false;
bool g_registered = false;

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

obs_source_t *create_delay_source(const char *name, int mode, int seconds)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "enabled", true);
	obs_data_set_int(settings, "mode", mode);
	obs_data_set_double(settings, "delay_sec", (double)seconds);
	obs_data_set_int(settings, "storage", STORAGE_DISK);
	obs_data_set_int(settings, "audio_storage", 1);
	obs_data_set_bool(settings, "audio_auto", false);

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
	       g_vertical_canvas;
}

void set_main_canvas_output(obs_source_t *source)
{
	if (!source)
		return;

	obs_canvas_t *canvas = obs_get_main_canvas();
	if (!canvas)
		return;
	obs_canvas_set_channel(canvas, 0, source);
	obs_canvas_release(canvas);
}

void set_vertical_canvas_output(obs_source_t *source)
{
	if (!source || !g_vertical_canvas)
		return;
	obs_canvas_set_channel(g_vertical_canvas, 0, source);
}

void activate_delay_outputs()
{
	if (!setup_ready())
		return;

	set_main_canvas_output(g_main_delay_source);
	set_vertical_canvas_output(g_vertical_delay_source);
}

void restore_live_outputs()
{
	if (!g_live_main_scene.empty()) {
		obs_source_t *main =
			obs_get_source_by_name(g_live_main_scene.c_str());
		if (main) {
			set_main_canvas_output(main);
			obs_source_release(main);
		}
	}

	if (g_vertical_canvas && !g_live_vertical_scene.empty()) {
		obs_source_t *vertical = obs_canvas_get_source_by_name(
			g_vertical_canvas, g_live_vertical_scene.c_str());
		if (vertical) {
			set_vertical_canvas_output(vertical);
			obs_source_release(vertical);
		}
	}
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
	if (!g_live_vertical_scene.empty())
		configure_vertical_source(g_live_vertical_scene,
					  current_delay_seconds());
}

void go_live()
{
	g_recovering = false;
	restore_live_outputs();
	warp_set_state(WARP_LIVE);
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
	configure_vertical_source(g_live_vertical_scene, seconds);
	warp_set_state(WARP_DELAYED);

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
			.arg(st.distance_s, 0, 'f', 1);
	if (g_delay_output_active)
		return QStringLiteral("RETARDO · %1 s")
			.arg(st.distance_s, 0, 'f', 1);
	return QStringLiteral("EN DIRECTO · búfer listo (%1 s)")
		.arg(st.target_s, 0, 'f', 0);
}

void poll()
{
	sync_live_targets();

	WarpStatus st;
	if (g_recovering && warp_get_status(st) && st.distance_s <= 0.12)
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
		"prepara sus motores internamente y mantiene sincronizados el canvas "
		"horizontal y Aitum Vertical."));
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
