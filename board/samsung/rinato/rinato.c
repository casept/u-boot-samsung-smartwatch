// SPDX-License-Identifier: GPL-2.0+
/*
 * Samsung Gear 2 (rinato)
 */

#include <button.h>
#include <cyclic.h>
#include <dm.h>
#include <env.h>
#include <errno.h>
#include <event.h>
#include <g_dnl.h>
#include <linker_lists.h>
#include <log.h>
#include <mapmem.h>
#include <stdio.h>
#include <string.h>
#include <sysinfo.h>
#include <sysreset.h>
#include <time.h>
#include <asm/global_data.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/setup.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>

/* A press shorter than this is taken for a bounce */
#define POWER_KEY_DEBOUNCE_MS	50
/* In recovery, a press at least this long powers off, a shorter one switches */
#define POWER_KEY_LONG_MS	2000
/* Still running this long after PS_HOLD went low means USB keeps it on */
#define POWER_OFF_TIMEOUT_MS	500

/*
 * PMU INFORM2: S-Boot's restart handler sets it to 0x12345678 for a normal
 * reboot. Otherwise, with a charger attached, S-Boot boots into charging.
 */
#define SBOOT_INFORM2		0x10020808

/* PMU INFORM3: S-Boot stores a reboot mode requested here */
#define SBOOT_INFORM3		0x1002080c
#define SBOOT_REBOOT_NORMAL	0x12345670

/* PMU PS_HOLD_CONTROL, its level bit is what the poweroff node clears */
#define PS_HOLD_CONTROL		0x1002330c
#define PS_HOLD_HIGH		BIT(8)

DECLARE_GLOBAL_DATA_PTR;

/*
 * UART1 only leaves the SoC through the MUIC, which U-Boot switches to USB,
 * so nothing drives RXD1. Without a pull it reads low after a cold boot,
 * the UART receives a break as a NUL character and that stops autoboot.
 */
int exynos_early_init_f(void)
{
	gpio_set_pull(EXYNOS4_GPIO_A04, S5P_GPIO_PULL_UP);

	return 0;
}

static struct cyclic_info power_key_cyclic;
static struct udevice *power_key;
/* The key may still be held from turning the watch on */
static bool power_key_released;
static ulong power_key_pressed_at;
static ulong power_off_requested_at;

/*
 * The boot script sets power_key to "fastboot" while a press should stop the
 * boot and start fastboot, and clears it when the key should power off again.
 * "recovery" makes a short press switch between fastboot and mass storage and
 * a long one power off. While it is set to anything else, the key does nothing.
 */
static bool power_key_is(const char *action)
{
	const char *value = env_get("power_key");

	return value && !strcmp(value, action);
}

static void rinato_power_key_poll(struct cyclic_info *c)
{
	ulong held;

	/*
	 * PS_HOLD is low now, but the PMIC stays on while USB powers the
	 * watch. Do what S-Boot does then: restart without marking it a
	 * normal reboot, which makes S-Boot take its charging boot path.
	 */
	if (power_off_requested_at &&
	    get_timer(power_off_requested_at) >= POWER_OFF_TIMEOUT_MS) {
		puts("Powered by USB, restarting into charging mode\n");
		/* Don't turn off half way if the cable goes now */
		setbits_le32(PS_HOLD_CONTROL, PS_HOLD_HIGH);
		writel(0, SBOOT_INFORM2);
		sysreset_walk_halt(SYSRESET_COLD);
	}

	if (button_get_state(power_key) == BUTTON_ON) {
		if (!power_key_released)
			return;

		if (!power_key_pressed_at) {
			power_key_pressed_at = get_timer(0) ?: 1;
			return;
		}

		/*
		 * While booting, a press asks the boot script for fastboot.
		 * Tell it right away and ignore the key until it is released,
		 * so that letting go doesn't power off the fastboot started.
		 */
		if (get_timer(power_key_pressed_at) >= POWER_KEY_DEBOUNCE_MS &&
		    power_key_is("fastboot")) {
			puts("\nPower key pressed, starting fastboot\n");
			env_set("power_key", "pressed");
			power_key_pressed_at = 0;
			power_key_released = false;
		}
		return;
	}

	power_key_released = true;

	if (!power_key_pressed_at)
		return;

	held = get_timer(power_key_pressed_at);
	power_key_pressed_at = 0;

	if (held < POWER_KEY_DEBOUNCE_MS)
		return;

	if (power_key_is("recovery")) {
		if (held < POWER_KEY_LONG_MS) {
			/* Ends fastboot or mass storage, the script runs the other */
			puts("\nPower key pressed, switching\n");
			g_dnl_trigger_detach();
			return;
		}
	} else if (env_get("power_key")) {
		return;
	}

	/* The PMIC keeps the power on while the key is held, so act on release */
	puts("\nPower key pressed, powering off\n");
	if (sysreset_walk(SYSRESET_POWER_OFF) == -EINPROGRESS)
		power_off_requested_at = get_timer(0) ?: 1;
}

