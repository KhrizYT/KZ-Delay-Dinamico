/*
 * Broadcast Delay - OBS audio mixer (exact values from Yami theme)
 */
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QWidget>
#include <QMainWindow>
#include <QDockWidget>
#include <QDialog>
#include <QEvent>
#include <QString>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSlider>
#include <QPushButton>
#include <QTimer>
#include <QCheckBox>
#include <QLabel>
#include <QPainter>
#include <QFontMetrics>
#include <QFrame>
#include <QPixmap>
#include <QMouseEvent>
#include <QStyleOptionSlider>
#include <QSvgRenderer>
#include <QApplication>
#include <QPalette>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/mixer-state.hpp"
#include "ui/vu-meter.hpp"

/* 0 = Bureau (default speaker), 1 = default mic, 2 = other mics, 3 = speakers. */
static int mixer_category(const MixerChannel &c) {
	if (!c.is_input && c.is_default) return 0; /* Bureau (desktop audio) */
	if (c.is_input && c.is_default)  return 1; /* Micro (défaut) */
	return c.is_input ? 2 : 3;
}
void mixer_state_set_devices(MixerState &st,
	const std::vector<std::string> &names, const std::vector<std::string> &ids,
	const std::vector<bool> &inputs, const std::vector<bool> &defaults)
{
	std::lock_guard lk(st.mtx);
	std::vector<MixerChannel> next; next.reserve(names.size());
	for (size_t i = 0; i < names.size(); i++) {
		MixerChannel c; c.name = names[i];
		c.id = i < ids.size() ? ids[i] : "";
		c.is_input = i < inputs.size() ? inputs[i] : false;
		c.is_default = i < defaults.size() ? defaults[i] : false;
		/* Default: only "Bureau" (default speaker) and the default mic are
		 * active; everything else starts MUTED - avoids duplicate/feedback
		 * from the many virtual output cables. */
		const bool isBureau = !c.is_input && c.is_default;
		const bool isDefaultMic = c.is_input && c.is_default;
		c.muted = !(isBureau || isDefaultMic);
		for (auto &o : st.chans) if (o.name == c.name)
			{ c.volume = o.volume; c.muted = o.muted; c.enabled = o.enabled;
			  c.monitor = o.monitor; c.mono = o.mono; c.balance = o.balance;
			  c.level_l = o.level_l; c.level_r = o.level_r; break; }
		next.push_back(c);
	}
	/* Bureau first, then microphones, then other speakers (stable within). */
	std::stable_sort(next.begin(), next.end(),
		[](const MixerChannel &a, const MixerChannel &b){ return mixer_category(a) < mixer_category(b); });
	st.chans = std::move(next);
}
std::string mixer_state_first_enabled_id(MixerState &st) {
	std::lock_guard lk(st.mtx); for (auto &c:st.chans) if(c.enabled&&!c.id.empty()) return c.id; return {};
}
float mixer_state_gain(MixerState &st, const std::string &name) {
	std::lock_guard lk(st.mtx); for (auto &c:st.chans) if(c.name==name) return c.muted?0.f:c.volume; return 1.f;
}
bool mixer_state_monitor(MixerState &st, const std::string &name) {
	std::lock_guard lk(st.mtx); for (auto &c:st.chans) if(c.name==name) return c.monitor; return false;
}
bool mixer_state_mono(MixerState &st, const std::string &name) {
	std::lock_guard lk(st.mtx); for (auto &c:st.chans) if(c.name==name) return c.mono; return false;
}
float mixer_state_balance(MixerState &st, const std::string &name) {
	std::lock_guard lk(st.mtx); for (auto &c:st.chans) if(c.name==name) return c.balance; return 0.f;
}
void mixer_state_set_level(MixerState &st, const std::string &name, float l, float r) {
	std::lock_guard lk(st.mtx); for (auto &c:st.chans) if(c.name==name){c.level_l=l;c.level_r=r;break;}
}
void mixer_state_get_level(MixerState &st, const std::string &name, float &l, float &r) {
	std::lock_guard lk(st.mtx); for (auto &c:st.chans) if(c.name==name){l=c.level_l;r=c.level_r;return;} l=r=0.f;
}

