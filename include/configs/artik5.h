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

/*
 * Console on both the UART and the USB CDC ACM gadget, for platforms
 * where the UART pads are not easily accessible. With no USB host
 * attached the ACM console waits at startup until the gadget is
 * enumerated; a ctrl-c on the UART skips it for that boot.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"stdin=serial,usbacm\0" \
	"stdout=serial,usbacm\0" \
	"stderr=serial,usbacm\0"

#endif /* __CONFIG_H */
