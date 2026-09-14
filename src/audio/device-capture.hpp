/*
 * Broadcast Delay - OS audio capture (miniaudio backend)
 *
 * Captures audio directly from a Windows audio device (loopback or input)
 * outside the OBS audio graph. Feeds into the AudioMixer for clean, crackle-free
 * delay. Runs its own high-priority capture thread.
 */
#pragma once

#include <obs.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <mutex>

/* Mutex-protected SPSC ring buffer for WASAPI -> OBS audio thread. (The audio
 * blocks are large and infrequent, so the lock is never contended in practice;
 * a lock-free version isn't worth the complexity here.) */
class DeviceRing {
public:
	void init(size_t channels, size_t capacity_frames)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		buf_.assign(channels, std::vector<float>(capacity_frames, 0.0f));
		mask_ = capacity_frames - 1;
		write_ = 0;
		read_ = 0;
	}

	size_t write(const float *const *data, size_t frames)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		const size_t cap = mask_ + 1;
		const size_t avail = cap - (write_ - read_);
		const size_t n = frames < avail ? frames : avail;
		for (size_t i = 0; i < n; i++) {
			const size_t idx = (write_ + i) & mask_;
			for (size_t ch = 0; ch < buf_.size(); ch++)
				buf_[ch][idx] = data[ch][i];
		}
		write_ += n;
		return n;
	}

	size_t read(std::vector<std::vector<float>> &out, size_t frames)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		const size_t avail = write_ - read_;
		const size_t n = frames < avail ? frames : avail;
		for (size_t ch = 0; ch < buf_.size(); ch++) {
			out[ch].resize(frames);
			for (size_t i = 0; i < n; i++)
				out[ch][i] = buf_[ch][(read_ + i) & mask_];
			for (size_t i = n; i < frames; i++)
				out[ch][i] = 0.0f;
		}
		read_ += n;
		return n;
	}

private:
	std::mutex mtx_;
	std::vector<std::vector<float>> buf_;
	size_t mask_ = 0;
	uint64_t write_ = 0;
	uint64_t read_ = 0;
};

/*
 * Live monitoring output: a single shared playback device (default output) that
 * mixes the audio of every device whose "monitor" toggle is on, so the user can
 * hear them in real time. Capture threads push their (gain-applied) stereo
 * blocks; the playback callback drains the mix. Thread-safe.
 */
namespace AudioMonitor {
/* Ensure the playback device is running (true) or stopped (false). Idempotent;
 * call from a normal thread (e.g. the video tick), not a capture callback. */
void set_active(bool on, size_t channels, size_t rate);
/* Sum one block (planar, gain-applied) into the monitor mix. Any thread. */
void push(const float *const *planar, size_t channels, size_t frames,
	  float gain);
} // namespace AudioMonitor

class DeviceCapture {
public:
	DeviceCapture();
	~DeviceCapture();

	DeviceCapture(const DeviceCapture &) = delete;
	DeviceCapture &operator=(const DeviceCapture &) = delete;

	/* List available audio devices. Call from any thread. */
	struct Device {
		std::string id;
		std::string name;
		bool is_input = false;   /* capture (mic) vs render (loopback) */
		bool is_default = false; /* default device for its type */
	};
	static std::vector<Device> enumerate_devices();

	/*
	 * Start capturing from a device.  Pass the mixer + a mutex that
	 * serialises mixer access.  `device_id` empty = default render device.
	 * `on_data` is called from the capture thread with planar float audio.
	 */
	bool start(const char *device_id, bool is_input, size_t channels,
		   size_t rate,
		   std::function<void(const float *data[], size_t frames,
				      uint64_t ts)>
			   on_data);
	void stop();

	bool active() const { return active_.load(std::memory_order_relaxed); }

private:
	std::atomic<bool> active_{false};
	void *client_ = nullptr;
	void *capture_ = nullptr;
	void *thread_ = nullptr;
};
