/*
 * Broadcast Delay - unit tests for the pure (OBS-free) engine modules.
 *
 * Tiny self-contained harness (no GoogleTest dep): VideoRing (RAM frame ring)
 * and TimeStretch (WSOLA). Builds + runs standalone via tests/CMakeLists.txt,
 * with an empty stub <obs.h> on the include path. No OBS / GPU needed.
 */
#include "audio/audio-ring.hpp"
#include "audio/time-stretch.hpp"
#include "delay/video-ring.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static int g_total = 0, g_fail = 0;
#define CHECK(cond)                                                       \
	do {                                                              \
		++g_total;                                                \
		if (!(cond)) {                                            \
			++g_fail;                                         \
			std::printf("  FAIL  L%-4d  %s\n", __LINE__, #cond); \
		}                                                         \
	} while (0)

/* ---- VideoRing: sample / sample2 / wrap / underflow ---- */
static void test_video_ring_basic()
{
	VideoRing r;
	const uint8_t *a = nullptr, *b = nullptr;
	float f = -1.f;

	/* Underflow: empty ring returns nothing. */
	CHECK(!r.ready());
	CHECK(r.sample(0) == nullptr);
	CHECK(!r.sample2(0, &a, &b, &f));

	CHECK(r.configure(2, 2, 3) == true); /* layout changed */
	CHECK(r.ready());
	CHECK(r.capacity() == 3);
	CHECK(r.count() == 0);
	CHECK(r.frame_bytes() == (size_t)2 * 2 * 4);
	CHECK(r.configure(2, 2, 3) == false); /* unchanged -> no flush */
	CHECK(r.sample(100) == nullptr);      /* still no frames written */

	/* Write 3 frames at ts 10/20/30, tag each by its first byte. */
	for (int i = 0; i < 3; i++) {
		uint8_t *p = r.acquire_write((uint64_t)(10 * (i + 1)));
		p[0] = (uint8_t)(i + 1);
	}
	CHECK(r.count() == 3);

	/* sample = newest frame with ts <= target (FIFO). */
	CHECK(r.sample(5) == nullptr);            /* older than oldest (ts10) */
	CHECK(r.sample(10) && r.sample(10)[0] == 1);
	CHECK(r.sample(25) && r.sample(25)[0] == 2);
	CHECK(r.sample(1000) && r.sample(1000)[0] == 3);

	/* sample2 brackets target 25 -> a=ts20, b=ts30, frac=0.5. */
	CHECK(r.sample2(25, &a, &b, &f));
	CHECK(a && a[0] == 2);
	CHECK(b && b[0] == 3);
	CHECK(std::fabs(f - 0.5f) < 1e-4f);

	/* sample2 at the newest -> a only, no b. */
	CHECK(r.sample2(30, &a, &b, &f));
	CHECK(a && a[0] == 3);
	CHECK(b == nullptr);
}

static void test_video_ring_wrap()
{
	VideoRing r;
	r.configure(1, 1, 3);
	/* Write 5 into a cap-3 ring: the oldest two are overwritten. */
	for (int i = 0; i < 5; i++) {
		uint8_t *p = r.acquire_write((uint64_t)(10 * (i + 1)));
		p[0] = (uint8_t)(i + 1);
	}
	CHECK(r.count() == 3); /* capped at capacity */
	/* Live window is now ts30/40/50 (bytes 3/4/5). */
	CHECK(r.sample(5) == nullptr);      /* older than oldest (ts30) */
	CHECK(r.sample(30) && r.sample(30)[0] == 3);
	CHECK(r.sample(45) && r.sample(45)[0] == 4);
	CHECK(r.sample(1000) && r.sample(1000)[0] == 5);
}

static void test_video_ring_edge()
{
	VideoRing r;
	/* A fresh ring is already empty (0x0, no slots), so a degenerate
	 * configure is a no-op and reports "unchanged". */
	CHECK(r.configure(0, 0, 0) == false);
	CHECK(!r.ready());
	CHECK(r.capacity() == 0);

	/* Re-configure to a smaller capacity flushes (count back to 0). */
	r.configure(1, 1, 5);
	for (int i = 0; i < 5; i++)
		r.acquire_write((uint64_t)(i + 1));
	CHECK(r.count() == 5);
	CHECK(r.configure(1, 1, 2) == true); /* layout changed -> flush */
	CHECK(r.capacity() == 2);
	CHECK(r.count() == 0);
	CHECK(r.sample(100) == nullptr); /* flushed: nothing to read */
}

/* ---- TimeStretch (WSOLA): finite output, sane sizing vs speed ---- */
static size_t drain(TimeStretch &ts, bool &all_finite)
{
	std::vector<std::vector<float>> out(1);
	size_t total = 0;
	for (int k = 0; k < 400; k++) {
		size_t got = ts.pull(out, 512);
		for (size_t i = 0; i < got; i++)
			if (!std::isfinite(out[0][i]))
				all_finite = false;
		total += got;
		if (got == 0 && k > 4)
			break;
	}
	return total;
}

static void test_time_stretch()
{
	std::vector<float> sig(20000);
	for (size_t i = 0; i < sig.size(); i++)
		sig[i] = std::sin(0.05 * (double)i);
	const float *in[1] = {sig.data()};

	bool finite1 = true, finite2 = true;

	TimeStretch a;
	a.configure(1);
	a.set_speed(1.0);
	a.push(in, sig.size());
	const size_t out1 = drain(a, finite1);

	TimeStretch b;
	b.configure(1);
	b.set_speed(2.0);
	b.push(in, sig.size());
	const size_t out2 = drain(b, finite2);

	CHECK(finite1);            /* no NaN/Inf at 1x */
	CHECK(finite2);            /* no NaN/Inf at 2x */
	CHECK(out1 > 0);
	CHECK(out2 > 0);
	/* 1x should track the input length within a generous margin. */
	CHECK(out1 > sig.size() / 2);
	CHECK(out1 < sig.size() * 2);
	/* 2x playback consumes input ~twice as fast -> fewer output frames. */
	CHECK(out2 < out1);
}

/* ---- AudioMixer: a DC input passes through (post gain/mute) at steady state ---- */
static float mixer_dc_steady(float gain, bool muted, double speed)
{
	AudioMixer m;
	m.configure(1, 48000);
	m.set_target_delay(50000000ULL); /* 50 ms */

	std::vector<float> block(512, 1.0f); /* constant DC = 1.0, 1 channel */
	const uint8_t *data[1] = {reinterpret_cast<const uint8_t *>(block.data())};
	std::vector<std::vector<float>> out(1);
	uint64_t out_ts = 0;
	const double dt_ns = 512.0 / 48000.0 * 1.0e9;
	uint64_t ts = 1000000ULL;

	float last = 0.f;
	size_t emitted = 0;
	bool finite = true;
	for (int k = 0; k < 300; k++) { /* ~3.2 s of audio -> well past any delay */
		size_t em = m.process(data, 512, ts, gain, muted, speed,
				      50000000LL, true, out, out_ts);
		for (size_t i = 0; i < em; i++) {
			if (!std::isfinite(out[0][i]))
				finite = false;
			last = out[0][i];
		}
		emitted += em;
		ts += (uint64_t)dt_ns;
	}
	CHECK(finite);
	CHECK(emitted > 0);
	return last; /* steady-state emitted sample */
}

static void test_audio_mixer()
{
	/* Unmuted, gain 1.0: DC passes through (~1.0). */
	CHECK(std::fabs(mixer_dc_steady(1.0f, false, 1.0) - 1.0f) < 0.05f);
	/* Gain 0.5 halves it. */
	CHECK(std::fabs(mixer_dc_steady(0.5f, false, 1.0) - 0.5f) < 0.05f);
	/* Muted -> silence. */
	CHECK(std::fabs(mixer_dc_steady(1.0f, true, 1.0)) < 0.05f);
	/* Stretched (speed 2x) DC is still DC (~1.0), just time-scaled. */
	CHECK(std::fabs(mixer_dc_steady(1.0f, false, 2.0) - 1.0f) < 0.1f);
}

/* Stereo: both channels carry their own DC through (post gain). */
static void test_audio_mixer_stereo()
{
	AudioMixer m;
	m.configure(2, 48000);
	CHECK(m.channels() == 2);
	m.set_target_delay(50000000ULL);

	std::vector<float> chL(512, 1.0f), chR(512, -0.5f); /* distinct per channel */
	const uint8_t *data[2] = {reinterpret_cast<const uint8_t *>(chL.data()),
				  reinterpret_cast<const uint8_t *>(chR.data())};
	std::vector<std::vector<float>> out(2);
	uint64_t out_ts = 0;
	const double dt_ns = 512.0 / 48000.0 * 1.0e9;
	uint64_t ts = 1000000ULL;
	float lastL = 0.f, lastR = 0.f;
	bool finite = true;
	for (int k = 0; k < 300; k++) {
		size_t em = m.process(data, 512, ts, 1.0f, false, 1.0,
				      50000000LL, true, out, out_ts);
		for (size_t i = 0; i < em; i++) {
			if (!std::isfinite(out[0][i]) || !std::isfinite(out[1][i]))
				finite = false;
			lastL = out[0][i];
			lastR = out[1][i];
		}
		ts += (uint64_t)dt_ns;
	}
	CHECK(finite);
	CHECK(std::fabs(lastL - 1.0f) < 0.05f);
	CHECK(std::fabs(lastR + 0.5f) < 0.05f);
}

int main()
{
	std::printf("Broadcast Delay - pure module unit tests\n");
	test_video_ring_basic();
	test_video_ring_wrap();
	test_video_ring_edge();
	test_time_stretch();
	test_audio_mixer();
	test_audio_mixer_stereo();
	std::printf("\n%d/%d checks passed%s\n", g_total - g_fail, g_total,
		    g_fail ? "  <<< FAILURES" : "");
	return g_fail ? 1 : 0;
}