/*
 * The watch has no other way to be turned off while U-Boot runs, for example
 * when it sits in fastboot. Watch the key from everywhere U-Boot waits.
 */
static int rinato_power_key_init(void)
{
	if (button_get_by_label("power", &power_key)) {
		log_warning("No power key, pressing it won't power off\n");
		return 0;
	}

	cyclic_register(&power_key_cyclic, rinato_power_key_poll, 20 * 1000,
			"power-key");

	return 0;
}

EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, rinato_power_key_init);

/*
 * S-Boot starts U-Boot the way it starts a kernel, with ATAGs in r2. Its
 * command line says why it booted (charging, recovery, ...) and how the
 * watch was turned on, so give that to the boot script. save_boot_params in
 * entry.S keeps r2.
 */
ulong sboot_atags __section(".data");

static char *sboot_cmdline;

/*
 * The ATAGs are in S-Boot's debug area, which the RAM console clears when it
 * is set up, so copy the command line out first
 */
static void rinato_sboot_cmdline_save(void)
{
	const struct tag *t, *tags = (const struct tag *)sboot_atags;

	/* Started some other way, r2 may point anywhere */
	if (sboot_atags < gd->ram_base ||
	    sboot_atags >= gd->ram_base + gd->ram_size ||
	    tags->hdr.tag != ATAG_CORE) {
		log_warning("No ATAGs from S-Boot at %p\n", tags);
		return;
	}

	for_each_tag(t, tags) {
		if (t->hdr.tag == ATAG_CMDLINE) {
			sboot_cmdline = strdup(t->u.cmdline.cmdline);
			break;
		}
	}
}

void exynos_init(void)
{
	rinato_sboot_cmdline_save();

	/*
	 * Ask for a normal boot, so that powering off or resetting from recovery
	 * doesn't end up in recovery again.
	 */
	writel(SBOOT_REBOOT_NORMAL, SBOOT_INFORM3);
}

static int rinato_sboot_cmdline_env(void)
{
	if (sboot_cmdline)
		env_set("sboot_cmdline", sboot_cmdline);

	return 0;
}

EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, rinato_sboot_cmdline_env);

extern u8 __splash_booting_begin[];
extern u8 __splash_fastboot_begin[];
extern u8 __splash_recovery_fastboot_begin[];
extern u8 __splash_recovery_ums_begin[];

/* For the boot script to show once it knows what it boots */
static int rinato_splash_env(void)
{
	env_set_hex("booting_splash", map_to_sysmem(__splash_booting_begin));
	env_set_hex("fastboot_splash", map_to_sysmem(__splash_fastboot_begin));
	env_set_hex("recovery_fastboot_splash",
		    map_to_sysmem(__splash_recovery_fastboot_begin));
	env_set_hex("recovery_ums_splash",
		    map_to_sysmem(__splash_recovery_ums_begin));

	return 0;
}

EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, rinato_splash_env);

/*
 * The board revision is strapped on four GPIOs, which S-Boot reads after
 * making them inputs without pulls, and hands to the kernel as the ATAG
 * revision. It leaves them configured like that, so read them as they are.
 * Revisions 8 and up are the Gear 2 Neo (SM-R381), the others the Gear 2
 * (SM-R380).
 */
#define RINATO_HWREV_STRAPS	4
#define RINATO_HWREV_NEO	8

struct rinato_sysinfo_priv {
	struct gpio_desc straps[RINATO_HWREV_STRAPS];
	int hwrev;
};

