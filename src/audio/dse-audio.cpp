/*
 * Broadcast Delay - audio capture + delayed mix/emit (see dse-audio.hpp).
 */
#include "dse-audio.hpp"

#include "../core/dse-internal.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <cmath>

/* Pull the whole pre-delay program mix from the mixer at the live frontier and
 * hand it to the public LIVE audio taps. Caller holds s->audio_mutex (so the
 * mixer state is consistent) and has just processed the clock block. */
static void dispatch_live_mix(DelayedSource *s, size_t channels, size_t rate)
{
	thread_local std::vector<std::vector<float>> livemix;
	const size_t lookback = rate ? rate / 33 : 1500; /* ~30 ms: let sources land */
	uint64_t lts = 0;
	size_t got = s->mixer.read_live(livemix, rate ? rate : 48000, lookback, lts);
	if (got == 0)
		return;
	const float *planar[MAX_AV_PLANES] = {};
	for (size_t c = 0; c < channels && c < MAX_AV_PLANES; c++)
		planar[c] = livemix[c].data();
	bd_dispatch_live_audio(s, planar, (uint32_t)got, (uint32_t)channels,
			       (uint32_t)rate, lts);
}

/* V2.5: recibe directamente una mezcla completa de OBS (una pista).
 * AquÃ­ ya vienen aplicados filtros, volumen, balance y la mezcla nativa.
 * Al no volver a sumar fuentes individualmente evitamos el audio Ã¡spero /
 * saturado que podÃ­a producir la ruta experimental anterior. */
static void program_mix_audio_cb(void *param, size_t mix_idx,
                                 struct audio_data *audio)
{
	auto *s = static_cast<DelayedSource *>(param);
	if (!s || !audio || !s->enabled.load(std::memory_order_relaxed))
		return;

	struct obs_audio_info oai;
	if (!obs_get_audio_info(&oai))
		return;

	const size_t channels = get_audio_channels(oai.speakers);
	const size_t rate = oai.samples_per_sec;
	if (channels == 0 || rate == 0 || audio->frames == 0)
		return;

	/* V2.6: reloj de escritura contiguo.
	 *
	 * OBS entrega la mezcla final en bloques consecutivos. Para escribirlos
	 * dentro del ring contamos muestras exactas (0, 1024, 2048...) en lugar
	 * de volver a calcular la posicion usando timestamps redondeados.
	 * El timestamp real de OBS se conserva para la SALIDA. */
	thread_local DelayedSource *program_clock_source = nullptr;
	thread_local uint64_t program_clock_base_ts = 0;
	thread_local uint64_t program_clock_frames = 0;

	if (program_clock_source != s || program_clock_base_ts == 0) {
		program_clock_source = s;
		program_clock_base_ts = audio->timestamp;
		program_clock_frames = 0;
	}

	const uint64_t whole_sec =
		program_clock_frames / (uint64_t)rate;
	const uint64_t rem_frames =
		program_clock_frames % (uint64_t)rate;
	const uint64_t ring_offset_ns =
		whole_sec * 1000000000ULL +
		(rem_frames * 1000000000ULL + (uint64_t)rate - 1ULL) /
			(uint64_t)rate;
	const uint64_t ring_ts =
		program_clock_base_ts + ring_offset_ns;
	program_clock_frames += audio->frames;
	const uint8_t *planes[MAX_AV_PLANES] = {};
	for (size_t ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++)
		planes[ch] = audio->data[ch];

	std::lock_guard<std::mutex> lock(s->audio_mutex);
	s->mixer.configure(channels, rate);
	s->mixer.set_target_delay(
		s->delay_ns.load(std::memory_order_relaxed));

	if (s->audio_out.size() < channels)
		s->audio_out.resize(channels);

	uint64_t out_ts = 0;
	const size_t emitted = s->mixer.process(
		planes, audio->frames, ring_ts, 1.0f, false,
		s->audio_speed.load(std::memory_order_relaxed),
		s->audio_snap_ns.load(std::memory_order_relaxed), true,
		s->audio_out, out_ts);

