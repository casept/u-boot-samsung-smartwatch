/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2025
 * Davids Paskevics <davids.paskevics@gmail.com>
 *
 * Configuation settings for the SAMSUNG TRATS (EXYNOS3250) board.
 * This board is primarily configured via DT, only put settings here that need
 * to be here.
 */

#ifndef __CONFIG_ARTIK5_H
#define __CONFIG_ARTIK5_H

#include <configs/exynos4-common.h>
#include <linux/sizes.h>

/* DRAM config */
/* TODO: Do we need it here? Overlaps with DT */
#define CFG_SYS_SDRAM_BASE 0x40000000
#define PHYS_SDRAM_1 CFG_SYS_SDRAM_BASE
#define SDRAM_BANK_SIZE SZ_64M
#define CFG_SYS_INIT_RAM_ADDR (CFG_SYS_SDRAM_BASE)
#define CFG_SYS_INIT_RAM_SIZE (SDRAM_BANK_SIZE)

#endif /* __CONFIG_H */
