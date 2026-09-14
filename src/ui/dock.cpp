/*
 * Broadcast Delay - Qt control dock
 *
 * A panel (like OBS's "Audio Mixer" / "Controls") with buttons to drive the
 * time-warp of all active Broadcast Delays: Live, Delay, accelerate, decelerate,
 * pause, +/-5s, plus a live status line (state / distance behind live).
 */
#include <obs-module.h>
#include <obs-frontend-api.h>

#include "warp/warp-control.hpp"
#include "ui/dock-shared.hpp"

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QBoxLayout>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QSlider>
#include <QTimer>
#include <QSignalBlocker>
#include <QString>
#include <QStringList>
#include <QListWidget>
#include <QFrame>
#include <QScrollArea>
#include <QGroupBox>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QDateTime>
#include <QToolButton>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QSplitter>
#include <QSplitterHandle>
#include <QPainter>
#include <QEvent>
#include <QWheelEvent>
#include <QAbstractSpinBox>
#include <QDockWidget>
#include <QMainWindow>
#include <QScreen>
#include <QGuiApplication>
#include <QSvgRenderer>

#include <cstring>

/* Stops the mouse wheel from changing a spin box / slider / combo value when the
 * user is just scrolling the panel: the widget only reacts to the wheel once it
 * has keyboard focus (i.e. after a click). Otherwise the wheel passes through to
 * the surrounding scroll area. */
class WheelGuard : public QObject {
public:
	using QObject::QObject;
	bool eventFilter(QObject *o, QEvent *e) override
	{
		if (e->type() == QEvent::Wheel) {
			QWidget *w = qobject_cast<QWidget *>(o);
			if (w && !w->hasFocus()) {
				e->ignore();
				return true; /* eat it -> the scroll area scrolls */
			}
		}
		return QObject::eventFilter(o, e);
	}
};

/* Install the wheel guard on every spin / slider / combo under `root` (and set
 * StrongFocus so they only grab the wheel after a deliberate click). */
static void install_wheel_guard(QWidget *root)
{
	static WheelGuard *guard = new WheelGuard();
	const auto guard_one = [](QWidget *w) {
		w->setFocusPolicy(Qt::StrongFocus);
		w->installEventFilter(guard);
	};
	for (QAbstractSpinBox *w : root->findChildren<QAbstractSpinBox *>())
		guard_one(w);
	for (QSlider *w : root->findChildren<QSlider *>())
		guard_one(w);
	for (QComboBox *w : root->findChildren<QComboBox *>())
		guard_one(w);
}

/* Keeps a FLOATING dock's title bar reachable: whenever it is shown or moved
 * off the visible screen (OBS sometimes restores a stale off-screen geometry),
 * nudge it back into the available screen area. */
class KeepOnScreen : public QObject {
public:
	using QObject::QObject;
	bool eventFilter(QObject *o, QEvent *e) override
	{
		if (e->type() == QEvent::Move || e->type() == QEvent::Show) {
			auto *d = qobject_cast<QDockWidget *>(o);
			if (d && d->isFloating()) {
				QScreen *scr = d->screen() ? d->screen()
							   : QGuiApplication::primaryScreen();
				if (scr) {
					const QRect av = scr->availableGeometry();
					QPoint p = d->pos();
					if (p.y() < av.top())
						p.setY(av.top());
					if (p.x() < av.left())
						p.setX(av.left());
					if (p.x() > av.right() - 80)
						p.setX(av.right() - 240);
					if (p.y() > av.bottom() - 40)
						p.setY(av.bottom() - 200);
					if (p != d->pos())
						d->move(p);
				}
			}
		}
		return QObject::eventFilter(o, e);
	}
};

/* Find the QDockWidget OBS created for `id` and keep it on screen. */
void keep_dock_on_screen(const char *id)
{
	auto *mw = (QMainWindow *)obs_frontend_get_main_window();
	if (!mw)
		return;
	static KeepOnScreen *guard = new KeepOnScreen();
	if (auto *d = mw->findChild<QDockWidget *>(QString::fromUtf8(id)))
		d->installEventFilter(guard);
}

