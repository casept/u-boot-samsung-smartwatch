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

#ifdef CONFIG_BOARD_EARLY_INIT_F
int exynos_early_init_f(void) { return 0; }
#endif
