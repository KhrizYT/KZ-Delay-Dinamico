/*
 * KZ Delay - simplified operator dock for Broadcast Delay + Aitum Vertical.
 *
 * V1 deliberately reuses the proven delay engine already in this module and
 * replaces the day-to-day workflow with three buttons: LIVE, DELAY, RECOVER.
 * The operator keeps using normal OBS scenes; while live, this controller keeps
 * both the horizontal and Aitum Vertical delay buffers pointed at the current
 * real scenes. Entering delay switches only the program outputs to the existing
 * delay scenes, without retargeting the buffers to themselves.
 */
#include "kz-delay-dock.hpp"

#include "../core/dse-internal.hpp"
#include "../warp/warp-control.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <callback/proc.h>

#include <QFont>
#include <QGroupBox>
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

/* V1 uses the scenes/sources already proven in the user's setup. A later V2 can
 * create them automatically and make these names editable. */
constexpr const char *MAIN_DELAY_SCENE = "Delay Escena";
constexpr const char *VERT_DELAY_SCENE = "Delay";
constexpr const char *MAIN_DELAY_SOURCE = "Broadcast Delay";
constexpr const char *VERT_DELAY_SOURCE = "Broadcast Delay 2";
constexpr const char *DOCK_ID = "KZDelayDock";

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
bool g_delay_output_active = false;
bool g_recovering = false;
bool g_registered = false;

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

bool switch_main_scene(const std::string &name)
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

bool aitum_switch_scene(const std::string &scene)
{
	if (scene.empty())
		return false;
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return false;

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_int(&cd, "width", 0);
	calldata_set_int(&cd, "height", 0);
	calldata_set_string(&cd, "scene", scene.c_str());
	const bool ok = proc_handler_call(ph, "aitum_vertical_switch_scene", &cd);
	calldata_free(&cd);
	return ok;
}

struct CanvasLookup {
	std::string scene_name;
	std::string canvas_name;
};

bool find_canvas_for_scene(void *param, obs_canvas_t *canvas)
{
	auto *lookup = static_cast<CanvasLookup *>(param);
	obs_source_t *src =
		obs_canvas_get_source_by_name(canvas, lookup->scene_name.c_str());
	if (!src)
		return true;
	obs_source_release(src);
	const char *name = obs_canvas_get_name(canvas);
	lookup->canvas_name = name ? name : "";
	return false;
}

std::string canvas_for_scene(const std::string &scene)
{
	if (scene.empty())
		return {};
	CanvasLookup lookup{scene, {}};
	obs_enum_canvases(find_canvas_for_scene, &lookup);
	return lookup.canvas_name;
}

bool source_exists(const char *name)
{
	obs_source_t *src = obs_get_source_by_name(name);
	if (!src)
		return false;
	obs_source_release(src);
	return true;
}

void set_source_delay(const char *name, int seconds)
{
	obs_source_t *src = obs_get_source_by_name(name);
	if (!src)
		return;
	obs_data_t *settings = obs_source_get_settings(src);
	obs_data_set_double(settings, "delay_sec", (double)seconds);
	/* Long delay on 1440p should stay on disk. Preserve every audio setting. */
	obs_data_set_int(settings, "storage", STORAGE_DISK);
	obs_source_update(src, settings);
	obs_data_release(settings);
	obs_source_release(src);
}

void configure_main_source(int seconds)
{
	obs_source_t *src = obs_get_source_by_name(MAIN_DELAY_SOURCE);
	if (!src)
		return;
	obs_data_t *settings = obs_source_get_settings(src);
	obs_data_set_int(settings, "mode", MODE_DOCKS);
	obs_data_set_double(settings, "delay_sec", (double)seconds);
	obs_data_set_int(settings, "storage", STORAGE_DISK);
	obs_source_update(src, settings);
	obs_data_release(settings);
	obs_source_release(src);
}

void configure_vertical_source(const std::string &scene, int seconds)
{
	if (scene.empty())
		return;
	const std::string canvas = canvas_for_scene(scene);
	if (canvas.empty())
		return;
	g_vertical_canvas_name = canvas;

	obs_source_t *src = obs_get_source_by_name(VERT_DELAY_SOURCE);
	if (!src)
		return;
	const std::string target = "@canvas:" + canvas + "|" + scene;
	obs_data_t *settings = obs_source_get_settings(src);
	obs_data_set_int(settings, "mode", MODE_CANVAS_SCENE);
	obs_data_set_string(settings, "target", target.c_str());
	obs_data_set_double(settings, "delay_sec", (double)seconds);
	obs_data_set_int(settings, "storage", STORAGE_DISK);
	obs_source_update(src, settings);
	obs_data_release(settings);
	/* Never let the vertical companion duplicate audio into the main mix. */
	obs_source_set_muted(src, true);
	obs_source_release(src);
}

void set_delay_seconds(int seconds)
{
	configure_main_source(seconds);
	set_source_delay(VERT_DELAY_SOURCE, seconds);
	if (!g_live_vertical_scene.empty())
		configure_vertical_source(g_live_vertical_scene, seconds);
}