#include <cmath>
#include <string>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

static QPushButton *make_button(const char *text)
{
	QPushButton *b = new QPushButton(QString::fromUtf8(text));
	b->setMinimumHeight(34);
	return b;
}

/* Elastic jog/shuttle: a centred horizontal slider that scrubs the delay
 * timeline. Drag left = go back in time, right = go forward (towards live);
 * the further from centre, the faster the scrub (quadratic for fine control).
 * Springs back to centre on release. Replaces the +5s / -5s buttons. */
class JogSlider : public QSlider {
public:
	explicit JogSlider(bool live = false, QWidget *p = nullptr)
		: QSlider(Qt::Horizontal, p), live_(live)
	{
		setRange(-1000, 1000);
		setValue(0);
		setMinimumHeight(28);
		timer_ = new QTimer(this);
		timer_->setInterval(60);
		QObject::connect(timer_, &QTimer::timeout, [this] { tick(); });
		QObject::connect(this, &QSlider::sliderReleased,
				 [this] { recenter(); });
	}

protected:
	void mousePressEvent(QMouseEvent *e) override
	{
		QSlider::mousePressEvent(e);
		if (!timer_->isActive())
			timer_->start();
	}
	void mouseReleaseEvent(QMouseEvent *e) override
	{
		QSlider::mouseReleaseEvent(e);
		recenter();
	}

public:
	/* Current distance from live (s), pushed by the dock status poll. Drives
	 * the decade-scaled step so navigation stays precise near now and fast
	 * far away, for any buffer size. */
	void setDistance(double s) { distance_s_ = s < 0 ? -s : s; }

private:
	QTimer *timer_ = nullptr;
	int held_ticks_ = 0;
	double distance_s_ = 0.0;
	void recenter()
	{
		timer_->stop();
		held_ticks_ = 0;
		setValue(0);
	}
	void tick()
	{
		const int v = value();
		if (v == 0) {
			held_ticks_ = 0;
			return;
		}
		++held_ticks_;
		const double f = (double)v / 1000.0;        /* -1..1 */
		/* Decade-scaled navigation: the step is the order of magnitude of
		 * the current distance from live, so you move by ~1 s near now,
		 * ~10 s once tens of seconds away, ~100 s in the hundreds, etc.
		 * (0-9 -> step 1, 10-90 -> 10, 100-900 -> 100, ...). Precise near
		 * live AND fast across a 100000 s buffer, with no hard-coded limit.
		 * Displacement (quadratic) + a short hold ramp set how fast we move
		 * through the decades. Left (« ) = advance (towards live), right
		 * (» ) = rewind -> seek negated to match the old button layout. */
		double dist = distance_s_;
		if (dist < 1.0)
			dist = 1.0;
		const double decade = std::pow(10.0, std::floor(std::log10(dist)));
		const double held_s = held_ticks_ * 0.06;
		const double ramp = 0.15 + 0.85 * (1.0 - std::exp(-held_s / 1.0));
		const double sec = f * std::fabs(f) * ramp * decade * 0.35;
		/* Pause jog: «=advance / »=rewind (negated). Live jog (délai
		 * direct): inverted -> «=rewind / »=advance. */
		const int64_t delta = (int64_t)((live_ ? sec : -sec) * 1.0e9);
		if (live_)
			warp_seek_live(delta); /* keep playing at the new delay */
		else
			warp_seek(delta);      /* scrub while paused */
	}
	bool live_ = false;
};

/* A two-widget row that lays out side-by-side when wide enough, and stacks them
 * vertically (classic) when there isn't enough width for both. */
class ReflowRow : public QWidget {
public:
	ReflowRow(QWidget *a, QWidget *b, QWidget *parent = nullptr)
		: QWidget(parent), a_(a), b_(b)
	{
		box_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
		box_->setContentsMargins(0, 0, 0, 0);
		/* Match the censor buttons' vertical spacing when stacked. */
		box_->setSpacing(4);
		box_->addWidget(a_);
		box_->addWidget(b_);
	}

