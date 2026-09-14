/*
 * Broadcast Delay - disk-backed video delay buffer
 *
 * Stores captured frames on disk as independently-compressed images (MJPEG via
 * FFmpeg) in a circular set of per-frame files, enabling long delays bounded by
 * disk space instead of RAM. A writer thread encodes + writes; a reader thread
 * decodes the frame at (now - delay) into a RAM staging buffer that the graphics
 * thread uploads. Both run off the OBS render/audio threads, so disk I/O never
 * blocks rendering.
 *
 * MJPEG keeps every frame self-contained: no GOP, trivial seeking, simple and
 * robust. Quality (and therefore size) is controlled by `quality` (lower = better
 * / heavier).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class DiskBuffer {
public:
	virtual ~DiskBuffer() = default;

	enum Codec { RAW = 0, PNG = 1, MJPEG = 2, HLS = 3, DASH = 4 };

	/* Create + start. Returns null on failure (logged). `quality` is the
	 * MJPEG q (lower = better); ignored for RAW/PNG (both lossless). */
	static std::unique_ptr<DiskBuffer>
	create(const std::string &folder, uint32_t cx, uint32_t cy, int fps,
	       size_t capacity_frames, int codec, int quality);

	/* Queue a captured RGBA frame (graphics thread; non-blocking copy). */
	virtual void push(const uint8_t *rgba, uint32_t linesize, uint64_t ts) = 0;

	/* Copy the current delayed frame (RGBA, tightly packed) for playback.
	 * Returns false if nothing is ready yet (underflow). Graphics thread.
	 * out_ts (optional) receives the frame's id/timestamp. */
	virtual bool get_current(std::vector<uint8_t> &out_rgba,
				 uint64_t *out_ts = nullptr) = 0;

	/* Id/timestamp of the frame currently available (changes when a new
	 * delayed frame is ready); lets the caller skip a redundant re-upload. */
	virtual uint64_t current_ts() const = 0;

	virtual void set_delay_ns(uint64_t delay_ns) = 0;

	virtual uint32_t width() const = 0;
	virtual uint32_t height() const = 0;
	virtual uint64_t frames_written() const = 0;
	virtual uint64_t bytes_on_disk() const = 0;
	/* Live health metrics (for the Consommation dock / monitoring). */
	virtual uint64_t queue_depth() const { return 0; }
	virtual uint64_t dropped_frames() const { return 0; }
};
