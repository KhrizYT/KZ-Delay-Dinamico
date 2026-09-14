/*
 * Broadcast Delay - real-time WSOLA time-stretcher (pitch-preserving)
 *
 * Waveform-Similarity Overlap-Add: push input samples + a speed, pull stretched
 * output. speed > 1 compresses (plays faster), speed < 1 expands (slower); pitch
 * is preserved because grains are time-shifted, not resampled.
 *
 * Unlike plain OLA (fixed grain positions -> phasey "robotic" voices), WSOLA
 * searches a small window around each analysis position for the grain whose
 * start best correlates with the *natural continuation* of the previously
 * emitted grain, so successive grains line up in phase and overlap-add
 * constructively. Stable states (speed == 1) bypass this entirely.
 *
 * THREADING: single-threaded (audio thread), guarded by the mixer's mutex.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class TimeStretch {
public:
	void configure(size_t channels)
	{
		if (channels == channels_)
			return;
		channels_ = channels;
		N_ = 2048;	    /* grain size (~43 ms @ 48 kHz) */
		Hs_ = N_ / 2;	    /* synthesis hop (50% overlap) */
		Delta_ = 512;	    /* +/- waveform-similarity search (~10 ms) */
		Lm_ = 512;	    /* correlation match length */
		win_.resize(N_);
		for (size_t i = 0; i < N_; i++)
			win_[i] = (float)(0.5 *
					  (1.0 - std::cos(2.0 * 3.14159265358979 *
							  (double)i /
							  (double)(N_ - 1))));
		norm_ = 1.0f;	    /* Hann @ 50% overlap -> COLA sum ~ 1.0 */
		reset();
	}

	void set_speed(double s) { speed_ = s > 0.01 ? s : 0.01; }

	void reset()
	{
		inbuf_.assign(channels_, std::vector<float>());
		out_.assign(channels_, std::deque<float>());
		acc_.assign(channels_, std::vector<float>(N_, 0.0f));
		in_base_ = 0;
		in_end_ = 0;
		ja_ = 0.0;
		km_ = 0;
		primed_ = false;
	}

	/* Push n planar-float input frames. */
	void push(const float *const data[], size_t n)
	{
		for (size_t ch = 0; ch < channels_; ch++)
			inbuf_[ch].insert(inbuf_[ch].end(), data[ch], data[ch] + n);
		in_end_ += n;
		process();
	}

	/* Pull `frames` stretched frames into out (zero-padded on underflow).
	 * Returns the number of real (non-padded) frames produced. */
	size_t pull(std::vector<std::vector<float>> &out, size_t frames)
	{
		const size_t avail = out_.empty() ? 0 : out_[0].size();
		const size_t got = avail < frames ? avail : frames;
		for (size_t ch = 0; ch < channels_; ch++) {
			out[ch].resize(frames);
			for (size_t i = 0; i < got; i++) {
				out[ch][i] = out_[ch].front();
				out_[ch].pop_front();
			}
			for (size_t i = got; i < frames; i++)
				out[ch][i] = 0.0f;
		}
		return got;
	}

private:
	inline float in_at(size_t ch, int64_t abs) const
	{
		return inbuf_[ch][(size_t)(abs - (int64_t)in_base_)];
	}

	void process()
	{
		if (channels_ == 0)
			return;
		if (!primed_) {
			ja_ = (double)in_base_;
			km_ = in_base_;
			primed_ = true;
		}

		for (;;) {
			const int64_t base = (int64_t)std::floor(ja_);
			const int64_t hi = base + (int64_t)Delta_ + (int64_t)N_;
			const int64_t need_hi =
				std::max(hi, (int64_t)(km_ + (uint64_t)Lm_));
			if (need_hi > (int64_t)in_end_)
				break; /* need more input */

			/* Find the grain start (within +/-Delta of `base`) whose
			 * first Lm samples best match the target continuation km_. */
			int64_t bestoff = base;
			double bestscore = -1e30;
			for (int64_t d = -(int64_t)Delta_; d <= (int64_t)Delta_; d++) {
				const int64_t gs = base + d;
				if (gs < (int64_t)in_base_)
					continue;
				if (gs + (int64_t)Lm_ > (int64_t)in_end_)
					continue;
				double dot = 0.0, eg = 0.0;
				for (size_t i = 0; i < Lm_; i++) {
					const float g = in_at(0, gs + (int64_t)i);
					const float t = in_at(0, (int64_t)km_ +
								      (int64_t)i);
					dot += (double)g * t;
					eg += (double)g * g;
				}
				const double score = dot / std::sqrt(eg + 1e-9);
				if (score > bestscore) {
					bestscore = score;
					bestoff = gs;
				}
			}
			const int64_t gp = bestoff;

			/* Overlap-add the windowed grain, emit Hs finished samples. */
			for (size_t ch = 0; ch < channels_; ch++) {
				std::vector<float> &a = acc_[ch];
				for (size_t i = 0; i < N_; i++)
					a[i] += in_at(ch, gp + (int64_t)i) * win_[i];
				for (size_t i = 0; i < Hs_; i++)
					out_[ch].push_back(a[i] / norm_);
				for (size_t i = 0; i < N_ - Hs_; i++)
					a[i] = a[i + Hs_];
				for (size_t i = N_ - Hs_; i < N_; i++)
					a[i] = 0.0f;
			}

			/* Target for the next grain = natural continuation of this
			 * one; advance analysis by the speed-scaled hop. */
			km_ = (uint64_t)(gp + (int64_t)Hs_);
			ja_ += (double)Hs_ * speed_;

			/* Drop input no longer reachable by the next search/target. */
			const int64_t keep_from =
				std::min((int64_t)std::floor(ja_) - (int64_t)Delta_,
					 (int64_t)km_);
			if (keep_from > (int64_t)in_base_) {
				const size_t drop =
					(size_t)(keep_from - (int64_t)in_base_);
				for (size_t ch = 0; ch < channels_; ch++)
					inbuf_[ch].erase(inbuf_[ch].begin(),
							 inbuf_[ch].begin() + drop);
				in_base_ += drop;
			}
		}
	}

	size_t channels_ = 0;
	size_t N_ = 0, Hs_ = 0, Delta_ = 0, Lm_ = 0;
	double speed_ = 1.0, ja_ = 0.0;
	uint64_t in_base_ = 0, in_end_ = 0, km_ = 0;
	bool primed_ = false;
	float norm_ = 1.0f;
	std::vector<float> win_;
	std::vector<std::vector<float>> inbuf_, acc_;
	std::vector<std::deque<float>> out_;
};
