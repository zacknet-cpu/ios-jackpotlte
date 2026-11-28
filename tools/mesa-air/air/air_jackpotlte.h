/* SPDX-License-Identifier: MIT
 * P13 (ios-jackpotlte): what air_run.c needs from Jesvi-iOS's redwood-drm
 * that this port's drm face (userspace/libjackpotlte) does not have. */
#ifndef AIR_JACKPOTLTE_H
#define AIR_JACKPOTLTE_H
#include <stdint.h>
/* GPU fault/reset/job counters; 0 = filled in, nonzero = not known here. */
int redwood_drm_counts(uint64_t counts[4]);
#endif
