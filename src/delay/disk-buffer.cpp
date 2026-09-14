/*
 * Broadcast Delay - disk-backed video delay buffer (implementation)
 *
 * Two backends, chosen by `quality`:
 *   quality == 0  -> RAW: one fixed-slot ring file (raw RGBA). Pixel-perfect,
 *                   ~zero CPU, a single file, constant smooth delay. Heavy disk.
 *   quality  > 0  -> MJPEG: one independently-compressed file per frame (FFmpeg).
 *                   Smaller, more CPU. `quality` = MJPEG q (lower = better).
 *
 * A writer thread stores frames; a reader thread fetches the frame at
 * (now - delay) into a RAM staging buffer the graphics thread uploads. Disk I/O
 * never touches the render/audio threads.
 */
#include "delay/disk-buffer.hpp"
#include "delay/png-stb.hpp"
#include "core/dse-constants.hpp"

#include <obs.h>
#include <util/platform.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define dsb_seek(f, off) _fseeki64((f), (int64_t)(off), SEEK_SET)
#else
#define dsb_seek(f, off) fseeko((f), (off_t)(off), SEEK_SET)
#endif

namespace {

struct QueueItem {
	std::vector<uint8_t> rgba; /* packed, linesize = cx*4 (raw/png/mjpeg) */
	/* HLS/DASH: the frame is SHARED with the playback ring, so the capture
	 * thread copies it once (not once per consumer). */
	std::shared_ptr<std::vector<uint8_t>> sframe;
	uint64_t ts = 0;
};

struct IndexEntry {
	uint64_t id = 0;
	uint64_t ts = 0;
};

class DiskBufferImpl : public DiskBuffer {
public:
	DiskBufferImpl(std::string folder, uint32_t cx, uint32_t cy, int fps,
		       size_t capacity, int codec, int quality)
		: folder_(std::move(folder)),
		  cx_(cx),
		  cy_(cy),
		  fps_(fps > 0 ? fps : 60),
		  capacity_(capacity ? capacity : 1),
		  codec_(codec),
		  quality_(quality),
		  raw_(codec == DiskBuffer::RAW)
	{
		stream_mode_ = (codec == DiskBuffer::HLS || codec == DiskBuffer::DASH);
	}

	~DiskBufferImpl() override { shutdown(); }

	bool init()
	{
		row_ = (size_t)cx_ * 4;
		frame_bytes_ = row_ * cy_;
		clean_folder();
		if (raw_)
			return init_raw();
		if (codec_ == DiskBuffer::PNG)
			return init_png_stb();
		if (codec_ == DiskBuffer::HLS || codec_ == DiskBuffer::DASH)
			return init_stream();
		return init_image(AV_CODEC_ID_MJPEG, AV_PIX_FMT_YUVJ420P, true);
	}

	/* HLS / DASH: encode with libx264, segment via AVFormatContext. */
	bool init_stream()
	{
		const char *fmt = codec_ == DiskBuffer::HLS ? "hls" : "dash";
		const char *ext = codec_ == DiskBuffer::HLS ? "ts" : "m4s";
		std::string playlist = folder_ + "/playlist." +
				       (codec_ == DiskBuffer::HLS ? "m3u8" : "mpd");

		/* Allocate output format context. */
		const AVOutputFormat *of =
			av_guess_format(fmt, nullptr, nullptr);
		if (!of) {
			blog(LOG_ERROR, "[disk-buffer] unknown format '%s'", fmt);
			return false;
		}
		AVFormatContext *fc = nullptr;
		if (avformat_alloc_output_context2(&fc, of, nullptr,
						   playlist.c_str()) < 0) {
			blog(LOG_ERROR,
			     "[disk-buffer] avformat_alloc_output_context2 failed");
			return false;
		}

		/* H.264 video stream. */
		const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!codec) {
			blog(LOG_ERROR, "[disk-buffer] H.264 encoder not found");
			avformat_free_context(fc);
			return false;
		}
		AVStream *st = avformat_new_stream(fc, nullptr);
		if (!st) {
			avformat_free_context(fc);
			return false;
		}
		AVCodecContext *cc = avcodec_alloc_context3(codec);
		if (!cc) {
			avformat_free_context(fc);
			return false;
		}
		cc->width = (int)cx_;
		cc->height = (int)cy_;
		cc->time_base = {1, fps_};
		cc->framerate = {fps_, 1};
		cc->pix_fmt = AV_PIX_FMT_YUV420P;
		cc->bit_rate = (int64_t)quality_ * 1000; /* quality_ = kbps for HLS */
		cc->gop_size = fps_ * segment_secs_;
		cc->max_b_frames = 0;
		/* Use every core; the encoder must keep up with the capture rate at
		 * full resolution, otherwise the write queue overflows and frames are
		 * dropped. Slice threads add latency-free parallelism per frame. */
		cc->thread_count = 0;            /* 0 = auto-detect all cores */
		cc->thread_type = FF_THREAD_SLICE; /* slice only: parallel, no added
						    * frame-buffering latency (pairs with
						    * zerolatency) */
		if (fc->oformat->flags & AVFMT_GLOBALHEADER)
			cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		/* ultrafast + zerolatency: x264's default ("medium") cannot encode a
		 * high-res 60 fps feed in real time and was the main cause of dropped
		 * frames. zerolatency also disables look-ahead/B-frames buffering so a
		 * frame is emitted as soon as it is fed. */
		AVDictionary *enc_opts = nullptr;
		av_dict_set(&enc_opts, "preset", "ultrafast", 0);
		av_dict_set(&enc_opts, "tune", "zerolatency", 0);
		if (avcodec_open2(cc, codec, &enc_opts) < 0) {
			blog(LOG_ERROR,
			     "[disk-buffer] avcodec_open2(H.264) failed");
			av_dict_free(&enc_opts);
			avcodec_free_context(&cc);
			avformat_free_context(fc);
			return false;
		}
		av_dict_free(&enc_opts);
		avcodec_parameters_from_context(st->codecpar, cc);
		st->time_base = cc->time_base;

