/*
 * Broadcast Delay - system-RAM video frame ring buffer
 *
 * A fixed-capacity circular buffer of CPU frame buffers (RGBA, tightly packed).
 * Frames are captured by reading back the rendered target from the GPU into RAM
 * and replayed by uploading the delayed frame back to a GPU texture. RAM (tens
 * of GB) vastly exceeds VRAM (a few GB), so this supports long delays that a GPU
 * texture ring cannot. Memory scales linearly with resolution x FPS x delay.
 *
 * Each slot is allocated separately (no giant contiguous block) for robustness,
 * and pre-allocated up front for predictable memory use. If the full capacity
 * cannot be allocated, it keeps what it got and reports the achieved capacity so
 * the caller can clamp the delay instead of thrashing.
 *
 * THREADING: all access happens on the OBS graphics thread (video_render).
 */
#pragma once

#include <obs.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

class VideoRing {
public:
	VideoRing() = default;
	~VideoRing() { destroy(); }

	VideoRing(const VideoRing &) = delete;
	VideoRing &operator=(const VideoRing &) = delete;

	/*
	 * (Re)allocate for the given dimensions and slot count. Returns true if
	 * the layout changed (caller treats this as a flush). On partial OOM it
	 * keeps the slots it managed to allocate; check capacity() afterwards.
	 */
	bool configure(uint32_t cx, uint32_t cy, size_t capacity)
	{
		const size_t fb = (size_t)cx * cy * 4;
		if (cx == width_ && cy == height_ && capacity == frames_.size())
			return false;

		destroy();
		if (cx == 0 || cy == 0 || capacity == 0)
			return true;

		frame_bytes_ = fb;
		width_ = cx;
		height_ = cy;
		frames_.reserve(capacity);
		ts_.reserve(capacity);
		for (size_t i = 0; i < capacity; i++) {
			/* No zero-fill: playback only ever reads slots that have
			 * been written (count_ tracks writes), and zeroing tens
			 * of GB on the graphics thread would stall the renderer.
			 * Pages commit lazily as frames are captured. */
			uint8_t *buf = new (std::nothrow) uint8_t[fb];
			if (!buf)
				break; /* partial: stop, keep what we have */
			frames_.emplace_back(buf);
			ts_.push_back(0);
		}
		head_ = 0;
		count_ = 0;
		return true;
	}

	void destroy()
	{
		frames_.clear();
		frames_.shrink_to_fit();
		ts_.clear();
		ts_.shrink_to_fit();
		width_ = height_ = 0;
		frame_bytes_ = 0;
		head_ = count_ = 0;
	}

	bool ready() const { return !frames_.empty(); }
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	size_t capacity() const { return frames_.size(); }
	size_t count() const { return count_; }
	size_t frame_bytes() const { return frame_bytes_; }

	/* Claim the next write slot, stamp it, return its RAM buffer to fill. */
	uint8_t *acquire_write(uint64_t ts)
	{
		const size_t cap = frames_.size();
		uint8_t *p = frames_[head_].get();
		ts_[head_] = ts;
		head_ = (head_ + 1) % cap;
		if (count_ < cap)
			count_++;
		return p;
	}

	/* Newest frame with timestamp <= target_time (FIFO), or nullptr. */
	const uint8_t *sample(uint64_t target_time) const
	{
		if (count_ == 0)
			return nullptr;
		const size_t cap = frames_.size();
		const size_t oldest = (head_ + cap - count_) % cap;
		const uint8_t *chosen = nullptr;
		for (size_t i = 0; i < count_; i++) {
			const size_t idx = (oldest + i) % cap;
			if (ts_[idx] <= target_time)
				chosen = frames_[idx].get();
			else
				break;
		}
		return chosen;
	}

	/* The two frames bracketing target_time + interpolation fraction. */
	bool sample2(uint64_t target_time, const uint8_t **a, const uint8_t **b,
		     float *frac) const
	{
		*a = *b = nullptr;
		*frac = 0.0f;
		if (count_ == 0)
			return false;
		const size_t cap = frames_.size();
		const size_t oldest = (head_ + cap - count_) % cap;
		int idxA = -1;
		for (size_t i = 0; i < count_; i++) {
			if (ts_[(oldest + i) % cap] <= target_time)
				idxA = (int)i;
			else
				break;
		}
		if (idxA < 0)
			return false;
		const size_t ia = (oldest + idxA) % cap;
		*a = frames_[ia].get();
		if ((size_t)idxA + 1 < count_) {
			const size_t ib = (oldest + idxA + 1) % cap;
			*b = frames_[ib].get();
			const uint64_t span = ts_[ib] - ts_[ia];
			float f = span ? (float)((double)(target_time - ts_[ia]) /
						 (double)span)
				       : 0.0f;
			*frac = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
		}
		return true;
	}

private:
	std::vector<std::unique_ptr<uint8_t[]>> frames_;
	std::vector<uint64_t> ts_;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	size_t frame_bytes_ = 0;
	size_t head_ = 0;
	size_t count_ = 0;
};
