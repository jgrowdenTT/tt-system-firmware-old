/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DM app side of the MCUBoot self-correction.
 *
 * The DMC's internal MCUBoot (partition "mcuboot", 64 KiB at 0x08000000) is
 * replaced from the new MCUBoot image held in SPI flash (partition "blupdate",
 * below the rotation lock boundary). This replaces the separate
 * dmc-rom-update app that the 19.x bootloader used to run.
 *
 * Only a mismatch triggers a rewrite. The comparison is a direct byte compare
 * of the full partition, which is at least as strong as a hash and needs no
 * crypto stack on the DMC.
 *
 * The SPI flash is shared with the ASIC (ARC). The mux is switched to the DMC
 * for the compare/copy and the caller re-applies its own mux setting afterwards.
 * This must run before the ASIC is allowed to read SPI flash.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <tenstorrent/bh_chip.h>

#include "bl_selfcorrect.h"

LOG_MODULE_REGISTER(bl_selfcorrect, LOG_LEVEL_INF);

#define MCUBOOT_PART_NODE  DT_NODE_BY_FIXED_PARTITION_LABEL(mcuboot)
#define BLUPDATE_PART_NODE DT_NODE_BY_FIXED_PARTITION_LABEL(blupdate)

BUILD_ASSERT(DT_FIXED_PARTITION_EXISTS(MCUBOOT_PART_NODE),
	     "No mcuboot partition found in devicetree");
BUILD_ASSERT(DT_FIXED_PARTITION_EXISTS(BLUPDATE_PART_NODE),
	     "No blupdate partition found in devicetree");
BUILD_ASSERT(PARTITION_NODE_SIZE(BLUPDATE_PART_NODE) <= PARTITION_NODE_SIZE(MCUBOOT_PART_NODE),
	     "blupdate is larger than the mcuboot partition");

#define CHUNK_SIZE 1024

static uint8_t chunk_a[CHUNK_SIZE] __aligned(4);
static uint8_t chunk_b[CHUNK_SIZE] __aligned(4);

/* SPI mux: logical active = flash <-> ASIC, inactive = flash <-> DMC */
static void spi_mux_select(bool to_dmc)
{
	ARRAY_FOR_EACH_BH_CHIP(chip) {
		if (chip->config.spi_mux.port != NULL) {
			gpio_pin_configure_dt(&chip->config.spi_mux,
					      to_dmc ? GPIO_OUTPUT_INACTIVE : GPIO_OUTPUT_ACTIVE);
		}
		if (chip->config.spi_mux1.port != NULL) {
			gpio_pin_configure_dt(&chip->config.spi_mux1,
					      to_dmc ? GPIO_OUTPUT_INACTIVE : GPIO_OUTPUT_ACTIVE);
		}
	}
}

/*
 * Returns 1 if the two flash ranges are identical, 0 if they differ, <0 on
 * a flash read error.
 */
static int ranges_identical(const struct device *src, off_t src_off,
			    const struct device *tgt, off_t tgt_off, size_t len)
{
	for (size_t done = 0; done < len; done += CHUNK_SIZE) {
		size_t n = MIN(CHUNK_SIZE, len - done);
		int rc;

		rc = flash_read(src, src_off + done, chunk_a, n);
		if (rc != 0) {
			return rc;
		}
		rc = flash_read(tgt, tgt_off + done, chunk_b, n);
		if (rc != 0) {
			return rc;
		}
		if (memcmp(chunk_a, chunk_b, n) != 0) {
			return 0;
		}
	}
	return 1;
}

int dmc_bl_selfcorrect(void)
{
	const struct device *tgt = PARTITION_NODE_DEVICE(MCUBOOT_PART_NODE);
	const struct device *src = PARTITION_NODE_DEVICE(BLUPDATE_PART_NODE);
	const off_t tgt_off = DT_REG_ADDR(MCUBOOT_PART_NODE);
	const off_t src_off = DT_REG_ADDR(BLUPDATE_PART_NODE);
	/* Copy the whole blupdate partition, so the target ends up byte-identical */
	const size_t len = PARTITION_NODE_SIZE(BLUPDATE_PART_NODE);
	int rc;

	if (!device_is_ready(tgt) || !device_is_ready(src)) {
		LOG_ERR("Flash devices not ready");
		return -ENODEV;
	}

	spi_mux_select(true);

	rc = ranges_identical(src, src_off, tgt, tgt_off, len);
	if (rc == 1) {
		LOG_INF("MCUBoot matches blupdate, no update needed");
		spi_mux_select(false);
		return 0;
	}
	if (rc < 0) {
		LOG_ERR("Failed to compare MCUBoot with blupdate: %d", rc);
		spi_mux_select(false);
		return rc;
	}

	LOG_WRN("MCUBoot differs from blupdate, updating %zu bytes", len);

	rc = flash_erase(tgt, tgt_off, len);
	if (rc != 0) {
		LOG_ERR("MCUBoot erase failed: %d", rc);
		spi_mux_select(false);
		return rc;
	}

	rc = flash_copy(src, src_off, tgt, tgt_off, len, chunk_a, sizeof(chunk_a));
	if (rc != 0) {
		LOG_ERR("MCUBoot copy failed: %d", rc);
		spi_mux_select(false);
		return rc;
	}

	/* Verify the write before rebooting into it */
	rc = ranges_identical(src, src_off, tgt, tgt_off, len);
	spi_mux_select(false);
	if (rc != 1) {
		LOG_ERR("MCUBoot verify after update failed: %d", rc);
		return rc < 0 ? rc : -EIO;
	}

	LOG_INF("MCUBoot updated from blupdate");
	return 1;
}
