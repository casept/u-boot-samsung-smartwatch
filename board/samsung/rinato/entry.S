/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Keep the ATAG pointer S-Boot passes in r2. This runs before U-Boot has
 * fixed up its relocations and set up a stack, at the address S-Boot loaded
 * it to rather than the one it is linked at, so only PC-relative addressing
 * works here.
 */

#include <linux/linkage.h>

ENTRY(save_boot_params)
	adr	r12, sboot_atags_offset
	ldr	r13, sboot_atags_offset
	add	r12, r12, r13
	str	r2, [r12]
	b	save_boot_params_ret
ENDPROC(save_boot_params)

sboot_atags_offset:
	.long	sboot_atags - .