		/* Segmenting: each segment is `segment_secs_` seconds. */
		av_opt_set_int(fc->priv_data, "hls_time", segment_secs_,
			       0);
		av_opt_set_int(fc->priv_data, "hls_list_size", 0, 0);
		av_opt_set(fc->priv_data, "hls_segment_filename",
			   (folder_ + "/seg_%05d." + ext).c_str(), 0);
		if (codec_ == DiskBuffer::DASH) {
			av_opt_set_int(fc->priv_data, "seg_duration",
				       segment_secs_, 0);
			av_opt_set(fc->priv_data, "dash_segment_type", ext,
				   0);
		}

		if (avformat_write_header(fc, nullptr) < 0) {
			blog(LOG_ERROR,
			     "[disk-buffer] avformat_write_header failed");
			avcodec_free_context(&cc);
			avformat_free_context(fc);
			return false;
		}

		fmt_ctx_ = fc;
		codec_ctx_ = cc;
		stream_ = st;
		frame_count_ = 0;
		sws_ = sws_getContext((int)cx_, (int)cy_, AV_PIX_FMT_RGBA,
				      (int)cx_, (int)cy_, AV_PIX_FMT_YUV420P,
				      SWS_FAST_BILINEAR, nullptr, nullptr,
				      nullptr);
		if (!sws_)
			return false;

		/* Ring buffer for decoded frames (reader reads from RAM). */
		ring_buf_.resize(capacity_);
		ring_ts_.resize(capacity_, 0);
		ring_head_ = ring_count_ = 0;

		running_ = true;
		writer_ = std::thread(&DiskBufferImpl::stream_writer, this);
		reader_ = std::thread(&DiskBufferImpl::stream_reader, this);
		blog(LOG_INFO,
		     "[broadcast-delay] disk %s: %ux%u, %d fps, %d kbps, segments %ds, '%s'",
		     fmt, cx_, cy_, fps_, quality_, segment_secs_,
		     folder_.c_str());
		return true;
	}

	void stream_writer()
	{
		AVFrame *f = av_frame_alloc();
		f->format = AV_PIX_FMT_YUV420P;
		f->width = (int)cx_;
		f->height = (int)cy_;
		av_frame_get_buffer(f, 32);
		AVPacket *pkt = av_packet_alloc();

		while (true) {
			bool got = false;
			QueueItem item = pop_or_exit(got);
			if (!got)
				break;

			/* The playback ring is filled directly in push() (full rate,
			 * independent of this thread), so a slow/keyframe encode here
			 * never stalls playback. This thread only encodes the .ts. */

			/* Encode to H.264 and write to HLS/DASH segments. */
			if (!item.sframe)
				continue;
			const uint8_t *src[4] = {item.sframe->data(), nullptr,
						 nullptr, nullptr};
			int src_ls[4] = {(int)row_, 0, 0, 0};
			sws_scale(sws_, src, src_ls, 0, (int)cy_, f->data,
				  f->linesize);
			f->pts = frame_count_++;
			if (avcodec_send_frame(codec_ctx_, f) < 0)
				continue;
			while (avcodec_receive_packet(codec_ctx_, pkt) == 0) {
				av_packet_rescale_ts(pkt, codec_ctx_->time_base,
						     stream_->time_base);
				pkt->stream_index = stream_->index;
				av_interleaved_write_frame(fmt_ctx_, pkt);
				av_packet_unref(pkt);
			}
			frames_written_.fetch_add(1);
		}
		av_write_trailer(fmt_ctx_);
		av_frame_free(&f);
		av_packet_free(&pkt);
	}

