/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DMC_BL_SELFCORRECT_H
#define DMC_BL_SELFCORRECT_H

/*
 * Make sure the DMC's MCUBoot (internal "mcuboot" partition) matches the new
 * MCUBoot image stored in SPI flash ("blupdate").
 *
 * Returns:
 *   0  mcuboot already matches blupdate, nothing written
 *   1  mcuboot was rewritten; the caller must reboot so the new MCUBoot runs
 *  <0  error (mcuboot may be left in an unknown state if it was being written)
 */
int dmc_bl_selfcorrect(void);

#endif /* DMC_BL_SELFCORRECT_H */
