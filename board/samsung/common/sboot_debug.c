// SPDX-License-Identifier: GPL-2.0+
/*
 * Crash debugging for boards chainloaded from Samsung's S-Boot
 *
 * The "ramcon" output device keeps a copy of the console in a ring buffer in
 * RAM, which S-Boot leaves alone on a warm reset, so that upload mode can read
 * it back. Add it to stdout and stderr to use it. A hang, which a panic ends
 * in with CONFIG_PANIC_HANG, enters upload mode, like a Linux panic does.
 */

#include <bootstage.h>
#include <command.h>
#include <cpu_func.h>
#include <event.h>
#include <linker_lists.h>
#include <stdio_dev.h>
#include <asm/io.h>
#include <asm/system.h>
#include <linux/string.h>
#include <linux/types.h>

/* Debug area layout of S-Boot: magic, panic string, ..., copy of the magic */
#define SBOOT_DEBUG_AREA_SIZE	0x4000
#define SBOOT_MAGIC_UPLOAD	0x66262564
#define SBOOT_PANIC_STR_OFF	4
#define SBOOT_PANIC_STR_SIZE	1020

#define RAMCON_MAGIC		0x4e4f4352	/* "RCON" */

struct ramcon {
	u32 magic;
	u32 size;	/* of data */
	u32 head;	/* total bytes written, data[head % size] is next */
	u32 boots;	/* boots logged since the buffer was set up */
	char data[];
};

static struct ramcon *const ramcon =
	(struct ramcon *)CONFIG_SAMSUNG_SBOOT_RAMCON_ADDR;

static void ramcon_putc(struct stdio_dev *dev, const char c)
{
	ramcon->data[ramcon->head++ % ramcon->size] = c;
}

static void ramcon_puts(struct stdio_dev *dev, const char *s)
{
	while (*s)
		ramcon_putc(dev, *s++);
}

static int ramcon_register(void)
{
	struct stdio_dev dev = {
		.name = "ramcon",
		.flags = DEV_FLAGS_OUTPUT,
		.putc = ramcon_putc,
		.puts = ramcon_puts,
	};
	u32 size = CONFIG_SAMSUNG_SBOOT_RAMCON_SIZE - sizeof(*ramcon);

	/*
	 * Upload mode reads it after a reset, which drops what is only in the
	 * data cache.
	 */
	mmu_set_region_dcache_behaviour(CONFIG_SAMSUNG_SBOOT_RAMCON_ADDR,
					CONFIG_SAMSUNG_SBOOT_RAMCON_SIZE,
					DCACHE_OFF);

	/* Keep what the previous boots logged */
	if (ramcon->magic != RAMCON_MAGIC || ramcon->size != size) {
		ramcon->magic = RAMCON_MAGIC;
		ramcon->size = size;
		ramcon->head = 0;
		ramcon->boots = 0;
	}
	ramcon->boots++;
	/* Don't leave an old message for upload mode to show */
	memset((u8 *)CONFIG_SAMSUNG_SBOOT_DEBUG_AREA + SBOOT_PANIC_STR_OFF, 0,
	       SBOOT_PANIC_STR_SIZE);
	ramcon_puts(NULL, "\n=== U-Boot boot ===\n");

	return stdio_register(&dev);
}

/* Before the console is set up from the environment, which may name ramcon */
EVENT_SPY_SIMPLE(EVT_SETTINGS_R, ramcon_register);

/*
 * Make S-Boot enter upload mode on the next boot, with msg as the reason, and
 * reset. The magic has to reach RAM, a reset drops what is only in the cache.
 */
static void sboot_enter_upload(const char *msg)
{
	u8 *area = (u8 *)CONFIG_SAMSUNG_SBOOT_DEBUG_AREA;

	memset(area + SBOOT_PANIC_STR_OFF, 0, SBOOT_PANIC_STR_SIZE);
	strlcpy((char *)area + SBOOT_PANIC_STR_OFF, msg, SBOOT_PANIC_STR_SIZE);
	writel(SBOOT_MAGIC_UPLOAD, area);
	writel(SBOOT_MAGIC_UPLOAD, area + SBOOT_DEBUG_AREA_SIZE - 4);

	/* A hang before relocation comes before ramcon is set up */
	if (ramcon->magic == RAMCON_MAGIC && ramcon->size)
		ramcon_puts(NULL, "\n=== entering upload mode ===\n");
	flush_dcache_all();
	reset_cpu();
}

void show_boot_progress(int val)
{
	/* hang() reports this before stopping */
	if (val != -BOOTSTAGE_ID_NEED_RESET)
		return;

	sboot_enter_upload("U-Boot hang, see its RAM console");
}

#ifdef CONFIG_CMD_SBOOT_UPLOAD
static int do_upload(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	sboot_enter_upload("Upload mode requested from U-Boot");

	return CMD_RET_FAILURE;
}

U_BOOT_CMD(upload, 1, 0, do_upload,
	   "reset into S-Boot's upload mode",
	   "\n    - keeps the RAM, including U-Boot's RAM console, for upload");
#endif