/* OBS SVG icons */
static const char *kSpkSVG = R"(
<svg height="16" width="16" viewBox="0 0 16 16" xmlns="http://www.w3.org/2000/svg">
<path d="M7 1c-.3 0-.58.13-.77.35L3 5H2C.9 5 0 5.84 0 7v2c0 1.09.91 2 2 2h1l3.23 3.64c.21.25.49.36.77.36zM13.46 1.97c-.19 0-.39.05-.56.17-.46.31-.58.93-.27 1.39 1.82 2.7 1.82 6.24 0 8.94-.31.46-.19 1.08.27 1.39s1.08.19 1.39-.27c1.14-1.69 1.7-3.64 1.7-5.59s-.56-3.9-1.7-5.59c-.2-.29-.51-.44-.83-.44zM10.04 3.99c-.22 0-.45.06-.64.2-.26.2-.4.5-.4.8v.06c.01.19.07.38.2.54 1.07 1.43 1.07 3.39 0 4.82-.13.16-.19.35-.2.53v.06c0 .31.13.61.4.81.44.33 1.06.24 1.39-.2.8-1.07 1.21-2.34 1.21-3.61s-.4-2.54-1.2-3.61c-.2-.25-.47-.38-.76-.4z" fill="#fefefe"/>
</svg>)";
static const char *kMuteSVG = R"(
<svg height="16" width="16" viewBox="0 0 16 16" xmlns="http://www.w3.org/2000/svg">
<g fill="#c01c28">
<path d="M7 1c-.3 0-.58.13-.77.35L3 5H2C.9 5 0 5.84 0 7v2c0 1.09.91 2 2 2h1l3.23 3.64c.21.25.49.36.77.36z"/>
<path d="M10 5c-.27 0-.52.1-.7.3-.4.38-.4 1.02 0 1.4l1.3 1.3-1.3 1.3c-.4.38-.4 1.02 0 1.4.38.4 1.02.4 1.4 0l1.3-1.3 1.3 1.3c.38.4 1.02.4 1.4 0 .4-.38.4-1.02 0-1.4l-1.3-1.3 1.3-1.3c.4-.38.4-1.02 0-1.4-.19-.19-.44-.3-.7-.3s-.52.11-.7.3l-1.3 1.3-1.3-1.3c-.18-.19-.44-.3-.7-.3z"/>
</g>
</svg>)";
static const char *kHPonSVG = R"(
<svg height="16" width="16" viewBox="0 0 16 16" xmlns="http://www.w3.org/2000/svg">
<path fill="#fefefe" d="M8 0C6.8 0 5.6.3 4.5.9 2.3 2.2 1 4.5 1 7v6s0 2 2 2h1c.6 0 1-.4 1-1v-4c0-.6-.5-1-1-1S3 8 3 7V6c0-1.8 1-3.4 2.5-4.3 1.5-.9 3.5-.9 5 0C12 2.6 13 4.2 13 6v1c0 1-1 1-1 1-.5 0-1 .4-1 1v4c0 .6.4 1 1 1h1c2 0 2-2 2-2V7c0-2.5-1.3-4.8-3.5-6.1C10.4.6 9.2.3 8 0z"/>
</svg>)";
static const char *kHPoffSVG = R"(
<svg height="16" width="16" viewBox="0 0 16 16" xmlns="http://www.w3.org/2000/svg">
<path fill="#fefefe" opacity="0.45" d="M8 0C6.8 0 5.6.3 4.5.9 2.3 2.2 1 4.5 1 7v6s0 2 2 2h1c.6 0 1-.4 1-1v-4c0-.6-.5-1-1-1S3 8 3 7V6c0-1.8 1-3.4 2.5-4.3 1.5-.9 3.5-.9 5 0C12 2.6 13 4.2 13 6v1c0 1-1 1-1 1-.5 0-1 .4-1 1v4c0 .6.4 1 1 1h1c2 0 2-2 2-2V7c0-2.5-1.3-4.8-3.5-6.1C10.4.6 9.2.3 8 0z"/>
<path fill="none" stroke="#fefefe" stroke-width="1.7" stroke-linecap="round" d="M2.5 2.5 13.5 13.5"/>
</svg>)";

