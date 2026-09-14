/*
 * Broadcast Delay - Consumption dock (per-element resource readout).
 * Split out of ui/dock.cpp.
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
/* ---- Consommation dock: detailed per-element resource usage ---- */
static QWidget *build_consumption_dock()
{
	QWidget *root = new QWidget();
	QVBoxLayout *v = new QVBoxLayout(root);
	v->setContentsMargins(8, 8, 8, 8);
	v->setSpacing(6);

	QCheckBox *cb = new QCheckBox(
		QString::fromUtf8(obs_module_text("Consumption.Enable")));
	cb->setChecked(false);
	v->addWidget(cb);

	QLabel *info = new QLabel();
	info->setTextFormat(Qt::RichText);
	info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	info->setWordWrap(true);
	QScrollArea *sa = new QScrollArea();
	sa->setWidgetResizable(true);
	sa->setFrameShape(QFrame::NoFrame);
	sa->setStyleSheet("QScrollArea{background:transparent;}");
	sa->setWidget(info);
	v->addWidget(sa, 1);

	/* Poll once a second, but ONLY while the dock is visible AND the
	 * checkbox is on - otherwise reading is disabled (costs nothing). */
	QTimer *t = new QTimer(root);
	QObject::connect(t, &QTimer::timeout, [root, cb, info] {
		const bool active = cb->isChecked() && root->isVisible();
		warp_set_consumption_reading(active);
		if (!active) {
			info->setText(QString::fromUtf8(
				obs_module_text("Consumption.Disabled")));
			return;
		}
		std::vector<ConsumptionItem> items = warp_get_consumption();
		if (items.empty()) {
			info->setText(QString::fromUtf8(
				obs_module_text("Consumption.NoSource")));
			return;
		}
		QString html;
		for (const ConsumptionItem &it : items) {
			if (it.header)
				html += QString("<p style='margin:8px 0 2px 0;"
						"color:#9aa0ac;'><b>%1</b></p>")
						.arg(QString::fromUtf8(
							     it.label.c_str())
						     .toHtmlEscaped());
			else
				html += QString("<div style='margin-left:10px;'>"
						"%1 : <b>%2</b></div>")
						.arg(QString::fromUtf8(
							     it.label.c_str())
							     .toHtmlEscaped(),
						     QString::fromUtf8(
							     it.value.c_str())
							     .toHtmlEscaped());
		}
		info->setText(html);
	});
	t->start(1000);

	return root;
}

void register_consumption_dock()
{
	obs_frontend_add_dock_by_id(
		"delay_source_consumption",
		obs_module_text("Dock.ConsumptionTitle"), build_consumption_dock());
	keep_dock_on_screen("delay_source_consumption");
}
