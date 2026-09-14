/*
 * Broadcast Delay - Preview dock (scene video return).
 * Split out of ui/dock.cpp. Shows the scene selected in the Diffusion dock.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>

#include "warp/warp-control.hpp"
#include "ui/dock-shared.hpp"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QScrollArea>
#include <QFrame>
#include <QTimer>
#include <QString>
#include <QDockWidget>
#include <QMainWindow>

#include <cstring>
#include <cmath>
#include <string>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
/* ---- Preview dock: live view of the scene selected in the diffusion dock ---- */
static std::mutex g_pv_mutex;
static std::string g_pv_scene;

void set_preview_scene(const char *name)
{
	std::lock_guard<std::mutex> lock(g_pv_mutex);
	g_pv_scene = name ? name : "";
}

static std::string get_preview_scene()
{
	std::lock_guard<std::mutex> lock(g_pv_mutex);
	return g_pv_scene;
}

/* The graphics-thread draw callback: render the selected scene, letterboxed,
 * into the preview display. Needs an ortho projection + viewport, otherwise the
 * source renders with no transform and nothing shows. */
static void preview_render(void *, uint32_t cx, uint32_t cy)
{
	/* Render the dock's transition (so scene switches fade/stinger live),
	 * falling back to the raw selected scene before any pick. */
	obs_source_t *s = warp_acquire_dock_render();
	if (!s) {
		const std::string name = get_preview_scene();
		if (name.empty())
			return;
		s = obs_get_source_by_name(name.c_str());
		if (!s)
			return;
	}

	int sw = (int)obs_source_get_width(s);
	int sh = (int)obs_source_get_height(s);
	if (sw <= 0 || sh <= 0) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			sw = (int)ovi.base_width;
			sh = (int)ovi.base_height;
		}
	}
	if (sw > 0 && sh > 0 && cx > 0 && cy > 0) {
		const float sx = (float)cx / (float)sw;
		const float sy = (float)cy / (float)sh;
		const float scale = sx < sy ? sx : sy; /* fit, keep aspect */
		const int vw = (int)(sw * scale);
		const int vh = (int)(sh * scale);
		const int vx = ((int)cx - vw) / 2;
		const int vy = ((int)cy - vh) / 2;

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		gs_set_viewport(vx, vy, vw, vh);
		obs_source_video_render(s);
		gs_projection_pop();
		gs_viewport_pop();
	}
	obs_source_release(s);
}

/* Heap context so the display outlives build_preview_dock() (the timer/destroy
 * lambdas run long after it returns -- capturing a stack obs_display* by
 * reference, as before, dangled and the preview never worked). */
struct PreviewDock {
	obs_display_t *display = nullptr;
	int last_cx = 0;
	int last_cy = 0;
};

static QWidget *build_preview_dock()
{
	QWidget *w = new QWidget();
	QVBoxLayout *lay = new QVBoxLayout(w);
	lay->setContentsMargins(0, 0, 0, 0);

	QFrame *displayFrame = new QFrame();
	displayFrame->setMinimumSize(320, 180);
	displayFrame->setStyleSheet("background: black;");
	/* Give the frame its own native HWND that OBS draws into directly, and
	 * stop Qt from painting over that surface. */
	displayFrame->setAttribute(Qt::WA_NativeWindow);
	displayFrame->setAttribute(Qt::WA_PaintOnScreen);
	displayFrame->setAttribute(Qt::WA_NoSystemBackground);
	displayFrame->setAttribute(Qt::WA_OpaquePaintEvent);

	lay->addWidget(displayFrame, 1);

	PreviewDock *ctx = new PreviewDock();

	QTimer *setupTimer = new QTimer(w);
	QObject::connect(setupTimer, &QTimer::timeout, [ctx, displayFrame, w] {
		if (!w->isVisible())
			return;
		const int cw = displayFrame->width();
		const int ch = displayFrame->height();
		if (cw <= 0 || ch <= 0)
			return;
#ifdef _WIN32
		if (!ctx->display) {
			struct gs_init_data gi = {};
			gi.cx = (uint32_t)cw;
			gi.cy = (uint32_t)ch;
			gi.format = GS_RGBA;
			gi.zsformat = GS_ZS_NONE;
			gi.window.hwnd = (HWND)displayFrame->winId();
			ctx->display = obs_display_create(&gi, 0x000000);
			if (ctx->display) {
				obs_display_add_draw_callback(
					ctx->display, preview_render, nullptr);
				ctx->last_cx = cw;
				ctx->last_cy = ch;
			}
		} else if (cw != ctx->last_cx || ch != ctx->last_cy) {
			obs_display_resize(ctx->display, (uint32_t)cw,
					   (uint32_t)ch);
			ctx->last_cx = cw;
			ctx->last_cy = ch;
		}
#endif
	});
	setupTimer->start(200);

	QObject::connect(w, &QObject::destroyed, [ctx]() {
		if (ctx->display) {
			obs_display_remove_draw_callback(ctx->display,
							 preview_render, nullptr);
			obs_display_destroy(ctx->display);
		}
		delete ctx;
	});

	return w;
}

void register_warp_preview_dock()
{
	obs_frontend_add_dock_by_id("delay_source_preview",
				    obs_module_text("Dock.PreviewTitle"),
				    build_preview_dock());
}