static QIcon mkIcon(const char *svg, int sz = 14) {
	QByteArray ba(svg); QSvgRenderer r(ba);
	qreal dpr = qApp->devicePixelRatio();
	QPixmap pm(qRound(sz*dpr), qRound(sz*dpr));
	pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
	QPainter pt(&pm);
	pt.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
	/* Render into an explicit LOGICAL rect so the SVG is scaled to fit. The
	 * no-arg render(&pt) maps to the device viewport, which then gets the
	 * devicePixelRatio applied again -> oversized icon clipped bottom-right. */
	r.render(&pt, QRectF(0, 0, sz, sz));
	pt.end();
	return QIcon(pm);
}

/* ---------- fader ---------- */
class MixFader : public QSlider {
public:
	MixFader(QWidget *p = nullptr) : QSlider(Qt::Vertical, p) {
		setRange(0,1000); setValue(1000); setFixedWidth(26);
		/* Static blue groove: it does NOT grow/shrink with the handle.
		 * Only the white handle moves to show the level. */
		setStyleSheet("QSlider::groove:vertical{background:#476BD7;border:none;border-radius:2px;width:4px;}"
			"QSlider::add-page:vertical{background:#476BD7;border:none;border-radius:2px;}"
			"QSlider::sub-page:vertical{background:#476BD7;border:none;border-radius:2px;}"
			"QSlider::handle:vertical{background:#FFF;border:none;border-radius:4px;width:10px;height:20px;margin:0 -3px;}"
			"QSlider::handle:vertical:hover{background:#B4B4B4;}"
			"QSlider::handle:vertical:pressed{background:#D2D2D2;}");
	}
	/* Top (v=1000) = 0 dB, bottom (v=0) = -100 dB = -inf (silent). */
	static float toGain(int v) { if(v<=0) return 0.f; float db=-100.f*(1.f-v/1000.f); return std::pow(10.f,db/20.f); }
	static int fromGain(float g) { if(g<=0.00001f) return 0; float db=20.f*std::log10(g); if(db<-100.f)db=-100.f; if(db>0.f)db=0.f; return (int)((1.f+db/100.f)*1000.f); }
	void setFromY(float y) {
		QStyleOptionSlider o; initStyleOption(&o);
		QRect gv=style()->subControlRect(QStyle::CC_Slider,&o,QStyle::SC_SliderGroove,this);
		QRect hd=style()->subControlRect(QStyle::CC_Slider,&o,QStyle::SC_SliderHandle,this);
		int sH=gv.height()-hd.height();
		if (sH<=0) return;
		setValue((int)(std::clamp((gv.bottom()-y-hd.height()/2.f)/(float)sH,0.f,1.f)*1000.f));
	}
protected:
	/* Click-to-jump AND drag: enter slider-down so the panel timer stops
	 * overwriting the value while the user moves it. */
	void mousePressEvent(QMouseEvent *ev) override {
		if (ev->button()==Qt::LeftButton) {
			setSliderDown(true);
			setFromY((float)ev->position().y());
			ev->accept(); return;
		}
		QSlider::mousePressEvent(ev);
	}
	void mouseMoveEvent(QMouseEvent *ev) override {
		if (isSliderDown()) { setFromY((float)ev->position().y()); ev->accept(); return; }
		QSlider::mouseMoveEvent(ev);
	}
	void mouseReleaseEvent(QMouseEvent *ev) override {
		if (ev->button()==Qt::LeftButton && isSliderDown()) {
			setSliderDown(false); ev->accept(); return;
		}
		QSlider::mouseReleaseEvent(ev);
	}
	/* Double-click resets to 0 dB (unity gain). */
	void mouseDoubleClickEvent(QMouseEvent *ev) override {
		if (ev->button()==Qt::LeftButton) {
			setSliderDown(false); setValue(1000); ev->accept(); return;
		}
		QSlider::mouseDoubleClickEvent(ev);
	}
	/* Plain groove + blue fill + handle (no dB graduation ticks). */
};

