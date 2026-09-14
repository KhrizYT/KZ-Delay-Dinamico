/*
 * Broadcast Delay - VuMeter: EXACT OBS VolumeMeter rendering code
 * Now with dynamic peak bars fed from WASAPI audio levels.
 */
#include <QWidget>
#include <QPainter>
#include <QPixmap>
#include <QFontMetrics>
#include <QPalette>
#include <cmath>
#include <algorithm>

class VuMeter : public QWidget {
	static constexpr int INDICATOR_THICKNESS = 3;
	static constexpr int TICK_SIZE = 2;
	static constexpr int TICK_DB_INTERVAL = 6;
	static constexpr int BAR_HOLD_H = 3;
	static constexpr const char *TICK_LABEL_TOKEN = "-88";

	QPixmap bgCache;
	int displayCh = 2;
	int meterThick = 6;
	qreal minLvl = -60.0, warnLvl = -20.0, errLvl = -9.0, clipLvl = 0.0;
	QSize tickTokenSize;

	QColor bgNom = QColor(0x26, 0x7F, 0x26);
	QColor bgWarn = QColor(0x7F, 0x7F, 0x26);
	QColor bgErr = QColor(0x7F, 0x26, 0x26);
	QColor fgNom = QColor(0x4C, 0xFF, 0x4C);
	QColor fgWarn = QColor(0xFF, 0xFF, 0x4C);
	QColor fgErr = QColor(0xFF, 0x4C, 0x4C);
	QColor majorTick = QColor(0x96, 0x96, 0x96);
	QColor magColor = QColor(0x00, 0x00, 0x00);

	float peakL = 0.f, peakR = 0.f;
	float holdL = 0.f, holdR = 0.f;
	bool monoMode = false;

	inline int cvt(float n) {
		constexpr int mn = std::numeric_limits<int>::min();
		constexpr int mx = std::numeric_limits<int>::max();
		if (n >= (float)mx) return mx;
		if (n < mn) return mn;
		return int(n);
	}

	void paintVTicks(QPainter &p, int x, int y, int height) {
		qreal sc = height / minLvl;
		QFont f; f.setPixelSize(7); f.setWeight(QFont::Bold);
		p.setFont(f);
		QFontMetrics fm(f);
		p.setPen(majorTick);
		for (int i = 0; i >= (int)minLvl; i -= TICK_DB_INTERVAL) {
			int pos = y + int(i * sc);
			QString s = QString::number(i);
			if (i == 0) p.drawText(x + 10, pos + fm.capHeight(), s);
			else p.drawText(x + 8, pos + fm.capHeight() / 2, s);
			p.drawLine(x, pos, x + TICK_SIZE, pos);
		}
	}

	void updateBg() {
		int w = width(), h = height();
		if (w <= 0 || h <= 0 || displayCh <= 0) return;

		bgCache = QPixmap(size() * devicePixelRatioF());
		bgCache.setDevicePixelRatio(devicePixelRatioF());
		/* The OBS dock background colour (Yami --bg_base = #272A33), same as the
		 * rest of the mixer, so the graduation area blends in. The meter trough +
		 * ticks are drawn on top. */
		bgCache.fill(QColor(0x27, 0x2A, 0x33));

		QPainter p(&bgCache);
		QRect wr = rect();

		paintVTicks(p, displayCh * (meterThick + 1) - 1, 0,
			    wr.height() - (INDICATOR_THICKNESS + 3));

		int meterLen = wr.height() - (INDICATOR_THICKNESS + 2);
		qreal sc = meterLen / minLvl;
		int warnPos = meterLen - cvt(warnLvl * sc);
		int errPos = meterLen - cvt(errLvl * sc);
		int nomLen = warnPos;
		int warnLen = nomLen + (errPos - warnPos);

		const int nc = monoMode ? 1 : displayCh;
		const int tw = monoMode ? (displayCh * (meterThick + 1) - 1)
					: meterThick;
		for (int ch = 0; ch < nc; ch++) {
			int off = ch * (meterThick + 1);
			p.fillRect(off, meterLen, tw, -meterLen, bgErr);
			p.fillRect(off, meterLen, tw, -warnLen, bgWarn);
			p.fillRect(off, meterLen, tw, -nomLen, bgNom);
		}
	}

