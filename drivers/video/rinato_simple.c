// SPDX-License-Identifier: GPL-2.0+
/*
 * Quick-n-dirty framebuffer driver for rinato.
 *
 * Relies on S-Boot to actually initialize hardware, so chainload-only.
 * Basically simplefb, but also has to explicitly trigger frame sync
 * because S-Boot leaves the panel in command mode.
 */

#include <dm.h>
#include <log.h>
#include <video.h>
#include <asm/io.h>

/* FIMD command-mode (i80) trigger control, timing register block. */
#define FIMD_TIMING_BASE	0x11c20000
#define FIMD_TRIGCON		(FIMD_TIMING_BASE + 0x1a4)
#define TRIGCON_TRGMODE_ENABLE	BIT(0)	/* enable SW trigger mode */
#define TRIGCON_SWTRGCMD	BIT(1)	/* issue a software trigger */

static int rinato_simple_sync(struct udevice *dev)
{
	/*
	 * Push the current framebuffer contents into the panel GRAM. The
	 * SWTRGCMD bit self-clears once the transfer starts.
	 */
	setbits_le32(FIMD_TRIGCON, TRIGCON_TRGMODE_ENABLE | TRIGCON_SWTRGCMD);

	return 0;
}

static int rinato_simple_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	fdt_addr_t base;
	fdt_size_t size;
	u32 width, height;

	base = dev_read_addr_size(dev, &size);
	if (base == FDT_ADDR_T_NONE) {
		log_err("rinato-video: missing reg (framebuffer address)\n");
		return -EINVAL;
	}

	if (dev_read_u32(dev, "width", &width) ||
	    dev_read_u32(dev, "height", &height) || !width || !height) {
		log_err("rinato-video: missing/invalid width/height\n");
		return -EINVAL;
	}

	/* Inherit the framebuffer S-Boot's FIMD is already scanning. */
	plat->base = base;
	plat->size = size;
	uc_priv->xsize = width;
	uc_priv->ysize = height;
	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8R8G8B8;

	rinato_simple_sync(dev);

	return 0;
}

static const struct video_ops rinato_simple_ops = {
	.video_sync = rinato_simple_sync,
};

static const struct udevice_id rinato_simple_ids[] = {
	{ .compatible = "samsung,rinato-simple" },
	{ }
};

U_BOOT_DRIVER(rinato_simple) = {
	.name	= "rinato_simple",
	.id	= UCLASS_VIDEO,
	.of_match = rinato_simple_ids,
	.ops	= &rinato_simple_ops,
	.probe	= rinato_simple_probe,
};