	void stream_reader()
	{
		using namespace std::chrono;
		while (running_) {
			uint64_t target = os_gettime_ns() -
					  delay_ns_.load(std::memory_order_relaxed);
			{
				std::lock_guard<std::mutex> lk(index_mutex_);
				if (ring_count_ > 0) {
					/* Newest frame with ts <= target. Timestamps are
					 * monotonic along the ring, so binary-search the
					 * boundary (O(log n) instead of scanning the whole
					 * ring every poll - matters for long buffers). */
					int best = -1;
					int lo = 0, hi = (int)ring_count_ - 1;
					while (lo <= hi) {
						int mid = lo + (hi - lo) / 2;
						size_t idx =
							(ring_head_ + (size_t)mid) %
							capacity_;
						if (ring_ts_[idx] <= target) {
							best = mid;
							lo = mid + 1;
						} else {
							hi = mid - 1;
						}
					}
					if (best >= 0) {
						size_t idx = (ring_head_ + (size_t)best) % capacity_;
						const uint64_t ts = ring_ts_[idx];
						/* Only copy when the chosen frame actually
						 * changed. Frame timestamps are monotonic and
						 * unique, so an overwritten ring slot has a
						 * different ts -> we never serve stale data,
						 * but we skip a full-frame copy every poll
						 * while the playhead sits on the same frame. */
						if (ts != read_ts_.load(
							    std::memory_order_relaxed) ||
						    !have_read_) {
							std::lock_guard<std::mutex> rlk(read_mutex_);
							if (ring_buf_[idx])
								read_buf_ = *ring_buf_[idx];
							have_read_ = true;
							read_ts_.store(
								ts,
								std::memory_order_relaxed);
						}
					}
				}
			}
			/* Tighter poll = the delayed frame is selected closer to its
			 * arrival, for smoother pacing (the copy still only happens
			 * when the chosen frame actually changes). */
			std::this_thread::sleep_for(milliseconds(5));
		}
	}


	/* Remove stale buffer files from a previous (possibly crashed) run:
	 * raw/png/mjpeg frames (dsbuf_*) AND HLS/DASH segments + playlists
	 * (seg_*, *.m3u8, *.mpd), which otherwise accumulate in %TEMP%. */
	void clean_folder()
	{
		static const char *patterns[] = {"/dsbuf_*", "/seg_*",
						 "/*.m3u8", "/*.mpd"};
		for (const char *pat : patterns) {
			std::string pattern = folder_ + pat;
			os_glob_t *g = nullptr;
			if (os_glob(pattern.c_str(), 0, &g) == 0 && g) {
				for (size_t i = 0; i < g->gl_pathc; i++)
					if (!g->gl_pathv[i].directory)
						os_unlink(g->gl_pathv[i].path);
				os_globfree(g);
			}
		}
	}

	void push(const uint8_t *rgba, uint32_t linesize, uint64_t ts) override
	{
		if (stream_mode_) {
			/* HLS/DASH: ONE copy into a shared frame, then O(1) into
			 * both the playback ring (filled at the full capture rate,
			 * so a slow/keyframe encode never stalls playback) and the
			 * encoder queue - halving the capture-thread memcpy. */
			auto frame = std::make_shared<std::vector<uint8_t>>(
				frame_bytes_);
			if (linesize == row_) {
				memcpy(frame->data(), rgba, frame_bytes_);
			} else {
				for (uint32_t y = 0; y < cy_; y++)
					memcpy(frame->data() + (size_t)y * row_,
					       rgba + (size_t)y * linesize, row_);
			}
			{
				std::lock_guard<std::mutex> lk(index_mutex_);
				size_t slot = (ring_head_ + ring_count_) % capacity_;
				if (ring_count_ < capacity_)
					ring_count_++;
				else
					ring_head_ = (ring_head_ + 1) % capacity_;
				ring_buf_[slot] = frame; /* O(1) shared_ptr copy */
				ring_ts_[slot] = ts;
				bytes_on_disk_.store(
					(uint64_t)frame_bytes_ * ring_count_,
					std::memory_order_relaxed);
			}
			{
				std::lock_guard<std::mutex> lk(wq_mutex_);
				while (wq_.size() > dse::kDiskWriteQueueMax) {
					wq_.pop_front(); /* shared_ptr auto-frees */
					dropped_frames_.fetch_add(
						1, std::memory_order_relaxed);
				}
				QueueItem item;
				item.ts = ts;
				item.sframe = std::move(frame); /* O(1) */
				wq_.push_back(std::move(item));
			}
			wq_cv_.notify_one();
			return;
		}

		/* RAW / PNG / MJPEG: pooled vector, written to files by the writer. */
		QueueItem item;
		item.ts = ts;
		{
			std::lock_guard<std::mutex> lk(wq_mutex_);
			if (!bufpool_.empty()) {
				item.rgba = std::move(bufpool_.back());
				bufpool_.pop_back();
			}
		}
		item.rgba.resize(frame_bytes_); /* no-op if already sized */
		if (linesize == row_) {
			memcpy(item.rgba.data(), rgba, frame_bytes_);
		} else {
			for (uint32_t y = 0; y < cy_; y++)
				memcpy(item.rgba.data() + (size_t)y * row_,
				       rgba + (size_t)y * linesize, row_);
		}
		{
			std::lock_guard<std::mutex> lk(wq_mutex_);
			while (wq_.size() > dse::kDiskWriteQueueMax) {
				/* overflow: drop oldest, recycle its buffer */
				bufpool_.push_back(std::move(wq_.front().rgba));
				wq_.pop_front();
				dropped_frames_.fetch_add(
					1, std::memory_order_relaxed);
			}
			wq_.push_back(std::move(item));
		}
		wq_cv_.notify_one();
	}

