// Copyright (c) 2026 asxeem
// Licensed under the MIT License - see LICENSE file in the repo root for details.
/*
 * @file main.c
 * @brief Passive HRP-UWB frame-capture firmware entry point - DWM3001CDK.
 *
 * Brings the DW3110 up, prints the one-time config banner, and arms continuous
 * receive.  From then on the capture engine (capture.c) logs one line per RX
 * event and re-arms itself; the operator drives it live over the `uwb` shell
 * (shell_cmds.c).
 */

#include "capture.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/** @brief Boot the radio, print the banner, and start capturing. */
int main(void)
{
	int err = capture_init();

	capture_print_banner();

	if (err) {
		LOG_ERR("capture_init failed: %d - check DW3110 wiring/power", err);
		return 0; /* Keep the shell alive so the operator can retry. */
	}

	err = capture_start();
	if (err) {
		LOG_ERR("capture_start failed: %d", err);
	}

	return 0;
}