	/* Allow the row to shrink to a single (stacked) button's width, so a
	 * narrow dock reflows instead of forcing a horizontal scrollbar. */
	QSize minimumSizeHint() const override
	{
		const QSize sa = a_->minimumSizeHint();
		const QSize sb = b_->minimumSizeHint();
		return QSize(qMax(sa.width(), sb.width()),
			     sa.height() + sb.height() + box_->spacing());
	}

protected:
	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		const int need = a_->sizeHint().width() + b_->sizeHint().width() +
				 box_->spacing();
		box_->setDirection(width() < need ? QBoxLayout::TopToBottom
						  : QBoxLayout::LeftToRight);
	}

private:
	QWidget *a_, *b_;
	QBoxLayout *box_;
};

/* A themed gear icon, painted to match the dock's text colour (like OBS's
 * "Controls" settings cog). Avoids needing the Qt SVG module. */
static QIcon make_gear_icon(const QColor &col, int px)
{
	const double PI = 3.14159265358979323846;
	QPixmap pm(px, px);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QPointF c(px / 2.0, px / 2.0);
	const double rOut = px * 0.48, rIn = px * 0.38, rHole = px * 0.18;
	const int teeth = 10;
	const int steps = teeth * 2;

	QPainterPath cog;
	for (int i = 0; i <= steps; i++) {
		const double a = (PI * 2.0) * i / steps;
		const double r = (i % 2 == 0) ? rOut : rIn;
		const QPointF pt(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
		if (i == 0)
			cog.moveTo(pt);
		else
			cog.lineTo(pt);
	}
	cog.closeSubpath();

	QPainterPath hole;
	hole.addEllipse(c, rHole, rHole);
	cog = cog.subtracted(hole);

	p.setPen(Qt::NoPen);
	p.setBrush(col);
	p.drawPath(cog);
	p.end();
	return QIcon(pm);
}


static void open_settings_dialog(QWidget *parent)
{
	WarpStatus st;
	bool has = warp_get_status(st);

	QDialog dlg(parent);
	dlg.setWindowTitle(QString::fromUtf8(obs_module_text("Dock.Settings")));
	QFormLayout *form = new QFormLayout(&dlg);

	QCheckBox *cd = new QCheckBox();
	cd->setChecked(has ? st.countdown_overlay : true);
	warp_set_countdown_overlay(cd->isChecked());
	QObject::connect(cd, &QCheckBox::toggled,
			 [](bool b) { warp_set_countdown_overlay(b); });
	form->addRow(QString::fromUtf8(obs_module_text("Dock.CountdownOverlay")), cd);

	/* Hold scene shown while paused. */
	QComboBox *pscene = new QComboBox();
	pscene->addItem(QString::fromUtf8(obs_module_text("Dock.PauseNone")),
			QString());
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const char *nm = obs_source_get_name(scenes.sources.array[i]);
		if (nm)
			pscene->addItem(QString::fromUtf8(nm));
	}
	obs_frontend_source_list_free(&scenes);
	if (has && st.pause_scene[0]) {
		int idx = pscene->findText(QString::fromUtf8(st.pause_scene));
		if (idx >= 0)
			pscene->setCurrentIndex(idx);
	}
	QObject::connect(pscene, &QComboBox::currentTextChanged,
			 [pscene](const QString &) {
		warp_set_pause_scene(pscene->currentIndex() <= 0
					     ? ""
					     : pscene->currentText()
						       .toUtf8()
						       .constData());
	});
	form->addRow(QString::fromUtf8(obs_module_text("Dock.PauseScene")), pscene);

	QDoubleSpinBox *acc = new QDoubleSpinBox();
	acc->setRange(1.25, 8.0);
	acc->setSingleStep(0.25);
	acc->setValue(has ? st.accel : 2.0);
	form->addRow(QString::fromUtf8(obs_module_text("Dock.AccelFactor")), acc);

	QDoubleSpinBox *dec = new QDoubleSpinBox();
	dec->setRange(0.10, 0.90);
	dec->setSingleStep(0.05);
	dec->setValue(has ? st.decel : 0.5);
	form->addRow(QString::fromUtf8(obs_module_text("Dock.DecelFactor")), dec);

	auto apply = [acc, dec] { warp_set_factors(acc->value(), dec->value()); };
	QObject::connect(acc, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
			 [apply](double) { apply(); });
	QObject::connect(dec, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
			 [apply](double) { apply(); });

	QCheckBox *redir = new QCheckBox();
	redir->setChecked(warp_get_redirect_scene());
	QObject::connect(redir, &QCheckBox::toggled,
			 [](bool b) { warp_set_redirect_scene(b); });
	form->addRow(QString::fromUtf8(
		obs_module_text("Dock.RedirectScene")), redir);

	/* Keep the box in sync if redirect is toggled elsewhere (hotkey /
	 * websocket) while this dialog is open. */
	QTimer *rsync = new QTimer(&dlg);
	QObject::connect(rsync, &QTimer::timeout, [redir] {
		const bool on = warp_get_redirect_scene();
		if (redir->isChecked() != on) {
			redir->blockSignals(true);
			redir->setChecked(on);
			redir->blockSignals(false);
		}
	});
	rsync->start(400);

	dlg.exec();
}