	/* Return a consumed frame buffer to the pool (capped) for reuse. */
	void recycle_buf(std::vector<uint8_t> &&b)
	{
		std::lock_guard<std::mutex> lk(wq_mutex_);
		if (bufpool_.size() < dse::kDiskBufPoolMax)
			bufpool_.push_back(std::move(b));
	}

	bool get_current(std::vector<uint8_t> &out,
			 uint64_t *out_ts = nullptr) override
	{
		std::lock_guard<std::mutex> lk(read_mutex_);
		if (!have_read_)
			return false;
		out = read_buf_;
		if (out_ts)
			*out_ts = read_ts_.load(std::memory_order_relaxed);
		return true;
	}

	uint64_t current_ts() const override
	{
		return read_ts_.load(std::memory_order_relaxed);
	}

	void set_delay_ns(uint64_t d) override
	{
		delay_ns_.store(d, std::memory_order_relaxed);
	}

	uint32_t width() const override { return cx_; }
	uint32_t height() const override { return cy_; }
	uint64_t frames_written() const override
	{
		return frames_written_.load(std::memory_order_relaxed);
	}
	uint64_t bytes_on_disk() const override
	{
		return bytes_on_disk_.load(std::memory_order_relaxed);
	}
	uint64_t queue_depth() const override
	{
		std::lock_guard<std::mutex> lk(wq_mutex_);
		return wq_.size();
	}
	uint64_t dropped_frames() const override
	{
		return dropped_frames_.load(std::memory_order_relaxed);
	}

private:
	/* ----- shared ----- */
	QueueItem pop_or_exit(bool &got)
	{
		std::unique_lock<std::mutex> lk(wq_mutex_);
		wq_cv_.wait(lk, [&] { return !wq_.empty() || !running_; });
		if (!running_ && wq_.empty()) {
			got = false;
			return {};
		}
		got = true;
		QueueItem item = std::move(wq_.front());
		wq_.pop_front();
		return item;
	}

	/* Newest stored timestamp <= target. Returns slot index (raw) or -1. */
	int64_t find_slot(uint64_t target, uint64_t &slot_ts) const
	{
		if (raw_count_ == 0)
			return -1;
		const size_t oldest =
			(raw_head_ + capacity_ - raw_count_) % capacity_;
		int64_t chosen = -1;
		for (size_t i = 0; i < raw_count_; i++) {
			const size_t idx = (oldest + i) % capacity_;
			if (raw_ts_[idx] && raw_ts_[idx] <= target) {
				chosen = (int64_t)idx;
				slot_ts = raw_ts_[idx];
			} else if (raw_ts_[idx] > target) {
				break;
			}
		}
		return chosen;
	}

	uint64_t now_target() const
	{
		const uint64_t now = os_gettime_ns();
		const uint64_t delay = delay_ns_.load(std::memory_order_relaxed);
		return now > delay ? now - delay : 0;
	}

