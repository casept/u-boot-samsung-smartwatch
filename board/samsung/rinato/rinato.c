// SPDX-License-Identifier: GPL-2.0+
/*
 * Samsung Gear 2 (rinato)
 */

#include <button.h>
#include <cyclic.h>
#include <env.h>
#include <errno.h>
#include <event.h>
#include <linker_lists.h>
#include <log.h>
#include <stdio.h>
#include <string.h>
#include <sysreset.h>
#include <time.h>
#include <asm/io.h>
#include <linux/bitops.h>

/* A press shorter than this is taken for a bounce */
#define POWER_KEY_DEBOUNCE_MS	50
/* Still running this long after PS_HOLD went low means USB keeps it on */
#define POWER_OFF_TIMEOUT_MS	500

/*
 * PMU INFORM2: S-Boot's restart handler sets it to 0x12345678 for a normal
 * reboot. Otherwise, with a charger attached, S-Boot boots into charging.
 */
#define SBOOT_INFORM2		0x10020808

/* PMU PS_HOLD_CONTROL, its level bit is what the poweroff node clears */
#define PS_HOLD_CONTROL		0x1002330c
#define PS_HOLD_HIGH		BIT(8)

static struct cyclic_info power_key_cyclic;
static struct udevice *power_key;
/* The key may still be held from turning the watch on */
static bool power_key_released;
static ulong power_key_pressed_at;
static ulong power_off_requested_at;

/*
 * The boot script sets power_key to "fastboot" while a press should stop the
 * boot and start fastboot, and clears it when the key should power off again.
 * While it is set to anything else, the key does nothing.
 */
static bool power_key_requests_fastboot(void)
{
	const char *action = env_get("power_key");

	return action && !strcmp(action, "fastboot");
}

static void rinato_power_key_poll(struct cyclic_info *c)
{
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
		    power_key_requests_fastboot()) {
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

	if (get_timer(power_key_pressed_at) < POWER_KEY_DEBOUNCE_MS) {
		power_key_pressed_at = 0;
		return;
	}

	power_key_pressed_at = 0;

	if (env_get("power_key"))
		return;

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
