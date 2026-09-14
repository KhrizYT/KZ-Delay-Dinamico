/*
 * Broadcast Delay - centralized tuning constants.
 *
 * Single source of truth for the cross-cutting "magic numbers" of the engine,
 * so behaviour is tuned in one place and future modules (once dse-source.cpp
 * is split) share the same values. Local, self-explanatory heuristics that only
 * make sense next to their code (e.g. the RAM-budget fallbacks) stay inline.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace dse {

/* Extra frames kept past the exact delay so the reader never races the writer
 * at the head of the ring. */
inline constexpr size_t kSafeHeadMargin = 3;

/* VRAM delay-ring budget (GPU texture ring upper bound). */
inline constexpr uint64_t kVramBudgetBytes = 4ULL << 30; /* 4 GiB */

/* "Live edge" floor: the playhead never sits closer than this to now, which
 * leaves room to mix audio and absorb capture jitter. */
inline constexpr int64_t kMinLiveNs = 50'000'000LL; /* 50 ms */

/* Disk writer queue: drop the OLDEST queued frame once it exceeds this many
 * frames. Kept SHALLOW on purpose: if the encoder ever falls behind, we'd rather
 * LOSE a few frames than let the queue grow - a deep queue makes the disk write
 * position lag behind live, which delays the delayed video and DESYNCS it from
 * the (separately-delayed) audio. Better to drop than to drift. The ultrafast
 * encoder normally keeps up, so this rarely fills. ~4 frames = ~67 ms @ 60 fps. */
inline constexpr size_t kDiskWriteQueueMax = 4;

/* Recycled frame-buffer pool cap (so push() rarely allocates). */
inline constexpr size_t kDiskBufPoolMax = 8;

} // namespace dse