	if (emitted == 0)
		return;

	struct obs_source_audio out = {};
	for (size_t ch = 0; ch < channels; ch++)
		out.data[ch] = reinterpret_cast<const uint8_t *>(
			s->audio_out[ch].data());
	out.frames = (uint32_t)emitted;
	out.speakers = oai.speakers;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = (uint32_t)rate;
	out.timestamp = audio->timestamp; /* reloj real de OBS */

	obs_source_output_audio(s->self, &out);

	UNUSED_PARAMETER(mix_idx);
}
/* Every captured audio source feeds this one callback, which mixes them and
 * emits the delayed result. Runs on the OBS audio thread(s). */
static void audio_capture_cb(void *param, obs_source_t *source,
			     const struct audio_data *audio, bool muted)
{
	auto *s = static_cast<DelayedSource *>(param);

	if (!s->enabled.load(std::memory_order_relaxed))
		return; /* disabled -> no audio output */

	struct obs_audio_info oai;
	if (!obs_get_audio_info(&oai))
		return;
	const size_t channels = get_audio_channels(oai.speakers);
	const size_t rate = oai.samples_per_sec;

	/* Apply the source's fader so the mix matches OBS's own levels. */
	const float gain = source ? obs_source_get_volume(source) : 1.0f;

	std::lock_guard<std::mutex> lock(s->audio_mutex);

	s->mixer.configure(channels, rate);
	s->mixer.set_target_delay(s->delay_ns.load(std::memory_order_relaxed));

	if (s->audio_out.size() < channels)
		s->audio_out.resize(channels);

	const double speed = s->audio_speed.load(std::memory_order_relaxed);
	const int64_t snap = s->audio_snap_ns.load(std::memory_order_relaxed);
	obs_source_t *clk = s->audio_clock.load(std::memory_order_relaxed);
	bool allow_emit = (clk == nullptr) || (clk == source);

#ifdef _WIN32
	/* Native-OS mode: the WASAPI capture threads mix + emit directly
	 * (feed_hw_audio_direct). OBS source callbacks must not also emit. */
	if (!s->audio_auto)
		return;
#endif

	uint64_t out_ts = 0;
	const size_t emitted = s->mixer.process(audio->data, audio->frames,
						audio->timestamp, gain, muted,
						speed, snap, allow_emit,
						s->audio_out, out_ts);

	/* Feed the LIVE audio taps the WHOLE pre-delay program mix (every captured
	 * source summed), pulled from the mixer at the live frontier. Once per clock
	 * tick; cheap no-op when nobody is tapping. */
	if (allow_emit)
		dispatch_live_mix(s, channels, rate);

	static std::atomic<uint64_t> s_last_log{0};
	const uint64_t nowlog = os_gettime_ns();
	uint64_t prev = s_last_log.load(std::memory_order_relaxed);
	if (nowlog - prev > 2000000000ULL &&
	    s_last_log.compare_exchange_strong(prev, nowlog)) {
		blog(LOG_INFO,
		     "[delay-audio] src='%s' clock=%s frames=%u emit=%zu rate=%zu ch=%zu gain=%.2f muted=%d speed=%.3f snap_ms=%lld",
		     source ? obs_source_get_name(source) : "?",
		     allow_emit ? "yes" : "no", (unsigned)audio->frames, emitted,
		     rate, channels, gain, muted ? 1 : 0, speed,
		     (long long)(snap / 1000000));
	}

	if (emitted == 0)
		return;

	struct obs_source_audio out = {};
	for (size_t ch = 0; ch < channels; ch++)
		out.data[ch] = reinterpret_cast<const uint8_t *>(
			s->audio_out[ch].data());
	out.frames = (uint32_t)emitted;
	out.speakers = oai.speakers;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = (uint32_t)rate;
	out.timestamp = out_ts;
	obs_source_output_audio(s->self, &out);
}