bool setup_ready()
{
	return source_exists(MAIN_DELAY_SOURCE) && source_exists(VERT_DELAY_SOURCE) &&
	       source_exists(MAIN_DELAY_SCENE);
}

void sync_live_targets()
{
	if (g_delay_output_active)
		return;

	const std::string main = current_main_scene();
	if (!main.empty() && main != MAIN_DELAY_SCENE && main != g_live_main_scene) {
		g_live_main_scene = main;
		/* KZ owns target routing. Disable the original auto-redirect because it
		 * would retarget to Delay Escena and create a self-capture loop. */
		warp_set_redirect_scene(false);
		warp_set_dock_scene(g_live_main_scene.c_str());
		configure_main_source(g_delay_spin ? g_delay_spin->value() : 30);
	}

	const std::string vertical = aitum_current_scene();
	if (!vertical.empty() && vertical != VERT_DELAY_SCENE &&
	    vertical != g_live_vertical_scene) {
		g_live_vertical_scene = vertical;
		configure_vertical_source(g_live_vertical_scene,
					  g_delay_spin ? g_delay_spin->value() : 30);
	}
}

void go_live()
{
	g_recovering = false;
	warp_set_state(WARP_LIVE);
	g_delay_output_active = false;

	if (!g_live_main_scene.empty())
		switch_main_scene(g_live_main_scene);
	/* Let Aitum process the native OBS scene change first, then force the exact
	 * vertical scene we remembered. */
	const std::string vertical = g_live_vertical_scene;
	QTimer::singleShot(120, [vertical] {
		if (!vertical.empty())
			aitum_switch_scene(vertical);
	});
}

void go_delay()
{
	/* Capture the current real scenes one last time before switching outputs. */
	sync_live_targets();
	if (g_live_main_scene.empty() || g_live_vertical_scene.empty())
		return;

	const int seconds = g_delay_spin ? g_delay_spin->value() : 30;
	set_delay_seconds(seconds);
	warp_set_redirect_scene(false);
	warp_set_dock_scene(g_live_main_scene.c_str());
	configure_vertical_source(g_live_vertical_scene, seconds);
	warp_set_state(WARP_DELAYED);

	g_delay_output_active = true;
	g_recovering = false;
	switch_main_scene(MAIN_DELAY_SCENE);
	QTimer::singleShot(80, [] { aitum_switch_scene(VERT_DELAY_SCENE); });
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
	if (!setup_ready())
		return QStringLiteral("Falta Broadcast Delay / Broadcast Delay 2 o Delay Escena");

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
		return QStringLiteral("DELAY · %1 s")
			.arg(st.distance_s, 0, 'f', 1);
	return QStringLiteral("LIVE · búfer listo (%1 s)")
		.arg(st.target_s, 0, 'f', 0);
}

void poll()
{
	/* While live, OBS + Aitum stay completely native. We only mirror their
	 * current scene choices into the two background delay buffers. */
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
		g_delay_btn->setEnabled(ready && !g_live_main_scene.empty() &&
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

	auto *title = new QLabel(QStringLiteral("KZ Delay"));
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
	g_live_btn = new QPushButton(QStringLiteral("DIRECTO"));
	g_delay_btn = new QPushButton(QStringLiteral("DELAY"));
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
		"Usa tus escenas normales de OBS. KZ Delay mantiene el búfer horizontal "
		"y Aitum Vertical en segundo plano y solo cambia a las escenas Delay cuando "
		"pulsas DELAY."));
	hint->setWordWrap(true);
	hint->setEnabled(false);
	layout->addWidget(hint);
	layout->addStretch(1);

	QObject::connect(g_live_btn, &QPushButton::clicked, [] { go_live(); });
	QObject::connect(g_delay_btn, &QPushButton::clicked, [] { go_delay(); });
	QObject::connect(g_recover_btn, &QPushButton::clicked, [] { recover_x2(); });
	QObject::connect(g_delay_spin,
			 QOverload<int>::of(&QSpinBox::valueChanged), [](int value) {
				 if (!g_delay_output_active)
					 set_delay_seconds(value);
			 });

	return root;
}

} // namespace

void register_kz_delay_dock()
{
	if (g_registered)
		return;
	g_registered = true;

	g_dock = build_dock();
	obs_frontend_add_dock_by_id(DOCK_ID, "KZ Delay", g_dock);

	/* The original redirect is useful for the old dock, but harmful to this
	 * controller because Delay Escena contains the delayed source itself. */
	warp_set_redirect_scene(false);

	g_timer = new QTimer(g_dock);
	QObject::connect(g_timer, &QTimer::timeout, [] { poll(); });
	g_timer->start(250);
	QTimer::singleShot(800, [] {
		sync_live_targets();
		set_delay_seconds(g_delay_spin ? g_delay_spin->value() : 30);
		poll();
	});
}

void unregister_kz_delay_dock()
{
	g_registered = false;
	g_recovering = false;
	g_delay_output_active = false;
	if (g_timer)
		g_timer->stop();
	g_timer = nullptr;
	g_dock = nullptr;
	g_state_label = nullptr;
	g_target_label = nullptr;
	g_delay_spin = nullptr;
	g_live_btn = nullptr;
	g_delay_btn = nullptr;
	g_recover_btn = nullptr;
}
