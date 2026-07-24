/*
 * Minimal Linux ARM zImage-compatible header used to wrap U-Boot so that
 * Samsung's S-Boot loader recognises the payload as a "zImage" and jumps
 * into it.  Layout mirrors arch/arm/boot/compressed/head.S:
 *
 *   0x00..0x1f  8 NOPs (legacy a.out 32-byte entry skip)
 *   0x20        b 1f            (branch over the magic table)
 *   0x24        magic sig       0x016f2818
 *   0x28        start           (0 == position independent)
 *   0x2c        end             (total image size, patched post-link)
 *   0x30        endian flag     0x04030201
 *   0x34        magic           0x45454545
 *   0x38        table pointer   (0 == none)
 *   0x3c        1:              padded to an 8-byte boundary, then falls
 *                               through into the appended U-Boot image
 *
 * S-Boot loads the image to 0x40008000 and enters at offset 0 with the MMU
 * off, so U-Boot's own start.S (cpu_init_cp15) handles cache/MMU state.  The
 * header MUST be a multiple of 8 bytes so U-Boot's _start - and hence its
 * internally-8-aligned embedded DTB - stays 8-byte aligned, as libfdt's
 * fdt_check_header() requires.
 */
	.arm
	.section .text
	.globl _start
_start:
	.rept 8
	mov	r0, r0
	.endr
	b	1f
	.word	0x016f2818	@ magic signature
	.word	0		@ start (position independent)
	.word	0xdeadbeef	@ end (total size, patched after concat)
	.word	0x04030201	@ endianness flag
	.word	0x45454545	@ magic
	.word	0		@ table pointer (none)
1:
	/* pad to an 8-byte boundary; falls through into U-Boot */
	.balign 8
