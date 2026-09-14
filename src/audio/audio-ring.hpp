/*
 * Broadcast Delay - multi-source audio delay mixer (time-warp aware)
 *
 * Sources are summed (gain-scaled) into a timestamp-addressed ring. Playback
 * reads from the ring at a position (next_emit_) that advances at the time-warp
 * speed, so the audio delay tracks the video playhead:
 *   - speed == 1  -> 1:1 passthrough (clean), the read lag stays constant.
 *   - speed  > 1  -> reads faster (lag shrinks) + time-stretch to keep pitch.
 *   - speed  < 1  -> reads slower (lag grows) + time-stretch to keep pitch.
 * A snap_delay (>= 0) jumps the read position so the audio lag matches a target
 * delay (used for Live / Delay / Play / seeks). snap < 0 = free-run via speed.
 *
 * EMISSION CLOCK: every attached source accumulates, but only ONE designated
 * "clock" source (allow_emit == true) drives output, emitting one steady block
 * per tick. The others merely add their audio into the ring. Because playback
 * reads `need` samples behind the newest input, the other sources are already
 * mixed in by the time their region is read.
 *
 * OUTPUT TIMESTAMP: each emitted block is stamped with the clock source's own
 * input timestamp (ts). Since exactly one block is emitted per clock callback,
 * the stamps stay contiguous AND locked to the same clock OBS uses -- a
 * free-running sample counter would slowly drift against the audio device
 * clock and make OBS periodically resync (clicks).
 *
 * THREADING: not internally locked; the owner serialises calls with its mutex.
 */
#pragma once

#include <obs.h>
#include <media-io/audio-io.h>
#include "audio/time-stretch.hpp"
#include <cstdint>
#include <vector>

class AudioMixer {
public:
	void configure(size_t channels, size_t rate)
	{
		if (channels == channels_ && rate == rate_)
			return;
		channels_ = channels;
		rate_ = rate;
		stretch_.configure(channels);
		reset();
	}

	void set_target_delay(uint64_t d)
	{
		const size_t ds =
			rate_ ? (size_t)((d * (uint64_t)rate_) / 1000000000ULL) : 0;
		if (ds > delay_samples_) {
			delay_samples_ = ds;
			reset();
		} else {
			delay_samples_ = ds;
		}
	}

	size_t channels() const { return channels_; }

	void reset()
	{
		L_ = delay_samples_ * 2 + (rate_ ? rate_ : 48000) + 8192;
		buf_.assign(channels_, std::vector<float>(L_, 0.0f));
		started_ = false;
		emitting_ = false;
		stretch_active_ = false;
		base_ts_ = 0;
		zeroed_upto_ = 0;
		live_read_ = 0;
		next_emit_ = 0;
		last_snap_ = -3;
		read_frac_ = 0.0;
		stretch_.reset();
	}