#ifdef _WIN32
/* Mix + emit a captured WASAPI block straight from the capture thread. Every
 * non-muted device feeds the same delay mixer; only the clock device emits the
 * summed output. obs_source_output_audio is thread-safe. */
static void feed_hw_audio_direct(DelayedSource *s, const std::string &devName,
				 int uid, const float *const *planar,
				 size_t frames, uint64_t ts)
{
	if (!s->enabled.load(std::memory_order_relaxed) || frames == 0)
		return;
	struct obs_audio_info oai;
	if (!obs_get_audio_info(&oai))
		return;
	const size_t channels = get_audio_channels(oai.speakers);
	const size_t rate = oai.samples_per_sec;

	/* Dynamic clock arbitration: this device emits if it owns the clock, or
	 * if the clock is free / its owner has gone idle (>150 ms). Only one
	 * device can win the CAS, so emission never doubles. Liveness uses the
	 * real wall clock (the audio `ts` is a per-device monotonic timeline and
	 * is not comparable across devices). */
	const uint64_t now = (uint64_t)os_gettime_ns();
	bool isClock = false;
	int owner = s->hw_clock_owner.load(std::memory_order_relaxed);
	if (owner == uid) {
		isClock = true;
		s->hw_clock_seen.store(now, std::memory_order_relaxed);
	} else {
		const uint64_t seen = s->hw_clock_seen.load(std::memory_order_relaxed);
		if (owner < 0 || now - seen > 150000000ULL) {
			int expected = owner;
			if (s->hw_clock_owner.compare_exchange_strong(expected, uid)) {
				isClock = true;
				s->hw_clock_seen.store(now, std::memory_order_relaxed);
			}
		}
	}

	std::lock_guard<std::mutex> lock(s->audio_mutex);
	s->mixer.configure(channels, rate);
	s->mixer.set_target_delay(s->delay_ns.load(std::memory_order_relaxed));
	if (s->audio_out.size() < channels)
		s->audio_out.resize(channels);

	const double speed = s->audio_speed.load(std::memory_order_relaxed);
	const int64_t snap = s->audio_snap_ns.load(std::memory_order_relaxed);
	const float gain = mixer_state_gain(*s->mixer_state, devName);

	/* Optional per-device mono downmix + L/R balance (pan). When either is
	 * active we build a per-channel scratch buffer; otherwise pass through. */
	const bool wantMono = mixer_state_mono(*s->mixer_state, devName);
	const float bal = mixer_state_balance(*s->mixer_state, devName);
	const float gL = bal <= 0.f ? 1.f : 1.f - bal;
	const float gR = bal >= 0.f ? 1.f : 1.f + bal;
	const float *eff[MAX_AV_PLANES] = {};
	for (size_t c = 0; c < channels; c++)
		eff[c] = planar[c];
	if ((wantMono || bal != 0.f) && channels >= 1) {
		thread_local std::vector<std::vector<float>> buf;
		if (buf.size() < channels) buf.resize(channels);
		for (size_t c = 0; c < channels; c++) buf[c].resize(frames);
		for (size_t i = 0; i < frames; i++) {
			float l, r;
			if (wantMono) {
				float sum = 0.f;
				for (size_t c = 0; c < channels; c++)
					sum += planar[c][i];
				l = r = sum / (float)channels;
			} else {
				l = planar[0][i];
				r = channels > 1 ? planar[1][i] : planar[0][i];
			}
			buf[0][i] = l * gL;
			if (channels > 1) buf[1][i] = r * gR;
			for (size_t c = 2; c < channels; c++)
				buf[c][i] = planar[c][i];
		}
		for (size_t c = 0; c < channels; c++)
			eff[c] = buf[c].data();
	}

	/* Live monitoring: hear this device (post-gain, pre-delay) right now. */
	if (mixer_state_monitor(*s->mixer_state, devName))
		AudioMonitor::push(eff, channels, frames, gain);

	const uint8_t *data[MAX_AV_PLANES] = {};
	for (size_t c = 0; c < channels; c++)
		data[c] = reinterpret_cast<const uint8_t *>(eff[c]);

	uint64_t out_ts = 0;
	const size_t emitted = s->mixer.process(data, (uint32_t)frames, ts, gain,
						false, speed, snap, isClock,
						s->audio_out, out_ts);

	/* Feed the LIVE audio taps the WHOLE pre-delay program mix (every device
	 * summed), pulled from the mixer at the live frontier. Clock device only. */
	if (isClock)
		dispatch_live_mix(s, channels, rate);

	{
		/* Per-thread throttle so every device (incl. the clock) logs. */
		thread_local uint64_t last = 0;
		const uint64_t now = os_gettime_ns();
		if (now - last > 3000000000ULL) {
			last = now;
			float inL = 0.f, inR = 0.f;
			for (size_t i = 0; i < frames && i < 4096; i++) {
				float a = std::fabs(planar[0][i]);
				if (a > inL) inL = a;
				if (channels > 1) {
					float b = std::fabs(planar[1][i]);
					if (b > inR) inR = b;
				}
			}
			float outL = 0.f, outR = 0.f;
			if (emitted && s->audio_out.size() >= channels) {
				for (size_t i = 0; i < emitted && i < 4096; i++) {
					float a = std::fabs(s->audio_out[0][i]);
					if (a > outL) outL = a;
					if (channels > 1) {
						float b = std::fabs(s->audio_out[1][i]);
						if (b > outR) outR = b;
					}
				}
			}
			blog(LOG_INFO,
			     "[hw-feed] dev='%s' clk=%d ch=%zu inL=%.4f inR=%.4f outL=%.4f outR=%.4f gain=%.2f emit=%zu",
			     devName.c_str(), isClock ? 1 : 0, channels, inL, inR,
			     outL, outR, gain, emitted);
		}
	}

	if (!isClock || emitted == 0)
		return;

	struct obs_source_audio out = {};
	for (size_t c = 0; c < channels; c++)
		out.data[c] = reinterpret_cast<const uint8_t *>(
			s->audio_out[c].data());
	out.frames = (uint32_t)emitted;
	out.speakers = oai.speakers;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = (uint32_t)rate;
	out.timestamp = out_ts;
	obs_source_output_audio(s->self, &out);
}