/* Custom splitter handle: invisible by default, white 1px line on hover. */
class DseSplitterHandle : public QSplitterHandle {
public:
	DseSplitterHandle(Qt::Orientation o, QSplitter *p)
		: QSplitterHandle(o, p)
	{
		setMouseTracking(true);
		setAttribute(Qt::WA_Hover, true);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		if (!underMouse())
			return;
		QPainter p(this);
		const int barW = 2;
		const int x = (width() - barW) / 2;
		p.fillRect(x, 0, barW, height(), QColor(255, 255, 255));
	}
};

/* Custom splitter: paints nothing, white bar centred on hover. */
class DseSplitter : public QSplitter {
public:
	using QSplitter::QSplitter;

protected:
	QSplitterHandle *createHandle() override
	{
		return new DseSplitterHandle(orientation(), this);
	}
};

/* GitHub mark (official invertocat) for the link button next to the gear. */
static QIcon make_github_icon(int px)
{
	static const char *svg =
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'>"
		"<path fill='#fefefe' d='M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 "
		"5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49"
		"-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 "
		"1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2"
		"-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 "
		".67-.21 2.2.82a7.65 7.65 0 0 1 2-.27c.68 0 1.36.09 2 .27 1.53-1.04 "
		"2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07"
		"-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 "
		".21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8z'/></svg>";
	QByteArray data(svg);
	QSvgRenderer r(data);
	QPixmap pm(px, px);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	r.render(&p, QRectF(0, 0, px, px));
	p.end();
	return QIcon(pm);
}

/* Popup: project repository + donation link (opens in the browser). */
static void show_links_popup(QWidget *parent)
{
	QDialog dlg(parent);
	dlg.setWindowTitle(QString::fromUtf8(obs_module_text("DelayedSourceEngine")));
	QVBoxLayout *v = new QVBoxLayout(&dlg);
	QLabel *l = new QLabel();
	l->setTextFormat(Qt::RichText);
	l->setOpenExternalLinks(true);
	l->setText("<p style='margin:4px 0;'><b>GitHub</b><br>"
		   "<a href='https://github.com/H0K0H/obs-broadcast-delay'>"
		   "github.com/H0K0H/obs-broadcast-delay</a></p>"
		   "<p style='margin:10px 0 4px;'><b>Support the project &#10084;</b><br>"
		   "<a href='https://ko-fi.com/hkn'>ko-fi.com/hkn</a></p>");
	v->addWidget(l);
	dlg.resize(380, 130);
	dlg.exec();
}