	/*
	 * Accumulate one source block. If allow_emit, also emit one output block
	 * (`frames` long) at the warp speed. Non-clock sources pass allow_emit
	 * false and only contribute to the mix.
	 */
	size_t process(const uint8_t *const data[], size_t frames, uint64_t ts,
		       float gain, bool muted, double speed,
		       int64_t snap_delay_ns, bool allow_emit,
		       std::vector<std::vector<float>> &out, uint64_t &out_ts)
	{
		if (channels_ == 0 || L_ == 0 || frames == 0)
			return 0;
		if (!started_) {
			started_ = true;
			base_ts_ = ts;
			emitting_ = false;
		}
		if (ts < base_ts_)
			return 0;

		const uint64_t idx =
			((ts - base_ts_) * (uint64_t)rate_) / 1000000000ULL;

		/* Clear each ring slot exactly once, as the write frontier first
		 * reaches it, so sources can accumulate (+=) into a clean slot.
		 * Reads are NON-destructive (see below), so the ring retains the
		 * last L_ samples of mixed audio -> switching LIVE->DELAY finds the
		 * already-buffered past instead of re-filling from scratch. */
		const uint64_t end_pos = idx + frames;
		if (end_pos > zeroed_upto_) {
			uint64_t from = zeroed_upto_;
			if (end_pos - from > (uint64_t)L_)
				from = end_pos - (uint64_t)L_; /* huge jump: clear ring */
			for (uint64_t p = from; p < end_pos; p++) {
				const size_t b = (size_t)(p % L_);
				for (size_t ch = 0; ch < channels_; ch++)
					buf_[ch][b] = 0.0f;
			}
			zeroed_upto_ = end_pos;
		}

		/* Accumulate this source's block into the shared ring. The valid
		 * window is the whole ring behind the frontier (so we keep the
		 * past), not just from the read head. */
		if (!muted && gain > 0.0f) {
			const uint64_t floor = zeroed_upto_ > (uint64_t)L_
						       ? zeroed_upto_ - (uint64_t)L_
						       : 0;
			for (size_t i = 0; i < frames; i++) {
				const uint64_t pos = idx + i;
				if (pos < floor || pos >= floor + L_)
					continue;
				const size_t b = (size_t)(pos % L_);
				for (size_t ch = 0; ch < channels_; ch++)
					buf_[ch][b] += gain *
						reinterpret_cast<const float *>(
							data[ch])[i];
			}
		}

		/* Only the clock source produces output. */
		if (!allow_emit)
			return 0;

		const size_t snap_samples =
			snap_delay_ns >= 0
				? (size_t)(((uint64_t)snap_delay_ns *
					    (uint64_t)rate_) / 1000000000ULL)
				: 0;

		if (!emitting_) {
			const size_t need =
				snap_delay_ns >= 0 ? snap_samples : delay_samples_;
			if (idx < need)
				return 0;
			emitting_ = true;
			next_emit_ = idx - need;
			last_snap_ = -3;
		}

		/* Snap every tick to stay locked to the video playhead.
		 * Only reposition when the target actually moved, to avoid
		 * jitter from idx rounding. */
		if (snap_delay_ns >= 0) {
			const size_t target_pos =
				idx > snap_samples ? idx - snap_samples : 0;
			if (snap_delay_ns != last_snap_) {
				/* LIVE->DELAY (or any snap change): just jump the
				 * read head back; the past is retained in the ring
				 * (non-destructive reads), so no re-wait / silence. */
				next_emit_ = target_pos;
				read_frac_ = 0.0;
				stretch_active_ = false;
			} else if (target_pos != next_emit_) {
				/* Small drift correction: snap silently. */
				next_emit_ = target_pos;
			}
		}
		last_snap_ = snap_delay_ns;

		if (speed <= 0.0)
			return 0; /* paused: emit nothing */

		/* Consume `speed * frames` input to produce `frames` real-time output. */
		double want = speed * (double)frames + read_frac_;
		size_t in_n = (size_t)(want < 0.0 ? 0.0 : want);
		read_frac_ = want - (double)in_n;
		const uint64_t avail_end = idx + frames;
		if (next_emit_ + in_n > avail_end)
			in_n = next_emit_ < avail_end
				       ? (size_t)(avail_end - next_emit_)
				       : 0;

		if (scratch_.size() < channels_)
			scratch_.resize(channels_);
		for (size_t ch = 0; ch < channels_; ch++)
			scratch_[ch].resize(in_n ? in_n : 1);
		for (size_t i = 0; i < in_n; i++) {
			const size_t b = (size_t)((next_emit_ + i) % L_);
			for (size_t ch = 0; ch < channels_; ch++)
				scratch_[ch][i] = buf_[ch][b]; /* non-destructive */
		}
		next_emit_ += in_n;

		/* Stamp with the clock source's own input time -> locked to OBS's
		 * clock, no free-running drift. */
		out_ts = ts;

		const bool unity = speed > 0.999 && speed < 1.001;
		if (unity && in_n == frames) {
			stretch_active_ = false;
			for (size_t ch = 0; ch < channels_; ch++) {
				out[ch].resize(frames);
				for (size_t i = 0; i < frames; i++)
					out[ch][i] = scratch_[ch][i];
			}
			return frames;
		}

		/* Time-stretch path (accel/decel, or the rare edge-clamp). */
		if (!stretch_active_) {
			stretch_.reset();
			stretch_active_ = true;
		}
		stretch_.set_speed(speed);
		const float *ptrs[64];
		for (size_t ch = 0; ch < channels_ && ch < 64; ch++)
			ptrs[ch] = scratch_[ch].data();
		stretch_.push(ptrs, in_n);
		stretch_.pull(out, frames);
		return frames;
	}

	/* Copy the ACCUMULATED mix (every source summed) at the LIVE frontier - the
	 * pre-delay program audio - via a monotonic cursor, so successive calls
	 * return contiguous blocks (no gap/overlap). Reads end `lookback` samples
	 * behind the newest write so every async source has landed. Non-destructive
	 * (the delayed read path is untouched). Returns sample count (0 if nothing
	 * new); out[ch] is resized. Used to feed the public audio taps. */
	size_t read_live(std::vector<std::vector<float>> &out, size_t max_n,
			 size_t lookback, uint64_t &ts_out)
	{
		if (channels_ == 0 || L_ == 0 || zeroed_upto_ == 0)
			return 0;
		const uint64_t end =
			zeroed_upto_ > lookback ? zeroed_upto_ - lookback : 0;
		const uint64_t floor =
			zeroed_upto_ > (uint64_t)L_ ? zeroed_upto_ - (uint64_t)L_ : 0;
		if (live_read_ < floor)
			live_read_ = floor; /* fell behind: skip the lost past */
		if (live_read_ >= end)
			return 0; /* nothing new yet */
		size_t cnt = (size_t)(end - live_read_);
		if (cnt > max_n)
			cnt = max_n;
		if (out.size() < channels_)
			out.resize(channels_);
		for (size_t ch = 0; ch < channels_; ch++)
			out[ch].resize(cnt);
		for (size_t i = 0; i < cnt; i++) {
			const size_t b = (size_t)((live_read_ + i) % L_);
			for (size_t ch = 0; ch < channels_; ch++)
				out[ch][i] = buf_[ch][b];
		}
		ts_out = base_ts_ +
			 (live_read_ * 1000000000ULL) / (uint64_t)rate_;
		live_read_ += cnt;
		return cnt;
	}

private:
	std::vector<std::vector<float>> buf_;
	std::vector<std::vector<float>> scratch_;
	TimeStretch stretch_;
	size_t channels_ = 0;
	size_t rate_ = 0;
	size_t delay_samples_ = 0;
	size_t L_ = 0;
	uint64_t base_ts_ = 0;
	uint64_t zeroed_upto_ = 0; /* ring positions cleared up to here (exclusive) */
	uint64_t live_read_ = 0;   /* monotonic LIVE-mix tap cursor */
	uint64_t next_emit_ = 0;
	int64_t last_snap_ = -3;
	double read_frac_ = 0.0;
	bool started_ = false;
	bool emitting_ = false;
	bool stretch_active_ = false;
};