	void updTokenSize() {
		QFont f; f.setPixelSize(7); f.setWeight(QFont::Bold);
		QFontMetrics fm(f);
		tickTokenSize = fm.size(Qt::TextSingleLine, TICK_LABEL_TOKEN);
	}

public:
	VuMeter(QWidget *p = nullptr) : QWidget(p) {
		setAttribute(Qt::WA_OpaquePaintEvent);
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
		updTokenSize();
	}

	/* When mono, draw a single wide bar instead of two identical L/R bars. */
	void setMono(bool m) {
		if (monoMode == m) return;
		monoMode = m;
		bgCache = QPixmap();
		update();
	}

	/* amplitude (0..1) -> bar fraction (0..1) over the [minLvl, 0] dB scale. */
	float ampFrac(float a) {
		float db = a > 1e-5f ? 20.f * std::log10(a) : (float)minLvl;
		if (db < (float)minLvl) db = (float)minLvl;
		if (db > 0.f) db = 0.f;
		return (float)((db - minLvl) / (0.0 - minLvl));
	}
	/* l/r are raw amplitudes; peakL/holdL store the bar FRACTION (0..1).
	 * Tuned for a ~30 fps refresh: instant attack, smooth ~0.4 s release. */
	void setLevels(float l, float r) {
		float fl = ampFrac(l), fr = ampFrac(r);
		peakL = fl > peakL ? fl : peakL * 0.90f; /* fast attack, smooth release */
		peakR = fr > peakR ? fr : peakR * 0.90f;
		holdL = fl > holdL ? fl : std::max(0.f, holdL - 0.006f);
		holdR = fr > holdR ? fr : std::max(0.f, holdR - 0.006f);
		update();
	}

	/* Keep the width (ticks + labels) but let the height shrink so the meter /
	 * dB grid area can be compressed when the dock is short. */
	QSize minimumSizeHint() const override { return QSize(sizeHint().width(), 40); }
	QSize sizeHint() const override {
		int bw = displayCh * (meterThick + 1) - 1;
		int labelTot = std::abs((int)(minLvl / TICK_DB_INTERVAL)) + 1;
		int w = bw + tickTokenSize.width() + TICK_SIZE + 10;
		int h = (int)(labelTot * (tickTokenSize.height() * 0.8f)) + INDICATOR_THICKNESS;
		return QSize(w, h);
	}
protected:
	void resizeEvent(QResizeEvent *) override { updateBg(); }
	void paintEvent(QPaintEvent *) override {
		if (bgCache.isNull()) updateBg();
		if (bgCache.isNull()) return;

		QPainter p(this);
		p.drawPixmap(0, 0, bgCache);

		/* Dynamic bars: peakL/holdL are fractions (0..1, 1 = 0 dB top).
		 * Bars rise from the bottom, coloured by the same zone boundaries
		 * as the dim background. */
		const int meterLen = height() - (INDICATOR_THICKNESS + 2);
		const float warnFrac = (float)((warnLvl - minLvl) / (0.0 - minLvl));
		const float errFrac = (float)((errLvl - minLvl) / (0.0 - minLvl));
		const int yWarn = (int)(meterLen * (1.f - warnFrac)); /* green/amber */
		const int yErr = (int)(meterLen * (1.f - errFrac));   /* amber/red */

		/* Mono: one wide bar using the (already equal) left value. */
		const float frac[2] = {peakL, monoMode ? peakL : peakR};
		const float hold[2] = {holdL, monoMode ? holdL : holdR};
		const int nc = monoMode ? 1 : displayCh;
		const int tw = monoMode ? (displayCh * (meterThick + 1) - 1)
					: meterThick;

		for (int ch = 0; ch < nc; ch++) {
			const int off = ch * (meterThick + 1);
			const int barTop = (int)(meterLen * (1.f - frac[ch]));

			const int gy = std::max(barTop, yWarn);
			if (gy < meterLen)
				p.fillRect(off, gy, tw, meterLen - gy, fgNom);
			if (barTop < yWarn) {
				const int ay = std::max(barTop, yErr);
				p.fillRect(off, ay, tw, yWarn - ay, fgWarn);
			}
			if (barTop < yErr)
				p.fillRect(off, barTop, tw, yErr - barTop, fgErr);

			if (hold[ch] > 0.001f) {
				const int hy = (int)(meterLen * (1.f - hold[ch]));
				const QColor hc = hy <= yErr ? fgErr
						  : hy <= yWarn ? fgWarn
								: fgNom;
				p.fillRect(off, hy - 1, tw, BAR_HOLD_H, hc);
			}
		}
	}
};