/* The board names S-Boot uses for each revision */
static const char *const rinato_revisions[] = {
	[0] = "B2_BT Rev00A",
	[1] = "SM-R380 Rev0.0",
	[2] = "SM-R380 Rev0.0B",
	[3] = "SM-R380 Rev0.1",
	[4] = "SM-R380 Rev0.3",
	[5] = "SM-R380 Rev0.4",
	[6] = "SM-R380 Rev0.5",
	[8] = "SM-R381 REV0.0",
	[9] = "SM-R381 REV0.1",
	[10] = "SM-R381 REV0.2",
	[11] = "SM-R381 REV0.3",
};

static int rinato_sysinfo_detect(struct udevice *dev)
{
	struct rinato_sysinfo_priv *priv = dev_get_priv(dev);
	int ret;

	/*
	 * S-Boot also turns revision 1 into 5 when gpm4-3 reads high while
	 * the camera I/O supply is on. Only early prototypes report 1, so
	 * that isn't done here.
	 */
	ret = dm_gpio_get_values_as_int(priv->straps, RINATO_HWREV_STRAPS);
	if (ret < 0)
		return ret;

	priv->hwrev = ret;

	return 0;
}

static int rinato_sysinfo_get_int(struct udevice *dev, int id, int *val)
{
	struct rinato_sysinfo_priv *priv = dev_get_priv(dev);

	if (id != SYSID_BOARD_MODEL)
		return -ENOENT;

	*val = priv->hwrev;

	return 0;
}

static int rinato_sysinfo_get_str(struct udevice *dev, int id, size_t size,
				  char *val)
{
	struct rinato_sysinfo_priv *priv = dev_get_priv(dev);
	const char *name = NULL;

	switch (id) {
	case SYSID_BOARD_MANUFACTURER:
		strlcpy(val, "Samsung", size);
		return 0;
	case SYSID_BOARD_MODEL:
		if (priv->hwrev < ARRAY_SIZE(rinato_revisions))
			name = rinato_revisions[priv->hwrev];
		snprintf(val, size, "Samsung Gear 2%s (%s)",
			 priv->hwrev >= RINATO_HWREV_NEO ? " Neo" : "",
			 name ?: "unknown revision");
		return 0;
	default:
		return -ENOENT;
	}
}

static const struct sysinfo_ops rinato_sysinfo_ops = {
	.detect = rinato_sysinfo_detect,
	.get_int = rinato_sysinfo_get_int,
	.get_str = rinato_sysinfo_get_str,
};

static int rinato_sysinfo_probe(struct udevice *dev)
{
	struct rinato_sysinfo_priv *priv = dev_get_priv(dev);
	int ret;

	ret = gpio_request_list_by_name(dev, "hwrev-gpios", priv->straps,
					RINATO_HWREV_STRAPS, GPIOD_IS_IN);
	if (ret != RINATO_HWREV_STRAPS) {
		dev_err(dev, "need %d revision straps, got %d\n",
			RINATO_HWREV_STRAPS, ret);
		return ret < 0 ? ret : -EINVAL;
	}

	return 0;
}

static const struct udevice_id rinato_sysinfo_ids[] = {
	{ .compatible = "samsung,rinato-sysinfo" },
	{ }
};

U_BOOT_DRIVER(rinato_sysinfo) = {
	.name		= "rinato_sysinfo",
	.id		= UCLASS_SYSINFO,
	.of_match	= rinato_sysinfo_ids,
	.ops		= &rinato_sysinfo_ops,
	.priv_auto	= sizeof(struct rinato_sysinfo_priv),
	.probe		= rinato_sysinfo_probe,
};

/* For scripts in the environment, e.g. to pick the device tree */
static int rinato_hwrev_env(void)
{
	struct udevice *dev;
	int hwrev, ret;

	ret = sysinfo_get(&dev);
	if (!ret)
		ret = sysinfo_detect(dev);
	if (!ret)
		ret = sysinfo_get_int(dev, SYSID_BOARD_MODEL, &hwrev);
	if (ret) {
		log_warning("Can't read the board revision: %d\n", ret);
		return 0;
	}

	env_set_ulong("board_rev", hwrev);
	env_set("board_model", hwrev >= RINATO_HWREV_NEO ? "SM-R381" : "SM-R380");

	return 0;
}

EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, rinato_hwrev_env);