/* ---------- balance (L/R pan) ---------- */
class BalanceSlider : public QSlider {
public:
	BalanceSlider(QWidget *p = nullptr) : QSlider(Qt::Horizontal, p) {
		setRange(-100, 100); setValue(0); setFixedHeight(16);
		setStyleSheet("QSlider::groove:horizontal{background:#476BD7;border:none;border-radius:2px;height:4px;}"
			"QSlider::add-page:horizontal{background:#476BD7;border:none;border-radius:2px;}"
			"QSlider::sub-page:horizontal{background:#476BD7;border:none;border-radius:2px;}"
			"QSlider::handle:horizontal{background:#FFF;border:none;border-radius:4px;width:8px;height:14px;margin:-5px 0;}"
			"QSlider::handle:horizontal:hover{background:#B4B4B4;}");
	}
protected:
	/* Double-click recenters the balance. */
	void mouseDoubleClickEvent(QMouseEvent *) override { setValue(0); }
};

/* ---------- strip ---------- */
struct SW { QWidget *w; QLabel *db; QLabel *nm; QPushButton *mt,*mon,*st; MixFader *fd; VuMeter *vu; BalanceSlider *bal; std::string nmStr; bool isIn; };
static const char *kBtnQSS =
	"QPushButton{background:transparent;border:1px solid transparent;border-radius:4px;padding:3px;margin:0 1px;outline:none;}"
	"QPushButton:hover{background:rgba(255,255,255,0.08);}";
static const char *kMonQSS =
	"QPushButton{background:transparent;border:1px solid transparent;border-radius:4px;padding:3px;margin:0 1px;outline:none;}"
	"QPushButton:checked{background:#17641E;border-color:transparent;}"
	"QPushButton:hover{background:rgba(255,255,255,0.08);}"
	"QPushButton:checked:hover{background:#1a7a22;}";