	/* ----- RAW backend (single fixed-slot ring file) ----- */
	bool init_raw()
	{
		raw_path_ = folder_ + "/dsbuf_raw.bin";
		raw_file_ = fopen(raw_path_.c_str(), "wb+");
		if (!raw_file_) {
			blog(LOG_ERROR, "[broadcast-delay] cannot open '%s'",
			     raw_path_.c_str());
			return false;
		}
		/* Unbuffered: 8 MB writes go straight to the OS page cache, so the
		 * separate reader handle sees them immediately. */
		setvbuf(raw_file_, nullptr, _IONBF, 0);
		/* Pre-size the file. */
		if (dsb_seek(raw_file_,
			     (int64_t)frame_bytes_ * (int64_t)capacity_ - 1) == 0) {
			fputc(0, raw_file_);
			fflush(raw_file_);
		}
		/* Reader gets its OWN handle so disk reads and writes don't
		 * serialise on one mutex (that halved throughput in delay mode
		 * and caused dropped frames). */
		raw_file_rd_ = fopen(raw_path_.c_str(), "rb");
		if (raw_file_rd_)
			setvbuf(raw_file_rd_, nullptr, _IONBF, 0);
		raw_ts_.assign(capacity_, 0);
		bytes_on_disk_.store((uint64_t)frame_bytes_ * capacity_);

		running_ = true;
		writer_ = std::thread(&DiskBufferImpl::raw_writer, this);
		reader_ = std::thread(&DiskBufferImpl::raw_reader, this);
		blog(LOG_INFO,
		     "[broadcast-delay] disk RAW ring: %ux%u, %d fps, %zu slots (~%.1f GB), '%s'",
		     cx_, cy_, fps_, capacity_,
		     (double)frame_bytes_ * capacity_ / (1024.0 * 1024.0 * 1024.0),
		     raw_path_.c_str());
		return true;
	}

	void raw_writer()
	{
		while (true) {
			bool got = false;
			QueueItem item = pop_or_exit(got);
			if (!got)
				break;

			size_t slot;
			{
				std::lock_guard<std::mutex> lk(index_mutex_);
				slot = raw_head_;
				raw_head_ = (raw_head_ + 1) % capacity_;
				if (raw_count_ < capacity_)
					raw_count_++;
			}
			{
				std::lock_guard<std::mutex> lk(raw_file_mutex_);
				dsb_seek(raw_file_,
					 (int64_t)slot * (int64_t)frame_bytes_);
				fwrite(item.rgba.data(), 1, frame_bytes_,
				       raw_file_);
			}
			{
				std::lock_guard<std::mutex> lk(index_mutex_);
				raw_ts_[slot] = item.ts; /* publish after write */
			}
			frames_written_.fetch_add(1);
			recycle_buf(std::move(item.rgba));
		}
	}

