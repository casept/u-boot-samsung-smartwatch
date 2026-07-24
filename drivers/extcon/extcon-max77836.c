// SPDX-License-Identifier: GPL-2.0+
/*
 * Tiny extcon driver for the Maxim MAX77836 MUIC.
 * Needed on Rinato to expose USB.
 */

#include <dm.h>
#include <i2c.h>
#include <extcon.h>
#include <dm/device_compat.h>

#define MUIC_REG_CONTROL1	0x0c
#define MUIC_REG_CONTROL2	0x0d

/* CONTROL1: COMN1SW is bits [2:0], COMP2SW is bits [5:3]. */
#define CONTROL1_COMN1SW_SHIFT	0
#define CONTROL1_COMP2SW_SHIFT	3
#define CONTROL1_SW_MASK	((0x7 << CONTROL1_COMN1SW_SHIFT) | \
				 (0x7 << CONTROL1_COMP2SW_SHIFT))
#define CONTROL1_SW_USB		((0x1 << CONTROL1_COMN1SW_SHIFT) | \
				 (0x1 << CONTROL1_COMP2SW_SHIFT))

/* CONTROL2: the switches only conduct with LowPwr cleared and CPEn set. */
#define CONTROL2_LOWPWR		BIT(0)
#define CONTROL2_CPEN		BIT(2)

static int max77836_muic_probe(struct udevice *dev)
{
	int ret;

	/* Route the micro-USB D+/D- lines to the AP USB controller. */
	ret = dm_i2c_reg_clrset(dev, MUIC_REG_CONTROL1,
				CONTROL1_SW_MASK, CONTROL1_SW_USB);
	if (ret) {
		dev_err(dev, "failed to route USB to the AP: %d\n", ret);
		return ret;
	}

	/*
	 * As Linux does for an attached cable. Without this the path only
	 * works once the MUIC itself detects an attach, that is after
	 * replugging the cable.
	 */
	ret = dm_i2c_reg_clrset(dev, MUIC_REG_CONTROL2,
				CONTROL2_LOWPWR | CONTROL2_CPEN, CONTROL2_CPEN);
	if (ret)
		dev_err(dev, "failed to power the USB switches: %d\n", ret);

	return ret;
}

/*
 * Nothing in U-Boot asks for the MUIC, but the USB lines only reach the AP
 * once it has been set up, so probe it right away.
 */
static int max77836_muic_bind(struct udevice *dev)
{
	dev_or_flags(dev, DM_FLAG_PROBE_AFTER_BIND);

	return 0;
}

static const struct udevice_id max77836_muic_ids[] = {
	{ .compatible = "maxim,max77836-muic" },
	{ }
};

U_BOOT_DRIVER(extcon_max77836) = {
	.name		= "extcon_max77836",
	.id		= UCLASS_EXTCON,
	.of_match	= max77836_muic_ids,
	.bind		= max77836_muic_bind,
	.probe		= max77836_muic_probe,
};
