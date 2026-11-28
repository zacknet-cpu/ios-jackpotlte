/* SPDX-License-Identifier: MIT
 * P13 (ios-jackpotlte): the two things Jesvi-iOS's AIR programs take from
 * that port's own patches and that this port does not have.
 *
 * redwood_drm_counts (redwood-drm, used by air_run): the JackpotlteMali user
 * client keeps no fault counters the drm face can read yet, so this says "not
 * known" and air_run prints no fault line.
 *
 * redwood_bo_cache_stats (Jesvi's panfrost-retain.patch in pan_bo.c, read by
 * libredwoodair's rwa_gpu_cache_stats): our panfrost has no BO-retain cache,
 * so the counters stay zero. Same layout as the declaration in air_metal.c. */
#include "air_jackpotlte.h"

int
redwood_drm_counts(uint64_t counts[4])
{
   (void)counts;
   return -1;
}

struct redwood_bo_cache_stats {
   uint64_t hits, misses, evicted, evicted_bytes;
   uint64_t cached_bytes, cached_bos;
};
struct redwood_bo_cache_stats redwood_bo_cache_stats;