	void raw_reader()
	{
		std::vector<uint8_t> buf(frame_bytes_);
		uint64_t last_ts = UINT64_MAX;
		while (running_) {
			const uint64_t target = now_target();
			int64_t slot;
			uint64_t slot_ts = 0;
			{
				std::lock_guard<std::mutex> lk(index_mutex_);
				slot = find_slot(target, slot_ts);
			}
			if (slot >= 0 && slot_ts != last_ts && raw_file_rd_) {
				/* Own handle: no contention with the writer. */
				dsb_seek(raw_file_rd_,
					 (int64_t)slot * (int64_t)frame_bytes_);
				bool ok = fread(buf.data(), 1, frame_bytes_,
						raw_file_rd_) == frame_bytes_;
				if (ok) {
					std::lock_guard<std::mutex> lk(read_mutex_);
					read_buf_ = buf;
					have_read_ = true;
					read_ts_.store(slot_ts,
						       std::memory_order_relaxed);
				}
				last_ts = slot_ts;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
		}
	}

	/* PNG: stb_image (RGBA file <-> RAM, no FFmpeg color conversion). */
	bool init_png_stb()
	{
		running_ = true;
		writer_ = std::thread(&DiskBufferImpl::image_writer, this);
		reader_ = std::thread(&DiskBufferImpl::image_reader, this);
		blog(LOG_INFO,
		     "[broadcast-delay] disk PNG (stb): %ux%u, %d fps, capacity %zu frames, '%s'",
		     cx_, cy_, fps_, capacity_, folder_.c_str());
		return true;
	}

	/* ----- Image backend (MJPEG via FFmpeg) ----- */
	bool init_image(AVCodecID codec_id, AVPixelFormat pix, bool use_qscale)
	{
		img_pix_ = pix;
		use_qscale_ = use_qscale;
		const AVCodec *enc = avcodec_find_encoder(codec_id);
		const AVCodec *dec = avcodec_find_decoder(codec_id);
		if (!enc || !dec) {
			blog(LOG_ERROR, "[broadcast-delay] image codec missing");
			return false;
		}
		enc_ctx_ = avcodec_alloc_context3(enc);
		enc_ctx_->width = (int)cx_;
		enc_ctx_->height = (int)cy_;
		enc_ctx_->pix_fmt = pix;
		enc_ctx_->time_base = AVRational{1, fps_};
		if (pix == AV_PIX_FMT_YUVJ420P)
			enc_ctx_->color_range = AVCOL_RANGE_JPEG;
		if (use_qscale) {
			enc_ctx_->flags |= AV_CODEC_FLAG_QSCALE;
			enc_ctx_->global_quality = quality_ * FF_QP2LAMBDA;
		}
		dec_ctx_ = avcodec_alloc_context3(dec);
		/* Do not force decoder pix_fmt; convert with sws using
		 * dec_frame_->format after decode. */
		if (avcodec_open2(enc_ctx_, enc, nullptr) < 0 ||
		    avcodec_open2(dec_ctx_, dec, nullptr) < 0) {
			blog(LOG_ERROR, "[broadcast-delay] cannot open image codec");
			return false;
		}
		enc_frame_ = av_frame_alloc();
		enc_frame_->format = pix;
		enc_frame_->width = (int)cx_;
		enc_frame_->height = (int)cy_;
		if (av_frame_get_buffer(enc_frame_, 32) < 0)
			return false;
		enc_pkt_ = av_packet_alloc();
		dec_frame_ = av_frame_alloc();
		dec_pkt_ = av_packet_alloc();
		/* OBS capture is RGBA; encoder input is pix (RGB24 for PNG). */
		sws_to_yuv_ = sws_getContext((int)cx_, (int)cy_, AV_PIX_FMT_RGBA,
					     (int)cx_, (int)cy_, pix,
					     SWS_BILINEAR, nullptr, nullptr,
					     nullptr);
		sws_to_rgba_ = nullptr; /* cached decode: native -> RGBA */
		if (!sws_to_yuv_)
			return false;

		running_ = true;
		writer_ = std::thread(&DiskBufferImpl::image_writer, this);
		reader_ = std::thread(&DiskBufferImpl::image_reader, this);
		blog(LOG_INFO,
		     "[broadcast-delay] disk %s: %ux%u, %d fps, capacity %zu frames, '%s'",
		     codec_id == AV_CODEC_ID_PNG ? "PNG" : "MJPEG", cx_, cy_,
		     fps_, capacity_, folder_.c_str());
		return true;
	}

	void image_path(uint64_t id, char *out, size_t n) const
	{
		snprintf(out, n, "%s/dsbuf_%010llu.dat", folder_.c_str(),
			 (unsigned long long)id);
	}

	void image_writer()
	{
		while (true) {
			bool got = false;
			QueueItem item = pop_or_exit(got);
			if (!got)
				break;

			if (codec_ == DiskBuffer::PNG) {
				char path[1024], tmp[1024];
				image_path(next_id_, path, sizeof(path));
				snprintf(tmp, sizeof(tmp), "%s.tmp", path);
				const int level =
					quality_ >= 0 ? quality_ : 6;
				bool wrote_file = png_stb_write(
					tmp, (int)cx_, (int)cy_,
					item.rgba.data(), (int)row_, level);
				if (wrote_file) {
					rename(tmp, path);
					FILE *f = fopen(path, "rb");
					if (f) {
						fseek(f, 0, SEEK_END);
						long fsz = ftell(f);
						fclose(f);
						if (fsz > 0)
							bytes_on_disk_.fetch_add(
								(uint64_t)fsz);
					}
					std::lock_guard<std::mutex> lk(
						index_mutex_);
					mindex_.push_back({next_id_, item.ts});
					while (mindex_.size() > capacity_) {
						IndexEntry old = mindex_.front();
						mindex_.pop_front();
						char oldpath[1024];
						image_path(old.id, oldpath,
							   sizeof(oldpath));
						remove(oldpath);
					}
				}
				next_id_++;
				frames_written_.fetch_add(1);
				continue;
			}

			const uint8_t *src[4] = {item.rgba.data(), nullptr,
						 nullptr, nullptr};
			int src_ls[4] = {(int)row_, 0, 0, 0};
			av_frame_make_writable(enc_frame_);
			sws_scale(sws_to_yuv_, src, src_ls, 0, (int)cy_,
				  enc_frame_->data, enc_frame_->linesize);
			if (use_qscale_)
				enc_frame_->quality = quality_ * FF_QP2LAMBDA;
			enc_frame_->pts = (int64_t)next_id_;
			if (avcodec_send_frame(enc_ctx_, enc_frame_) < 0)
				continue;
			bool wrote_file = false;
			while (avcodec_receive_packet(enc_ctx_, enc_pkt_) == 0) {
				char path[1024], tmp[1024];
				image_path(next_id_, path, sizeof(path));
				snprintf(tmp, sizeof(tmp), "%s.tmp", path);
				FILE *f = fopen(tmp, "wb");
				if (f) {
					fwrite(enc_pkt_->data, 1,
					       enc_pkt_->size, f);
					fclose(f);
					rename(tmp, path);
					bytes_on_disk_.fetch_add(
						(uint64_t)enc_pkt_->size);
					wrote_file = true;
				}
				av_packet_unref(enc_pkt_);
			}
			if (wrote_file) {
				std::lock_guard<std::mutex> lk(index_mutex_);
				mindex_.push_back({next_id_, item.ts});
				while (mindex_.size() > capacity_) {
					IndexEntry old = mindex_.front();
					mindex_.pop_front();
					char path[1024];
					image_path(old.id, path, sizeof(path));
					remove(path);
				}
			}
			next_id_++;
			frames_written_.fetch_add(1);
		}
	}

	void image_reader()
	{
		std::vector<uint8_t> rgba(frame_bytes_);
		uint64_t last_id = UINT64_MAX;
		while (running_) {
			const uint64_t target = now_target();
			uint64_t want_id = 0;
			bool found = false;
			{
				std::lock_guard<std::mutex> lk(index_mutex_);
				for (auto it = mindex_.rbegin();
				     it != mindex_.rend(); ++it) {
					if (it->ts <= target) {
						want_id = it->id;
						found = true;
						break;
					}
				}
			}
			if (found && want_id != last_id) {
				if (image_decode(want_id, rgba)) {
					std::lock_guard<std::mutex> lk(read_mutex_);
					read_buf_ = rgba;
					have_read_ = true;
					read_ts_.store(want_id,
						       std::memory_order_relaxed);
					last_id = want_id;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
		}
	}

	bool image_decode(uint64_t id, std::vector<uint8_t> &out_rgba)
	{
		char path[1024];
		image_path(id, path, sizeof(path));
		if (codec_ == DiskBuffer::PNG)
			return png_stb_read(path, (int)cx_, (int)cy_,
					    out_rgba.data(), frame_bytes_);
		FILE *f = fopen(path, "rb");
		if (!f)
			return false;
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz <= 0) {
			fclose(f);
			return false;
		}
		file_buf_.resize((size_t)sz + AV_INPUT_BUFFER_PADDING_SIZE);
		size_t rd = fread(file_buf_.data(), 1, (size_t)sz, f);
		fclose(f);
		if (rd != (size_t)sz)
			return false;
		memset(file_buf_.data() + sz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		/* Use a proper FFmpeg-owned packet - never point dec_pkt_->data
		 * at our own buffer (avcodec may free it -> heap corruption). */
		av_packet_unref(dec_pkt_);
		if (av_new_packet(dec_pkt_, (int)sz) < 0)
			return false;
		memcpy(dec_pkt_->data, file_buf_.data(), (size_t)sz);
		/* Drain any leftover frame before the next still image. */
		while (avcodec_receive_frame(dec_ctx_, dec_frame_) == 0)
			av_frame_unref(dec_frame_);
		if (avcodec_send_packet(dec_ctx_, dec_pkt_) < 0)
			return false;
		memset(out_rgba.data(), 0, frame_bytes_);
		bool ok = false;
		while (avcodec_receive_frame(dec_ctx_, dec_frame_) == 0) {
			const int dw = dec_frame_->width;
			const int dh = dec_frame_->height;
			if (dw <= 0 || dh <= 0)
				continue;
			const AVPixelFormat sf =
				(AVPixelFormat)dec_frame_->format;
			sws_to_rgba_ = sws_getCachedContext(
				sws_to_rgba_, dw, dh, sf, (int)cx_, (int)cy_,
				AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
				nullptr);
			if (!sws_to_rgba_)
				continue;
			uint8_t *dst[4] = {out_rgba.data(), nullptr, nullptr,
					   nullptr};
			int dst_ls[4] = {(int)row_, 0, 0, 0};
			sws_scale(sws_to_rgba_, dec_frame_->data,
				  dec_frame_->linesize, 0, dh, dst, dst_ls);
			ok = true;
		}
		av_frame_unref(dec_frame_);
		return ok;
	}

	void shutdown()
	{
		running_ = false;
		wq_cv_.notify_all();
		if (writer_.joinable())
			writer_.join();
		if (reader_.joinable())
			reader_.join();

		if (raw_) {
			if (raw_file_rd_)
				fclose(raw_file_rd_);
			raw_file_rd_ = nullptr;
			if (raw_file_)
				fclose(raw_file_);
			raw_file_ = nullptr;
			remove(raw_path_.c_str());
		} else if (stream_mode_) {
			if (sws_)
				sws_freeContext(sws_);
			if (codec_ctx_) {
				avcodec_free_context(&codec_ctx_);
			}
			if (fmt_ctx_) {
				avformat_free_context(fmt_ctx_);
			}
		} else {
			for (const IndexEntry &e : mindex_) {
				char path[1024];
				image_path(e.id, path, sizeof(path));
				remove(path);
			}
			mindex_.clear();
			if (codec_ != DiskBuffer::PNG) {
				if (sws_to_yuv_)
					sws_freeContext(sws_to_yuv_);
				if (sws_to_rgba_)
					sws_freeContext(sws_to_rgba_);
				if (enc_frame_)
					av_frame_free(&enc_frame_);
				if (dec_frame_)
					av_frame_free(&dec_frame_);
				if (enc_pkt_)
					av_packet_free(&enc_pkt_);
				if (dec_pkt_)
					av_packet_free(&dec_pkt_);
				if (enc_ctx_)
					avcodec_free_context(&enc_ctx_);
				if (dec_ctx_)
					avcodec_free_context(&dec_ctx_);
			}
		}
	}

	std::string folder_;
	uint32_t cx_, cy_;
	int fps_;
	size_t capacity_;
	int codec_;
	int quality_;
	bool raw_;
	AVPixelFormat img_pix_ = AV_PIX_FMT_YUVJ420P;
	bool use_qscale_ = true;
	size_t row_ = 0;
	size_t frame_bytes_ = 0;

	std::atomic<bool> running_{false};
	std::atomic<uint64_t> delay_ns_{0};
	std::atomic<uint64_t> frames_written_{0};
	std::atomic<uint64_t> bytes_on_disk_{0};
	std::atomic<uint64_t> dropped_frames_{0};

	std::thread writer_;
	std::thread reader_;

	mutable std::mutex wq_mutex_;
	std::condition_variable wq_cv_;
	std::deque<QueueItem> wq_;
	/* Recycled frame buffers (guarded by wq_mutex_) so push() doesn't
	 * malloc/free a full frame on the render thread every tick - that churn
	 * caused irregular frame-time spikes. */
	std::deque<std::vector<uint8_t>> bufpool_;

	std::mutex index_mutex_;
	std::mutex read_mutex_;
	std::vector<uint8_t> read_buf_;
	bool have_read_ = false;
	/* Timestamp of the frame currently in read_buf_, so the render thread can
	 * skip re-copying + re-uploading an unchanged delayed frame (e.g. when the
	 * render fps exceeds the source fps, or playback is paused). */
	std::atomic<uint64_t> read_ts_{0};

	/* RAW */
	std::string raw_path_;
	FILE *raw_file_ = nullptr;     /* writer handle */
	FILE *raw_file_rd_ = nullptr;  /* separate reader handle (no write lock) */
	std::mutex raw_file_mutex_;
	std::vector<uint64_t> raw_ts_;
	size_t raw_head_ = 0;
	size_t raw_count_ = 0;

	/* MJPEG */
	std::deque<IndexEntry> mindex_;
	uint64_t next_id_ = 0;
	AVCodecContext *enc_ctx_ = nullptr;
	AVCodecContext *dec_ctx_ = nullptr;
	AVFrame *enc_frame_ = nullptr;
	AVFrame *dec_frame_ = nullptr;
	AVPacket *enc_pkt_ = nullptr;
	AVPacket *dec_pkt_ = nullptr;
	SwsContext *sws_to_yuv_ = nullptr;
	SwsContext *sws_to_rgba_ = nullptr;
	std::vector<uint8_t> file_buf_;

	/* HLS / DASH */
	int segment_secs_ = 4;
	AVFormatContext *fmt_ctx_ = nullptr;
	AVCodecContext *codec_ctx_ = nullptr;
	AVStream *stream_ = nullptr;
	SwsContext *sws_ = nullptr;
	int64_t frame_count_ = 0;
	bool stream_mode_ = false;

	/* HLS / DASH decoding ring (stream_reader reads from RAM). */
	std::vector<std::shared_ptr<std::vector<uint8_t>>> ring_buf_;
	std::vector<uint64_t> ring_ts_;
	size_t ring_head_ = 0;
	size_t ring_count_ = 0;
};

} // namespace

std::unique_ptr<DiskBuffer> DiskBuffer::create(const std::string &folder,
					       uint32_t cx, uint32_t cy, int fps,
					       size_t capacity_frames, int codec,
					       int quality)
{
	auto impl = std::make_unique<DiskBufferImpl>(
		folder, cx, cy, fps, capacity_frames, codec, quality);
	if (!impl->init())
		return nullptr;
	return impl;
}
