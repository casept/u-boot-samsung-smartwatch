// SPDX-License-Identifier: GPL-2.0+
/*
 *  Copyright (C) 2025 Davids Paskevics
 */

#include <asm/arch/cpu.h>
#include <asm/arch/mmc.h>
#include <asm/arch/periph.h>
#include <asm/arch/pinmux.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <linux/bitops.h>

/*
 * Exynos3250 CMU register offsets (from Linux clk-exynos3250.c).
 *
 * The DW MMC and SPI drivers set their own dividers via the
 * mach-exynos clock helpers, but the source mux and clock gates
 * must be set up by the board — there is no gate framework in
 * u-boot's exynos platform code, and BL1/BL2 only bring up PLLs
 * and DRAM.
 */
#define CMU_BASE		0x10030000
#define CMU_SRC_FSYS		(CMU_BASE + 0xC240)
#define CMU_GATE_SCLK_FSYS	(CMU_BASE + 0xC840)
#define CMU_GATE_SCLK_PERIL	(CMU_BASE + 0xC850)
#define CMU_GATE_IP_FSYS	(CMU_BASE + 0xC940)
#define CMU_GATE_IP_PERIL	(CMU_BASE + 0xC950)

/* SRC_FSYS: MMC0/1/2 source mux in bits [11:0], input 6 = div_mpll_pre */
#define MMC_SEL_MASK		0xFFF
#define MMC_SEL_MPLL_PRE	0x666

/* GATE_SCLK_FSYS: sclk gates for MMC channels */
#define SCLK_MMC0_GATE		BIT(0)
#define SCLK_MMC1_GATE		BIT(1)
#define SCLK_MMC2_GATE		BIT(2)

/* GATE_IP_FSYS: bus interface gates for SDMMC controllers */
#define IP_SDMMC0_GATE		BIT(5)
#define IP_SDMMC1_GATE		BIT(6)
#define IP_SDMMC2_GATE		BIT(7)

/* GATE_SCLK_PERIL / GATE_IP_PERIL: SPI0 gates */
#define SCLK_SPI0_GATE		BIT(6)
#define IP_SPI0_GATE		BIT(16)

#ifdef CONFIG_BOARD_EARLY_INIT_F
int exynos_early_init_f(void)
{

	/* Select div_mpll_pre as source for MMC0/1/2 */
	clrsetbits_le32(CMU_SRC_FSYS, MMC_SEL_MASK, MMC_SEL_MPLL_PRE);

	/* Open all MMC and SPI0 clock gates */
	setbits_le32(CMU_GATE_SCLK_FSYS,
		     SCLK_MMC0_GATE | SCLK_MMC1_GATE | SCLK_MMC2_GATE);
	setbits_le32(CMU_GATE_IP_FSYS,
		     IP_SDMMC0_GATE | IP_SDMMC1_GATE | IP_SDMMC2_GATE);
	setbits_le32(CMU_GATE_SCLK_PERIL, SCLK_SPI0_GATE);
	setbits_le32(CMU_GATE_IP_PERIL, IP_SPI0_GATE);

	return 0;
}
#endif