static SW mkStrip(const std::string &name, std::shared_ptr<MixerState> st, bool is_input, bool is_default) {
	SW sw; sw.nmStr=name; sw.isIn = is_input;
	sw.w=new QWidget(); sw.w->setFixedWidth(110);
	auto *L=new QVBoxLayout(sw.w); L->setContentsMargins(0,0,0,0); L->setSpacing(0);

	/* Category badge - Bureau / Micro (défaut) / Micro / Haut-parleur. */
	const bool isBureau = !is_input && is_default;
	const bool isDefMic = is_input && is_default;
	const char *badgeTxt = isBureau ? obs_module_text("Mixer.Desktop")
			      : isDefMic ? obs_module_text("Mixer.MicDefault")
			      : (is_input ? obs_module_text("Mixer.Mic")
					  : obs_module_text("Mixer.Speaker"));
	const char *badgeCol = isBureau ? "#0E7A6B"
			      : is_input ? "#6B21A8" : "#1A3278";
	auto *badge = new QLabel(badgeTxt);
	badge->setAlignment(Qt::AlignCenter);
	badge->setFixedHeight(16);
	badge->setStyleSheet(QString("QLabel{color:#e7eaef;font-size:8px;font-weight:bold;"
		"background:%1;padding:2px 0;}").arg(badgeCol));
	L->addWidget(badge);

	sw.nm=new QLabel(QString::fromUtf8(name.c_str()));
	sw.nm->setAlignment(Qt::AlignHCenter|Qt::AlignTop); sw.nm->setWordWrap(true);
	sw.nm->setMinimumHeight(40); sw.nm->setMaximumHeight(56);
	sw.nm->setToolTip(QString::fromUtf8(name.c_str()));
	sw.nm->setStyleSheet("QLabel{font-size:10px;font-weight:600;padding:2px 4px;border-bottom:1px solid #3C404D;}");
	L->addWidget(sw.nm,0);
	L->addSpacing(4);

	sw.db=new QLabel("0 dB"); sw.db->setAlignment(Qt::AlignCenter);
	sw.db->setStyleSheet("QLabel{color:#8b93a1;font-size:11px;font-weight:500;padding:2px 0;}");

	auto *mf=new QFrame(); mf->setObjectName("volMeterFrame");
	mf->setStyleSheet("QFrame#volMeterFrame{padding:4px 0;}");
	sw.fd=new MixFader(); sw.fd->setMinimumHeight(90); /* more compressible */
	sw.fd->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::MinimumExpanding);
	sw.vu = new VuMeter();

	/* Fader + VU side by side */
	auto *meterLayout = new QHBoxLayout(); meterLayout->setContentsMargins(0,0,0,0); meterLayout->setSpacing(6);
	meterLayout->addStretch(); meterLayout->addWidget(sw.fd); meterLayout->addWidget(sw.vu); meterLayout->addStretch();

	/* dB row - same horizontal offset as meterLayout so dB sits above fader */
	auto *dbRow = new QHBoxLayout(); dbRow->setContentsMargins(0,0,0,0); dbRow->setSpacing(6);
	dbRow->addStretch(); dbRow->addWidget(sw.db); 
	auto *vuSpacer = new QWidget(); vuSpacer->setFixedSize(sw.vu->sizeHint().width(), 1);
	dbRow->addWidget(vuSpacer); dbRow->addStretch();

	auto *mfInner = new QVBoxLayout(mf); mfInner->setContentsMargins(0,0,0,0); mfInner->setSpacing(2);
	mfInner->addLayout(dbRow);
	mfInner->addLayout(meterLayout);
	L->addWidget(mf,1);

	auto *br=new QHBoxLayout(); br->setContentsMargins(0,2,0,4); br->setSpacing(4);
	br->addStretch();

	sw.mt=new QPushButton(); sw.mt->setCheckable(true);
	sw.mt->setFixedSize(22,22);
	sw.mt->setStyleSheet("QPushButton{background:transparent;border:1px solid transparent;border-radius:4px;padding:0;margin:0;outline:none;}"
		"QPushButton:hover{background:rgba(192,28,40,0.15);border:1px solid rgba(192,28,40,0.55);}"
		"QPushButton:checked{background:rgba(192,28,40,0.22);border:1px solid rgba(192,28,40,0.7);}"
		"QPushButton:checked:hover{background:rgba(192,28,40,0.34);border:1px solid #c01c28;}");
	sw.mt->setIcon(mkIcon(kSpkSVG,16)); sw.mt->setIconSize(QSize(16,16));
	QObject::connect(sw.mt,&QPushButton::toggled,[st,name,mt=sw.mt](bool b){
		mt->setIcon(mkIcon(b?kMuteSVG:kSpkSVG,16));
		std::lock_guard lk(st->mtx); for(auto &c:st->chans)if(c.name==name){c.muted=b;break;} });
	br->addWidget(sw.mt);

	/* Monitor: hear this device live through the monitor output. */
	sw.mon=new QPushButton(); sw.mon->setCheckable(true);
	sw.mon->setFixedSize(22,22);
	sw.mon->setStyleSheet("QPushButton{background:transparent;border:1px solid transparent;border-radius:4px;padding:0;margin:0;outline:none;}"
		"QPushButton:checked{background:#17641E;border:1px solid #2a9d3a;}"
		"QPushButton:hover{background:rgba(255,255,255,0.06);border:1px solid rgba(255,255,255,0.22);}"
		"QPushButton:checked:hover{background:#1a7a22;}");
	sw.mon->setIcon(mkIcon(kHPoffSVG,16)); sw.mon->setIconSize(QSize(16,16));
	sw.mon->setToolTip(isBureau
		? obs_module_text("Mixer.MonitorWarn")
		: obs_module_text("Mixer.Monitor"));
	QObject::connect(sw.mon,&QPushButton::toggled,[st,name,mon=sw.mon](bool b){
		mon->setIcon(mkIcon(b?kHPonSVG:kHPoffSVG,16));
		std::lock_guard lk(st->mtx); for(auto &c:st->chans)if(c.name==name){c.monitor=b;break;} });
	br->addWidget(sw.mon);

	/* Stereo / mono switch (text: ST = stereo, M = mono downmix). */
	sw.st=new QPushButton("ST"); sw.st->setCheckable(true);
	sw.st->setFixedSize(22,22);
	sw.st->setStyleSheet("QPushButton{background:transparent;border:1px solid transparent;border-radius:4px;padding:0;margin:0;outline:none;color:#cfd6e2;font-size:11px;font-weight:bold;}"
		"QPushButton:hover{background:rgba(71,107,215,0.30);border:1px solid #476BD7;color:#fff;}"
		"QPushButton:checked{background:#476BD7;border:1px solid #476BD7;color:#fff;}"
		"QPushButton:checked:hover{background:#3550A8;border:1px solid #3550A8;color:#fff;}");
	sw.st->setToolTip(obs_module_text("Mixer.StereoMono"));
	QObject::connect(sw.st,&QPushButton::toggled,[st,name,b1=sw.st](bool b){
		b1->setText(b?"M":"ST");
		std::lock_guard lk(st->mtx); for(auto &c:st->chans)if(c.name==name){c.mono=b;break;} });
	br->addWidget(sw.st);
	br->addStretch();
	L->addLayout(br);

	/* Balance L<->R, under the buttons (double-click the handle to center). */
	auto *balRow=new QHBoxLayout(); balRow->setContentsMargins(6,0,6,4); balRow->setSpacing(4);
	auto mkLbl=[](const char *t){ auto *l=new QLabel(t);
		l->setStyleSheet("QLabel{color:#8b93a1;font-size:9px;font-weight:bold;}"); return l; };
	sw.bal=new BalanceSlider();
	sw.bal->setToolTip(obs_module_text("Mixer.Balance"));
	balRow->addWidget(mkLbl("L"));
	balRow->addWidget(sw.bal,1);
	balRow->addWidget(mkLbl("R"));
	L->addLayout(balRow);
	QObject::connect(sw.bal,&QSlider::valueChanged,[st,name](int v){
		std::lock_guard lk(st->mtx); for(auto &c:st->chans)if(c.name==name){c.balance=v/100.f;break;} });

	QObject::connect(sw.fd,&QSlider::valueChanged,[st,name](int v){
		std::lock_guard lk(st->mtx); for(auto &c:st->chans)if(c.name==name){c.volume=MixFader::toGain(v);break;} });
	return sw;
}

