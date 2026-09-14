/*
 * Broadcast Delay - VRAM (GPU texture) video ring
 *
 * A fixed-capacity ring of OBS native GPU textures (gs_texture_t). Capture
 * copies the rendered target straight into a slot (gs_copy_texture); playback
 * samples the slot whose timestamp best matches the playhead and draws it. No
 * GPU<->CPU readback, no extra copies on the hot path -- the fastest backend,
 * bounded by VRAM. Allocation stops as soon as the GPU is out of memory (the
 * partially-filled ring is kept), so it can never crash on VRAM exhaustion.
 *
 * THREADING: graphics thread only (called from video_render / capture).
 */
#pragma once

#include <obs.h>
#include <cstdint>
#include <vector>

class VramRing {
public:
	VramRing() = default;
	~VramRing() { destroy(); }

	VramRing(const VramRing &) = delete;
	VramRing &operator=(const VramRing &) = delete;

	/* (Re)allocate; returns true if the layout changed (treat as a flush).
	 * Stops early (keeps what it got) if the GPU runs out of memory. */
	bool configure(uint32_t cx, uint32_t cy, size_t capacity)
	{
		if (cx == width_ && cy == height_ && capacity == slots_.size())
			return false;
		destroy();
		if (cx == 0 || cy == 0 || capacity == 0)
			return true;

		slots_.reserve(capacity);
		for (size_t i = 0; i < capacity; i++) {
			gs_texture_t *t = gs_texture_create(
				cx, cy, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
			if (!t)
				break; /* VRAM full: keep the partial ring */
			slots_.push_back({t, 0});
		}
		width_ = cx;
		height_ = cy;
		head_ = 0;
		count_ = 0;
		return true;
	}

	void destroy()
	{
		for (Slot &s : slots_)
			if (s.tex)
				gs_texture_destroy(s.tex);
		slots_.clear();
		width_ = height_ = 0;
		head_ = count_ = 0;
	}

	bool ready() const { return !slots_.empty(); }
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	size_t capacity() const { return slots_.size(); }

	/* Claim the next slot, stamp it, return its texture to copy into. */
	gs_texture_t *acquire_write(uint64_t ts)
	{
		Slot &s = slots_[head_];
		s.ts = ts;
		gs_texture_t *tex = s.tex;
		head_ = (head_ + 1) % slots_.size();
		if (count_ < slots_.size())
			count_++;
		return tex;
	}

	/* Newest texture with timestamp <= target_time, or nullptr (underflow). */
	gs_texture_t *sample(uint64_t target_time) const
	{
		if (count_ == 0)
			return nullptr;
		const size_t cap = slots_.size();
		const size_t oldest = (head_ + cap - count_) % cap;
		gs_texture_t *chosen = nullptr;
		for (size_t i = 0; i < count_; i++) {
			const Slot &s = slots_[(oldest + i) % cap];
			if (s.ts <= target_time)
				chosen = s.tex;
			else
				break;
		}
		return chosen;
	}

	/*
	 * The two textures bracketing target_time (a = newest <= target, b = the
	 * next one) plus the interpolation fraction in [0,1]. b may be null at the
	 * edge. Returns false on underflow. Used for smooth slow/fast motion.
	 */
	bool sample2(uint64_t target_time, gs_texture_t **a, gs_texture_t **b,
		     float *frac) const
	{
		*a = *b = nullptr;
		*frac = 0.0f;
		if (count_ == 0)
			return false;
		const size_t cap = slots_.size();
		const size_t oldest = (head_ + cap - count_) % cap;
		int idxA = -1;
		for (size_t i = 0; i < count_; i++) {
			if (slots_[(oldest + i) % cap].ts <= target_time)
				idxA = (int)i;
			else
				break;
		}
		if (idxA < 0)
			return false;
		const Slot &A = slots_[(oldest + idxA) % cap];
		*a = A.tex;
		if ((size_t)idxA + 1 < count_) {
			const Slot &B = slots_[(oldest + idxA + 1) % cap];
			*b = B.tex;
			const uint64_t span = B.ts - A.ts;
			float f = span ? (float)((double)(target_time - A.ts) /
						 (double)span)
				       : 0.0f;
			*frac = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
		}
		return true;
	}

private:
	struct Slot {
		gs_texture_t *tex = nullptr;
		uint64_t ts = 0;
	};
	std::vector<Slot> slots_;
	uint32_t width_ = 0, height_ = 0;
	size_t head_ = 0, count_ = 0;
};