/* Capture every non-muted device automatically (no enable step); muting stops a
 * device's capture. Emission is driven by a dynamically-claimed clock device
 * (see feed_hw_audio_direct), so the order devices start in does not matter. */
void reconcile_hw(DelayedSource *s)
{
	if (s->audio_auto) {
		if (!s->hw_caps.empty()) {
			for (auto &h : s->hw_caps)
				if (h.cap) h.cap->stop();
			s->hw_caps.clear();
			s->hw_active.store(false, std::memory_order_relaxed);
			s->hw_sel_key.clear();
			s->hw_clock_owner.store(-1, std::memory_order_relaxed);
		}
		AudioMonitor::set_active(false, 0, 0);
		return;
	}

	struct obs_audio_info oai_m;
	size_t mch = 2, msr = 48000;
	if (obs_get_audio_info(&oai_m)) {
		mch = get_audio_channels(oai_m.speakers);
		msr = oai_m.samples_per_sec;
	}

	struct Want { std::string id, name; bool is_input; };
	std::vector<Want> want;
	std::string key;
	bool anyMonitor = false;
	{
		std::lock_guard<std::mutex> lk(s->mixer_state->mtx);
		for (const MixerChannel &c : s->mixer_state->chans) {
			if (c.monitor)
				anyMonitor = true;
			if (c.muted || c.id.empty())
				continue;
			want.push_back({c.id, c.name, c.is_input});
			key += c.id + "\n";
		}
	}

	/* Toggling monitor/mute every tick is cheap + idempotent. */
	AudioMonitor::set_active(anyMonitor, mch, msr);

	if (key == s->hw_sel_key)
		return; /* unchanged */
	s->hw_sel_key = key;

	for (auto &h : s->hw_caps)
		if (h.cap) h.cap->stop();
	s->hw_caps.clear();
	s->hw_clock_owner.store(-1, std::memory_order_relaxed); /* re-elect */

	struct obs_audio_info oai;
	size_t ch = 2, sr = 48000;
	if (obs_get_audio_info(&oai)) {
		ch = get_audio_channels(oai.speakers);
		sr = oai.samples_per_sec;
	}

	int next_uid = 1;
	for (const Want &w : want) {
		DelayedSource::HwCap hc;
		hc.id = w.id; hc.name = w.name; hc.uid = next_uid++;
		hc.cap.reset(new DeviceCapture());
		const std::string devName = w.name;
		const int uid = hc.uid;
		hc.cap->start(w.id.c_str(), w.is_input, ch, sr,
			      [s, devName, uid, ch](const float *data[],
						    size_t frames, uint64_t ts) {
				      feed_hw_audio_direct(s, devName, uid, data,
							   frames, ts);
				      float pl = 0.f, pr = 0.f;
				      for (size_t i = 0; i < frames; i++) {
					      float a = std::fabs(data[0][i]);
					      if (a > pl) pl = a;
					      if (ch > 1) {
						      float b = std::fabs(data[1][i]);
						      if (b > pr) pr = b;
					      }
				      }
				      if (ch <= 1) pr = pl;
				      /* Mono: both meters show the averaged level
				       * (2 synced bars), so the balance can still
				       * push them apart L/R. */
				      if (mixer_state_mono(*s->mixer_state, devName)) {
					      float m = 0.5f * (pl + pr);
					      pl = pr = m;
				      }
				      /* Apply L/R balance to the meter too. */
				      const float bl = mixer_state_balance(
					      *s->mixer_state, devName);
				      pl *= bl <= 0.f ? 1.f : 1.f - bl;
				      pr *= bl >= 0.f ? 1.f : 1.f + bl;
				      /* Show POST-gain level so the fader + mute
				       * visibly move the meter (0 when muted). */
				      const float g = mixer_state_gain(
					      *s->mixer_state, devName);
				      mixer_state_set_level(*s->mixer_state, devName,
							    pl * g, pr * g);
			      });
		s->hw_caps.push_back(std::move(hc));
	}
	s->hw_active.store(!s->hw_caps.empty(), std::memory_order_relaxed);
	blog(LOG_INFO, "[reconcile_hw] capturing %zu device(s), clock auto-elected",
	     s->hw_caps.size());
}
#endif

