/* SPDX-License-Identifier: MIT
 * P13 (ios-jackpotlte): redwood_drm_counts for air_run on this port. The
 * JackpotlteMali user client keeps no fault counters that the drm face can
 * read yet, so this says "not known" and air_run prints no fault line. */
#include "air_jackpotlte.h"

int
redwood_drm_counts(uint64_t counts[4])
{
   (void)counts;
   return -1;
}