/* ---------- panel ---------- */
static QWidget *panel(std::shared_ptr<MixerState> st) {
	auto *rt=new QWidget(); auto *o=new QVBoxLayout(rt); o->setContentsMargins(0,0,0,0);
	auto *sc=new QScrollArea(); sc->setWidgetResizable(true);
	sc->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	sc->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); /* compress: scroll when short */
	sc->setFrameShape(QFrame::NoFrame);
	/* Paint the WHOLE mixer with the OBS DOCK background colour, the same one the
	 * Diffusion dock uses. In the OBS theme QSS that is `OBSDock > QWidget {
	 * background: var(--bg_base) }` = Yami #272A33 (grey6) - NOT --bg_window
	 * (#1D1F26) which palette(Window) resolves to. The VU meters fill the same
	 * colour, so the whole mixer is one uniform dock-coloured background. */
	const QString win = "#272A33"; /* Yami --bg_base (OBS dock background) */
	sc->setStyleSheet(QString("QScrollArea{background:%1;border:none;}"
		"QScrollArea>QWidget>QWidget{background:%1;}").arg(win));
	auto *strips=new QWidget();
	strips->setStyleSheet("QWidget#stripWidget{padding-bottom:4px;}");
	auto *sl=new QHBoxLayout(strips); sl->setContentsMargins(0,0,0,0); sl->setSpacing(0);
	auto slist=std::make_shared<std::vector<SW>>();
	auto *t=new QTimer(rt);
	QObject::connect(t,&QTimer::timeout,[strips,slist,st]{
		auto *sl=qobject_cast<QHBoxLayout*>(strips->layout()); if(!sl) return;
		std::vector<MixerChannel> ch; { std::lock_guard lk(st->mtx); ch=st->chans; }
		bool same=ch.size()==slist->size();
		for(size_t i=0;same&&i<ch.size();i++) if((*slist)[i].nmStr!=ch[i].name) same=false;
		if(!same) {
			blog(LOG_INFO,"[mixer-panel] rebuild: %zu devices",ch.size());
			QLayoutItem *it; while((it=sl->takeAt(0))){ if(it->widget()) it->widget()->deleteLater(); delete it; }
			slist->clear();
			for(auto &c:ch){auto s=mkStrip(c.name,st,c.is_input,c.is_default);
				s.w->setObjectName("stripWidget");slist->push_back(s);sl->addWidget(s.w);
				if(&c != &ch.back()){
					/* Separator = the same themed background colour (blends
					 * in) + no fixed height so the strips can compress. */
					auto *div = new QWidget(); div->setFixedWidth(6);
					div->setAutoFillBackground(true);
					div->setBackgroundRole(QPalette::Window);
					sl->addWidget(div);
				}
			}
			/* All titles share the height of the tallest wrapped name. */
			QFont nf; nf.setPixelSize(10); nf.setWeight(QFont::DemiBold);
			QFontMetrics fm(nf); int maxH=22;
			for(auto &c:ch){
				QRect br=fm.boundingRect(QRect(0,0,98,1000),
					Qt::TextWordWrap|Qt::AlignHCenter,
					QString::fromUtf8(c.name.c_str()));
				maxH=std::max(maxH, br.height()+14); /* +padding+border */
			}
			for(auto &s:*slist) s.nm->setFixedHeight(maxH);
			sl->addStretch(); return;
		}
		for(auto &s:*slist) {
			float v=1.f,bal=0.f;bool mu=false,mon=false,mono=false;
			{std::lock_guard lk(st->mtx);
				for(auto &c:st->chans)if(c.name==s.nmStr){v=c.volume;mu=c.muted;mon=c.monitor;mono=c.mono;bal=c.balance;break;}}
			s.mt->blockSignals(true);s.mt->setChecked(mu);
			s.mt->setIcon(mkIcon(mu?kMuteSVG:kSpkSVG,16));s.mt->blockSignals(false);
			s.mon->blockSignals(true);s.mon->setChecked(mon);
			s.mon->setIcon(mkIcon(mon?kHPonSVG:kHPoffSVG,16));s.mon->blockSignals(false);
			s.st->blockSignals(true);s.st->setChecked(mono);s.st->setText(mono?"M":"ST");s.st->blockSignals(false);
			if(!s.fd->isSliderDown())s.fd->setValue(MixFader::fromGain(v));
			if(!s.bal->isSliderDown()){s.bal->blockSignals(true);s.bal->setValue((int)(bal*100.f));s.bal->blockSignals(false);}
			float db=v>0.00001f?20.f*std::log10(v):-200.f;
			s.db->setText(db<=-100.f?QString("-inf dB"):QString::asprintf("%.1f dB",(double)db));
			/* Feed audio levels to VU meter (always 2 synced bars; balance makes them differ). */
			float l=0,r=0;
			{std::lock_guard lk(st->mtx);
			 for(auto &c:st->chans)if(c.name==s.nmStr){l=c.level_l;r=c.level_r;break;}}
			s.vu->setLevels(l,r);
		}
	});
	t->start(33); sc->setWidget(strips); o->addWidget(sc); return rt;
}

