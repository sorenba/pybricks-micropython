// SPDX-License-Identifier: MIT
// Copyright (c) 2021 The Pybricks Authors

/**
 * @addtogroup WatchdogDriver Driver: Watchdog Timer
 * @{
 */

#ifndef _PBDRV_WATCHDOG_H_
#define _PBDRV_WATCHDOG_H_

#include <pbdrv/config.h>

#if PBDRV_CONFIG_WATCHDOG

/**
 * Updates the watchdog timer to prevent reset.
 */
void pbdrv_watchdog_update(void);

/**
 * Extends the watchdog timeout to the maximum supported duration before a
 * deliberate low-power interval. The next hardware reset restores the normal
 * watchdog configuration.
 */
void pbdrv_watchdog_prepare_for_stop(void);

#else // PBDRV_CONFIG_WATCHDOG

static inline void pbdrv_watchdog_update(void) {
}

static inline void pbdrv_watchdog_prepare_for_stop(void) {
}

#endif // PBDRV_CONFIG_WATCHDOG

#endif // _PBDRV_WATCHDOG_H_

/** @} */