/* Add a (strong-ref) source to the capture list, de-duplicated by pointer.
 * Takes ownership of one ref; releases it if it is a duplicate. */
static void add_audio_unique(std::vector<obs_source_t *> *list, obs_source_t *src)
{
	if (!src)
		return;
	for (obs_source_t *existing : *list) {
		if (existing == src) {
			obs_source_release(src);
			return;
		}
	}
	list->push_back(src);
}

/* The global audio devices (Desktop Audio, Mic/Aux) live on OBS output channels
 * 1.. and are NOT scene items, so a scene render must pull them in explicitly to
 * delay "everything you hear". Channel 0 is the program video. */
static void collect_global_audio(std::vector<obs_source_t *> *list)
{
	for (uint32_t ch = 1; ch <= 8; ch++) {
		obs_source_t *src = obs_get_output_source(ch); /* strong ref */
		if (!src)
			continue;
		if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)
			add_audio_unique(list, src);
		else
			obs_source_release(src);
	}
}

/* Drop the current audio capture set. Caller holds target_mutex. */
void detach_audio_locked(DelayedSource *s)
{
	if (s->program_mix_attached) {
		obs_remove_raw_audio_callback(s->program_mix_idx,
					      program_mix_audio_cb, s);
		s->program_mix_attached = false;
	}
	for (obs_source_t *src : s->audio_sources) {
		obs_source_remove_audio_capture_callback(src, audio_capture_cb, s);
		obs_source_release(src);
	}
	s->audio_sources.clear();
	s->audio_clock.store(nullptr, std::memory_order_relaxed);
}