/* When the user closes the mixer dock, unregister it from OBS so it stops being
 * listed in View > Docks (re-added on the next "Open Audio Mixer"). */
class MixerDockCloser : public QObject {
public:
	std::string id;
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type()==QEvent::Close) {
			std::string did=id;
			QMetaObject::invokeMethod(qApp, [did]{
				obs_frontend_remove_dock(did.c_str());
			}, Qt::QueuedConnection);
		}
		return QObject::eventFilter(o,e);
	}
};

void mixer_show_dialog(std::shared_ptr<MixerState> st, const char *title,
		       const char *id) {
	auto *mw=(QMainWindow*)obs_frontend_get_main_window();
	if(!mw) return;
	/* Stable, OBS-safe dock id per source (obs_frontend_add_dock_by_id sets it
	 * as the QDockWidget's objectName, so we can re-surface it on later clicks). */
	std::string did="delay_mixer_";
	for(const char *p=(id&&*id)?id:"x"; *p; ++p)
		did += std::isalnum((unsigned char)*p)?*p:'_';
	if(auto *ex=mw->findChild<QDockWidget*>(QString::fromStdString(did))) {
		ex->show(); ex->raise(); return; /* already added: just surface it */
	}
	/* Register a NATIVE OBS dock: the user can float it or dock it anywhere in
	 * the OBS UI, and OBS remembers its place. */
	obs_frontend_add_dock_by_id(did.c_str(),
		title?title:"Mixer", panel(st));
	if(auto *dk=mw->findChild<QDockWidget*>(QString::fromStdString(did))) {
		dk->setFloating(true);
		dk->resize(640,380);
		/* Centre it on the main window so the title bar is always reachable. */
		const QRect g=mw->geometry();
		dk->move(g.center().x()-320, g.center().y()-190);
		/* Drop it from OBS's dock list when closed. */
		auto *closer=new MixerDockCloser(); closer->id=did;
		closer->setParent(dk); dk->installEventFilter(closer);
		dk->show(); dk->raise();
	}
}
void register_mixer_dock() {}