static QWidget *build_dock_widget()
{
	QWidget *root = new QWidget();
	QVBoxLayout *rootLayout = new QVBoxLayout(root);
	rootLayout->setContentsMargins(6, 6, 6, 6);
	DseSplitter *split = new DseSplitter(Qt::Horizontal);
	split->setHandleWidth(10);
	split->setChildrenCollapsible(false);
	rootLayout->addWidget(split);

	/* ---- Left: scene list styled as a dock-in-a-dock (auto-refreshing) ---- */
	QListWidget *sceneList = new QListWidget();
	sceneList->setMinimumWidth(140);
	sceneList->setFrameShape(QFrame::NoFrame); /* the box draws the border */

	/* Rebuilds the list only when the scene set actually changed. */
	auto repopulate = [sceneList] {
		QStringList names;
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			const char *nm =
				obs_source_get_name(scenes.sources.array[i]);
			if (nm)
				names << QString::fromUtf8(nm);
		}
		obs_frontend_source_list_free(&scenes);

		bool same = names.size() == sceneList->count();
		for (int i = 0; same && i < names.size(); i++)
			if (sceneList->item(i)->text() != names[i])
				same = false;
		if (same)
			return;

		const QListWidgetItem *cur = sceneList->currentItem();
		const QString sel = cur ? cur->text() : QString();
		sceneList->blockSignals(true);
		sceneList->clear();
		sceneList->addItems(names);
		const QList<QListWidgetItem *> m =
			sceneList->findItems(sel, Qt::MatchExactly);
		if (!m.isEmpty())
			sceneList->setCurrentItem(m.first());
		sceneList->blockSignals(false);
	};
	repopulate();
	QObject::connect(sceneList, &QListWidget::currentItemChanged,
			 [](QListWidgetItem *item, QListWidgetItem *) {
				 if (!item)
					 return;
				 const QByteArray n = item->text().toUtf8();
				 warp_set_dock_scene(n.constData());
				 set_preview_scene(n.constData());
			 });

	/* Bordered container with a coloured header (dock-in-a-dock look). */
	QFrame *sceneBox = new QFrame();
	sceneBox->setObjectName("dseSceneBox");
	sceneBox->setStyleSheet(
		"#dseSceneBox { border: 1px solid #3c404d; border-radius: 4px; }"
		"QLabel#dseSceneHdr { background: #3c404d; padding: 4px 6px;"
		" font-weight: bold; border-top-left-radius: 4px;"
		" border-top-right-radius: 4px; }");
	QVBoxLayout *boxLay = new QVBoxLayout(sceneBox);
	boxLay->setContentsMargins(0, 0, 0, 0);
	boxLay->setSpacing(0);
	QLabel *hdr = new QLabel(QString::fromUtf8(obs_module_text("Dock.Scenes")));
	hdr->setObjectName("dseSceneHdr");
	boxLay->addWidget(hdr);
	boxLay->addWidget(sceneList, 1);
	split->addWidget(sceneBox);

	/* Auto-refresh the scene list. */
	QTimer *sceneTimer = new QTimer(root);
	QObject::connect(sceneTimer, &QTimer::timeout,
			 [repopulate] { repopulate(); });
	sceneTimer->start(800);

	/* ---- Right: transport controls in individual bordered sections ---- */
	QWidget *rightPanel = new QWidget();
	QVBoxLayout *v = new QVBoxLayout(rightPanel);
	v->setContentsMargins(0, 0, 8, 0); /* right margin for scrollbar */
	QScrollArea *scroll = new QScrollArea();
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setStyleSheet("QScrollArea { background: transparent; }");
	scroll->setWidget(rightPanel);
	split->addWidget(scroll);
	split->setStretchFactor(0, 0);
	split->setStretchFactor(1, 1);
	split->setSizes({200, 400});

	auto title = [](const char *t) {
		QLabel *l = new QLabel(QString::fromUtf8(t));
		l->setStyleSheet("font-weight:bold; color:#9aa0ac;");
		return l;
	};

	/* Information */
	v->addWidget(title(obs_module_text("Dock.SecInfo")));
	QLabel *status = new QLabel(QString::fromUtf8("-"));
	status->setStyleSheet("font-weight:bold;");
	v->addWidget(status);

	/* Direct */
	v->addWidget(title(obs_module_text("Dock.Live")));
	QPushButton *bLive = make_button(obs_module_text("Dock.Live"));
	QObject::connect(bLive, &QPushButton::clicked,
			 [] { warp_set_state(WARP_LIVE); });
	v->addWidget(bLive);

	/* Delay: jump to the configured delay, with a LIVE jog beside it that
	 * scrubs the delay on the fly while playback keeps running (WARP_PLAY). */
	v->addWidget(title(obs_module_text("Dock.Delay")));
	QPushButton *bDelay = make_button(obs_module_text("Dock.Delay"));
	QObject::connect(bDelay, &QPushButton::clicked,
			 [] { warp_set_state(WARP_DELAYED); });
	JogSlider *jogLive = new JogSlider(true);
	jogLive->setToolTip(QString::fromUtf8(obs_module_text("Dock.JogLive")));
	QLabel *dL = new QLabel(QString::fromUtf8("\xC2\xAB")); /* « advance */
	QLabel *dR = new QLabel(QString::fromUtf8("\xC2\xBB")); /* » rewind */
	dL->setStyleSheet("QLabel{color:#8b93a1;font-weight:bold;}");
	dR->setStyleSheet("QLabel{color:#8b93a1;font-weight:bold;}");
	QHBoxLayout *dr = new QHBoxLayout();
	dr->addWidget(bDelay);
	dr->addWidget(dL);
	dr->addWidget(jogLive, 1);
	dr->addWidget(dR);
	v->addLayout(dr);

	/* Transport row 1: Pause + an elastic jog/shuttle (replaces +5s/-5s),
	 * side by side. Drag left = back in the timeline, right = forward
	 * (towards live); the further you push, the faster it scrubs; springs
	 * back to centre on release. */
	QPushButton *bPause = make_button(obs_module_text("Dock.Pause"));
	QObject::connect(bPause, &QPushButton::clicked,
			 [] { warp_toggle_pause(); });
	JogSlider *jog = new JogSlider();
	jog->setToolTip(QString::fromUtf8(obs_module_text("Dock.Jog")));
	QLabel *jL = new QLabel(QString::fromUtf8("\xC2\xAB")); /* « advance */
	QLabel *jR = new QLabel(QString::fromUtf8("\xC2\xBB")); /* » rewind */
	jL->setStyleSheet("QLabel{color:#8b93a1;font-weight:bold;}");
	jR->setStyleSheet("QLabel{color:#8b93a1;font-weight:bold;}");
	QHBoxLayout *jr = new QHBoxLayout();
	jr->addWidget(bPause);
	jr->addWidget(jL);
	jr->addWidget(jog, 1);
	jr->addWidget(jR);
	v->addLayout(jr);

	/* Transport row 2: Play + a 3-position speed selector (x0.5/x1/x2)
	 * side by side. */
	QPushButton *bPlay = make_button(obs_module_text("Dock.PlayDelay"));
	QObject::connect(bPlay, &QPushButton::clicked,
			 [] { warp_play_current(); });

	/* Make Délai direct / Chargement / Jouer all as wide as the longest,
	 * so the three jog/slider rows line up. */
	{
		int bw = bDelay->sizeHint().width();
		if (bPause->sizeHint().width() > bw)
			bw = bPause->sizeHint().width();
		if (bPlay->sizeHint().width() > bw)
			bw = bPlay->sizeHint().width();
		bDelay->setMinimumWidth(bw);
		bPause->setMinimumWidth(bw);
		bPlay->setMinimumWidth(bw);
	}

	QSlider *spd = new QSlider(Qt::Horizontal);
	spd->setRange(0, 2);
	spd->setValue(1);
	spd->setSingleStep(1);
	spd->setPageStep(1);
	spd->setTickPosition(QSlider::TicksBelow);
	spd->setTickInterval(1);
	spd->setToolTip(QString::fromUtf8(obs_module_text("Dock.Speed")));
	/* Slow/fast positions = the configurable decel/accel factors (so they
	 * follow Réglages -> Facteur d'accélération / décélération). */
	QLabel *spdL = new QLabel(QString::fromUtf8("\xC3\x97""0.5"));
	QLabel *spdR = new QLabel(QString::fromUtf8("\xC3\x97""2"));
	QLabel *spdCur = new QLabel(QString::fromUtf8("\xC3\x97""1"));
	for (QLabel *l : {spdL, spdR})
		l->setStyleSheet("QLabel{color:#8b93a1;font-size:10px;}");
	spdCur->setStyleSheet("QLabel{color:#cfd6e2;font-weight:bold;}");
	QObject::connect(spd, &QSlider::valueChanged, [spdCur](int val) {
		WarpStatus st;
		warp_get_status(st);
		const double sp =
			val == 0 ? st.decel : (val == 2 ? st.accel : 1.0);
		spdCur->setText(QString::fromUtf8("\xC3\x97") +
				QString::number(sp, 'g', 3));
		warp_set_play_speed(sp);
	});
	QHBoxLayout *rS = new QHBoxLayout();
	rS->addWidget(bPlay);
	rS->addWidget(spdL);
	rS->addWidget(spd, 1);
	rS->addWidget(spdR);
	rS->addWidget(spdCur);
	v->addLayout(rS);

	v->addSpacing(18);
	v->addStretch();

	/* Settings gear, bottom-right. */
	QHBoxLayout *gearRow = new QHBoxLayout();
	gearRow->addStretch();
	QToolButton *gear = new QToolButton();
	gear->setFixedSize(34, 34);
	gear->setAutoRaise(true);
	gear->setStyleSheet("QToolButton { padding: 0px; border: none; }");
	gear->setIcon(make_gear_icon(QColor("#fefefe"), 16));
	gear->setIconSize(QSize(16, 16));
	gear->setToolTip(QString::fromUtf8(obs_module_text("Dock.Settings")));
	QObject::connect(gear, &QToolButton::clicked, [root] {
		open_settings_dialog(root);
	});
	/* GitHub + donation link button, next to the gear. */
	QToolButton *ghBtn = new QToolButton();
	ghBtn->setFixedSize(34, 34);
	ghBtn->setAutoRaise(true);
	ghBtn->setStyleSheet("QToolButton { padding: 0px; border: none; }");
	ghBtn->setIcon(make_github_icon(18));
	ghBtn->setIconSize(QSize(18, 18));
	ghBtn->setToolTip("GitHub / Support");
	QObject::connect(ghBtn, &QToolButton::clicked, [root] { show_links_popup(root); });
	gearRow->addWidget(ghBtn);
	gearRow->addWidget(gear);
	v->addLayout(gearRow);

	/* Live status + button states, refreshed by a timer. */
	QTimer *timer = new QTimer(root);
	QObject::connect(timer, &QTimer::timeout,
			 [status, bLive, bDelay, bPause, bPlay, jog, jogLive,
			  spd, spdL, spdR, spdCur] {
		WarpStatus st;
		const bool has_docks = warp_get_status(st);
		jog->setDistance(st.distance_s); /* feed the decade-scaled jogs */
		jogLive->setDistance(st.distance_s);

		/* Keep the speed selector in sync with the configured accel/decel
		 * factors (which the user can change in Réglages). */
		spdL->setText(QString::fromUtf8("\xC3\x97") +
			      QString::number(st.decel, 'g', 3));
		spdR->setText(QString::fromUtf8("\xC3\x97") +
			      QString::number(st.accel, 'g', 3));
		spdCur->setText(QString::fromUtf8("\xC3\x97") +
				QString::number(st.play_speed, 'g', 3));
		/* If playing at slow/fast and the factor changed, re-apply live. */
		const int sv = spd->value();
		const double tgt = sv == 0 ? st.decel : (sv == 2 ? st.accel : 1.0);
		if (sv != 1 && st.state == WARP_PLAY &&
		    qAbs(st.play_speed - tgt) > 0.001)
			warp_set_play_speed(tgt);

		/* Guide messages when not fully set up. */
		const char *guide = nullptr;
		if (!has_docks) {
			if (st.active_has_dse)
				guide = obs_module_text("Dock.NoDocks");
			else if (!st.dock_picked)
				guide = obs_module_text("Dock.NoSource");
			else
				guide = obs_module_text("Dock.NoScene");
		} else if (!st.has_scene) {
			guide = !st.active_has_dse
					? obs_module_text("Dock.NoSource")
					: obs_module_text("Dock.NoScene");
		}

		if (guide) {
			status->setText(QString::fromUtf8(guide));
			bLive->setEnabled(false);
			bDelay->setEnabled(false);
			bPause->setEnabled(false);
			bPlay->setEnabled(false);
			return;
		}
		if (!has_docks)
			return;

		static const char *names[] = {"LIVE",  "DELAY", "ACCEL",
					      "DECEL", "PAUSE", "PLAY"};
		static const char *store[] = {"RAM", "Disk", "VRAM"};
		const char *nm = st.buffer_filling
					 ? "BUFFER"
					 : ((st.state >= 0 && st.state < 6)
						    ? names[st.state]
						    : "?");
		const char *sg = (st.storage >= 0 && st.storage < 3)
					 ? store[st.storage]
					 : "?";
		bool attente = !st.has_scene;
		if (attente) {
			const char *msg = obs_module_text(
				!st.active_has_dse
					? "Dock.NoSource"
					: (!st.dock_picked
						   ? "Dock.NoScene"
						   : "Dock.NoDocks"));
			status->setText(QString::fromUtf8(msg));
			nm = "";
		}
		const char *dot = "";
		if (attente) {
			dot = "ATTENTE  ";
		} else if (st.state == WARP_LIVE) {
			dot = QDateTime::currentSecsSinceEpoch() % 2
				      ? "\xF0\x9F\x94\xB4 "
				      : "\xE2\x9A\xAB ";
		} else if (st.state == WARP_DELAYED && !st.buffer_filling) {
			static const char *clocks[12] = {
				"\xF0\x9F\x95\x90 ", "\xF0\x9F\x95\x91 ",
				"\xF0\x9F\x95\x92 ", "\xF0\x9F\x95\x93 ",
				"\xF0\x9F\x95\x94 ", "\xF0\x9F\x95\x95 ",
				"\xF0\x9F\x95\x96 ", "\xF0\x9F\x95\x97 ",
				"\xF0\x9F\x95\x98 ", "\xF0\x9F\x95\x99 ",
				"\xF0\x9F\x95\x9A ", "\xF0\x9F\x95\x9B ",
			};
			dot = clocks[QDateTime::currentSecsSinceEpoch() % 12];
		}
		if (!attente && st.disk_error)
			status->setText(QString::fromUtf8(
				"\xE2\x9A\xA0\xEF\xB8\x8F ") +
				QString::fromUtf8(obs_module_text("Dock.DiskError")));
		else if (!attente)
			status->setText(QString::asprintf(
				"%s%s  -  -%d s / %d s  -  %s", dot, nm,
				(int)(st.distance_s + 0.5),
				(int)(st.target_s + 0.5), sg));

		/* Disable actions that don't make sense in the current state.
		 * At full delay (DELAYED) we are already at the back of the
		 * buffer: no Pause (can't build more) and no -step (can't go
		 * further back). */
		const int s = st.state;
		bLive->setEnabled(s != WARP_LIVE);
		bDelay->setEnabled(s != WARP_DELAYED);
		bPause->setEnabled(s != WARP_PAUSED && s != WARP_DELAYED);
		bPlay->setEnabled(!st.buffer_filling);
	});
	timer->start(150);

	install_wheel_guard(root); /* wheel scrolls the panel, not the values */
	return root;
}




void register_warp_dock()
{
	obs_frontend_add_dock_by_id("delay_source_dock",
				    obs_module_text("Dock.Title"),
				    build_dock_widget());
	keep_dock_on_screen("delay_source_dock");
}