/* (Re)build the captured audio set. Manual selection (audio_auto == false) takes
 * exactly the chosen sources by name -- which lets the user drop the desktop
 * loopback that feeds back when the delayed output is monitored. Otherwise a
 * Scene pulls the global devices and a Source uses itself. Holds target_mutex. */
void gather_audio_locked(DelayedSource *s)
{
	detach_audio_locked(s);

	/* V2.5 program mix path: OBS ya hizo toda la mezcla. No adjuntamos
	 * callbacks fuente-por-fuente y, muy importante, no reseteamos el ring
	 * al cambiar de escena/pista. */
	if (s->program_mix_audio) {
		obs_add_raw_audio_callback(s->program_mix_idx, nullptr,
					   program_mix_audio_cb, s);
		s->program_mix_attached = true;
		s->audio_clock.store(nullptr, std::memory_order_relaxed);
		return;
	}
	if (!s->audio_auto) {
		/* WASAPI mode: attach global audio as clock to drive the
		 * callback.  OBS audio is replaced by WASAPI data when
		 * hw_active is true. */
		collect_global_audio(&s->audio_sources);
	} else if (!s->audio_sel.empty()) {
		/* Experimental OBS mode: capture exactly the checked sources. */
		for (const std::string &nm : s->audio_sel) {
			obs_source_t *src = obs_get_source_by_name(nm.c_str());
			if (src)
				add_audio_unique(&s->audio_sources, src);
		}
		/* If none of the checked sources resolved, fall back to global. */
		if (s->audio_sources.empty())
			collect_global_audio(&s->audio_sources);
	} else if (s->target && obs_source_is_scene(s->target)) {
		collect_global_audio(&s->audio_sources);
	} else if (s->target &&
		   (obs_source_get_output_flags(s->target) & OBS_SOURCE_AUDIO)) {
		add_audio_unique(&s->audio_sources, obs_source_get_ref(s->target));
	} else {
		/* Experimental mode with nothing else resolvable: use global. */
		collect_global_audio(&s->audio_sources);
	}

	/* First attached source is the emission clock (steady block per tick);
	 * the rest only feed the mix. */
	s->audio_clock.store(s->audio_sources.empty() ? nullptr
						      : s->audio_sources.front(),
			     std::memory_order_relaxed);

	for (obs_source_t *src : s->audio_sources)
		obs_source_add_audio_capture_callback(src, audio_capture_cb, s);

	{
		std::lock_guard<std::mutex> alock(s->audio_mutex);
		s->mixer.reset();
	}
}
/* Cambia la pista nativa que alimenta el ring sin borrar lo ya almacenado.
 * En directo capturamos Track 1. Al activar retardo movemos las fuentes a
 * un bus libre (normalmente Track 6) y capturamos ese bus; KZ puede entonces
 * salir por Track 1 sin realimentarse a sÃ­ mismo. */
void set_program_mix_track(DelayedSource *s, size_t mix_idx)
{
	if (!s || mix_idx >= MAX_AUDIO_MIXES)
		return;

	std::lock_guard<std::mutex> lock(s->target_mutex);
	if (s->program_mix_audio && s->program_mix_attached &&
	    s->program_mix_idx == mix_idx)
		return;

	const bool first_enable = !s->program_mix_audio;
	detach_audio_locked(s);

	s->program_mix_audio = true;
	s->program_mix_idx = mix_idx;
	obs_add_raw_audio_callback(mix_idx, nullptr, program_mix_audio_cb, s);
	s->program_mix_attached = true;
	s->audio_clock.store(nullptr, std::memory_order_relaxed);

	if (first_enable) {
		std::lock_guard<std::mutex> alock(s->audio_mutex);
		s->mixer.reset();
	}

	blog(LOG_INFO,
	     "[kz-delay-dinamico] audio nativo OBS: capturando Track %zu",
	     mix_idx + 1);
}
