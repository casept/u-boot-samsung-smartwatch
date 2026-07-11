// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos SoC USB 2.0 PHY driver
 *
 * Based on the Linux kernel driver: drivers/phy/samsung/phy-exynos4x12-usb2.c
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 */

#include <dm.h>
#include <generic-phy.h>
#include <regmap.h>
#include <syscon.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <linux/err.h>

/* PHY registers */
#define EXYNOS_4X12_UPHYPWR			0x0
#define EXYNOS_4X12_UPHYPWR_PHY0		(BIT(0) | BIT(3) | BIT(4) | BIT(5))

#define EXYNOS_4X12_UPHYCLK			0x4
#define EXYNOS_4X12_UPHYCLK_PHYFSEL_MASK	0x7
#define EXYNOS_4X12_UPHYCLK_PHYFSEL_24MHZ	0x5
#define EXYNOS_3250_UPHYCLK_REFCLKSEL		(0x2 << 8)

#define EXYNOS_4X12_UPHYRST			0x8
#define EXYNOS_4X12_URSTCON_PHY0		BIT(0)

/* PMU isolation register */
#define EXYNOS_USBPHY_CONTROL			0x0704

struct exynos_usb2_phy {
	void __iomem *reg_phy;
	struct regmap *reg_pmu;
};

static int exynos_usb2_phy_power_on(struct phy *phy)
{
	struct exynos_usb2_phy *priv = dev_get_priv(phy->dev);
	u32 reg;

	/* Set reference clock: 24MHz, REFCLKSEL=0x2 */
	writel(EXYNOS_3250_UPHYCLK_REFCLKSEL |
	       EXYNOS_4X12_UPHYCLK_PHYFSEL_24MHZ,
	       priv->reg_phy + EXYNOS_4X12_UPHYCLK);

	/* Remove USB isolation from PMU */
	regmap_update_bits(priv->reg_pmu, EXYNOS_USBPHY_CONTROL, BIT(0), BIT(0));

	/* Power on PHY0: clear power-down bits */
	reg = readl(priv->reg_phy + EXYNOS_4X12_UPHYPWR);
	reg &= ~EXYNOS_4X12_UPHYPWR_PHY0;
	writel(reg, priv->reg_phy + EXYNOS_4X12_UPHYPWR);

	/* Reset PHY */
	reg = readl(priv->reg_phy + EXYNOS_4X12_UPHYRST);
	reg |= EXYNOS_4X12_URSTCON_PHY0;
	writel(reg, priv->reg_phy + EXYNOS_4X12_UPHYRST);
	udelay(10);
	reg &= ~EXYNOS_4X12_URSTCON_PHY0;
	writel(reg, priv->reg_phy + EXYNOS_4X12_UPHYRST);
	udelay(80);

	return 0;
}

static int exynos_usb2_phy_power_off(struct phy *phy)
{
	struct exynos_usb2_phy *priv = dev_get_priv(phy->dev);
	u32 reg;

	/* Power off PHY0: set power-down bits */
	reg = readl(priv->reg_phy + EXYNOS_4X12_UPHYPWR);
	reg |= EXYNOS_4X12_UPHYPWR_PHY0;
	writel(reg, priv->reg_phy + EXYNOS_4X12_UPHYPWR);

	/* Add USB isolation via PMU */
	regmap_update_bits(priv->reg_pmu, EXYNOS_USBPHY_CONTROL, BIT(0), 0);

	return 0;
}

static int exynos_usb2_phy_probe(struct udevice *dev)
{
	struct exynos_usb2_phy *priv = dev_get_priv(dev);

	priv->reg_phy = dev_read_addr_ptr(dev);
	if (!priv->reg_phy)
		return -EINVAL;

	priv->reg_pmu = syscon_regmap_lookup_by_phandle
		(dev, "samsung,pmureg-phandle");
	if (IS_ERR(priv->reg_pmu)) {
		dev_err(dev, "Failed to lookup PMU regmap\n");
		return PTR_ERR(priv->reg_pmu);
	}

	return 0;
}

static const struct udevice_id exynos_usb2_phy_ids[] = {
	{ .compatible = "samsung,exynos3250-usb2-phy" },
	{ .compatible = "samsung,exynos4x12-usb2-phy" },
	{ }
};

static struct phy_ops exynos_usb2_phy_ops = {
	.power_on = exynos_usb2_phy_power_on,
	.power_off = exynos_usb2_phy_power_off,
};

U_BOOT_DRIVER(exynos_usb2_phy) = {
	.name		= "exynos_usb2_phy",
	.id		= UCLASS_PHY,
	.of_match	= exynos_usb2_phy_ids,
	.probe		= exynos_usb2_phy_probe,
	.ops		= &exynos_usb2_phy_ops,
	.priv_auto	= sizeof(struct exynos_usb2_phy),
};
