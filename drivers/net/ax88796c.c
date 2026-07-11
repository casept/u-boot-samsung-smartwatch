// SPDX-License-Identifier: GPL-2.0+
/*
 * ASIX AX88796C SPI Ethernet driver for U-Boot
 *
 * DM SPI child driver: the device sits on an SPI bus described in the
 * devicetree (chip select via the bus, chip reset via "reset-gpios").
 *
 * Register access sequences follow the vendor driver (downstream u-boot
 * 2012.07 drivers/net/ax88796c_spi.c) and the Linux ax88796c driver.
 */

#include <log.h>
#include <net.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <spi.h>
#include <asm/gpio.h>
#include <linux/err.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/printk.h>

/* SPI command opcodes */
#define AX_SPICMD_READ_REG	0x03
#define AX_SPICMD_READ_RXQ	0x0B
#define AX_SPICMD_WRITE_TXQ	0x02
#define AX_SPICMD_WRITE_REG	0xD8

/* Page 0 registers */
#define P0_PSR		0x00
#define   PSR_RESET		0
#define   PSR_RESET_CLR		BIT(15)
#define   PSR_DEV_READY		BIT(7)
#define P0_FER		0x04
#define   FER_IPALM		BIT(0)
#define   FER_BSWAP		BIT(9)
#define   FER_RXEN		BIT(14)
#define   FER_TXEN		BIT(15)
#define P0_ISR		0x06
#define   ISR_RXPKT		BIT(0)
#define   ISR_TXERR		BIT(8)
#define   ISR_LINK		BIT(9)
#define P0_PSCR		0x0C
#define   PSCR_PHYLINK		BIT(14)
#define P0_TSNR		0x12
#define   TSNR_TXB_ERR		BIT(5)
#define   TSNR_TXB_IDLE		BIT(6)
#define   TSNR_PKT_CNT(x)	(((x) & 0x3F) << 8)
#define   TSNR_TXB_REINIT	BIT(14)
#define   TSNR_TXB_START	BIT(15)
#define P0_RXBCR1	0x16
#define   RXBCR1_RXB_DISCARD	BIT(14)
#define   RXBCR1_RXB_START	BIT(15)
#define P0_RXBCR2	0x18
#define   RXBCR2_PKT_MASK	0xFF
#define   RXBCR2_RXB_READY	BIT(13)
#define   RXBCR2_RXB_IDLE	BIT(14)
#define   RXBCR2_RXB_REINIT	BIT(15)
#define P0_RTWCR	0x1A
#define   RTWCR_RX_LATCH	BIT(15)
#define P0_RCPHR	0x1C
#define   RX_HDR_ERROR		0x7000
#define   RX_HDR_LEN		0x07FF

/* Page 1 registers */
#define P1_RPPER	0x22
#define   RPPER_RXEN		BIT(0)
#define P1_RXBSPCR	0x30

/* Page 2 registers */
#define P2_POOLCR	0x44
#define   POOLCR_POLL_EN	BIT(0)
#define   POOLCR_POLL_BMCR	BIT(2)
#define   POOLCR_PHYID(x)	((x) << 8)
#define P2_MDIODR	0x48
#define P2_MDIOCR	0x4A
#define   MDIOCR_RADDR(x)	((x) & 0x1F)
#define   MDIOCR_FADDR(x)	(((x) & 0x1F) << 8)
#define   MDIOCR_VALID		BIT(13)
#define   MDIOCR_READ		BIT(14)
#define   MDIOCR_WRITE		BIT(15)
#define P2_LCR0		0x4C
#define   LCR_LED0_EN		BIT(0)
#define   LCR_LED0_DUPLEX	BIT(2)
#define   LCR_LED1_EN		BIT(8)
#define   LCR_LED1_100MODE	BIT(9)
#define P2_LCR1		0x4E
#define   LCR_LED2_MASK		0xFF00
#define   LCR_LED2_EN		BIT(0)
#define   LCR_LED2_LINK		BIT(3)
#define P2_RXCR		0x56
#define   RXCR_AB		BIT(3)

/* Page 3 registers */
#define P3_MACASR0	0x62
#define P3_MACASR1	0x64
#define P3_MACASR2	0x66
#define P3_EECR		0x78
#define   EECR_RELOAD		BIT(14)

/* PHY registers */
#define PHY_BMCR	0x00
#define   BMCR_RST_NEG		BIT(9)
#define   BMCR_ANEN		BIT(12)
#define   BMCR_SPEED_100	BIT(13)
#define PHY_ANAR	0x04
#define   ANAR_ADVERTISE	0x25E1

/* TX/RX queue framing */
#define TX_HDR_SEG_FS		0x8000
#define TX_HDR_SEG_LS		0x4000
#define TX_HDR_SEQNUM(x)	(((x) & 0x1F) << 11)
#define TX_LENBAR(len)		(~(len) & 0x07FF)
#define TX_HDR_SIZE		8
#define TX_EOP_SIZE		4
#define RX_HDR_SIZE		6

#define AX_PHY_ID		0x10
#define AX_MAX_RX_LEN		1600

struct ax88796c_priv {
	u16 seq_num;
	struct gpio_desc reset_gpio;
	/*
	 * Bounce buffers, 4-byte aligned and padded to multiples of 4:
	 * many SPI controllers move aligned buffers with wider accesses.
	 */
	u8 tx_buf[4 + TX_HDR_SIZE + ALIGN(PKTSIZE_ALIGN, 4) + TX_EOP_SIZE]
		__aligned(4);
	u8 rx_buf[ALIGN(AX_MAX_RX_LEN + RX_HDR_SIZE, 4)] __aligned(4);
};

static int ax88796c_write_hwaddr_op(struct udevice *dev);

/* --- register access over SPI --- */

static u16 ax88796c_read_reg(struct udevice *dev, u8 reg)
{
	u8 cmd[4] = { AX_SPICMD_READ_REG, reg, 0xFF, 0xFF };
	u8 rx[2] = { 0xFF, 0xFF };

	dm_spi_xfer(dev, 4 * 8, cmd, NULL, SPI_XFER_BEGIN);
	dm_spi_xfer(dev, 2 * 8, NULL, rx, SPI_XFER_END);

	return rx[0] | (rx[1] << 8);
}

static void ax88796c_write_reg(struct udevice *dev, u8 reg, u16 val)
{
	u8 cmd[4] = { AX_SPICMD_WRITE_REG, reg, val & 0xFF, val >> 8 };

	dm_spi_xfer(dev, 4 * 8, cmd, NULL, SPI_XFER_BEGIN | SPI_XFER_END);
}

/* Poll @reg until all bits in @mask are set; 10us granularity */
static int ax88796c_poll_reg(struct udevice *dev, u8 reg, u16 mask,
			     int timeout_us)
{
	int waited = 0;

	for (;;) {
		if ((ax88796c_read_reg(dev, reg) & mask) == mask)
			return 0;
		if (waited >= timeout_us)
			return -ETIMEDOUT;
		udelay(10);
		waited += 10;
	}
}

static int ax88796c_soft_reset(struct udevice *dev)
{
	ax88796c_write_reg(dev, P0_PSR, PSR_RESET);
	ax88796c_write_reg(dev, P0_PSR, PSR_RESET_CLR);

	return ax88796c_poll_reg(dev, P0_PSR, PSR_DEV_READY, 100000);
}

static void ax88796c_reload_eeprom(struct udevice *dev)
{
	ax88796c_write_reg(dev, P3_EECR, EECR_RELOAD);
	ax88796c_poll_reg(dev, P0_PSR, PSR_DEV_READY, 10000);
}

static int ax88796c_mdio_write(struct udevice *dev, u8 loc, u16 val)
{
	ax88796c_write_reg(dev, P2_MDIODR, val);
	ax88796c_write_reg(dev, P2_MDIOCR, MDIOCR_RADDR(loc) |
			   MDIOCR_FADDR(AX_PHY_ID) | MDIOCR_WRITE);

	return ax88796c_poll_reg(dev, P2_MDIOCR, MDIOCR_VALID, 10000);
}

static void ax88796c_read_hwaddr(struct udevice *dev, u8 *enetaddr)
{
	u16 val;

	val = ax88796c_read_reg(dev, P3_MACASR0);
	enetaddr[4] = val >> 8; enetaddr[5] = val & 0xFF;
	val = ax88796c_read_reg(dev, P3_MACASR1);
	enetaddr[2] = val >> 8; enetaddr[3] = val & 0xFF;
	val = ax88796c_read_reg(dev, P3_MACASR2);
	enetaddr[0] = val >> 8; enetaddr[1] = val & 0xFF;

	/* Boards without an EEPROM read zeros: fall back to a fixed
	 * locally administered address */
	if (!is_valid_ethaddr(enetaddr)) {
		memset(enetaddr, 0, 6);
		enetaddr[0] = 0x02;
		enetaddr[5] = 0x01;
	}
}

/* --- eth_ops --- */

static int ax88796c_eth_start(struct udevice *dev)
{
	struct ax88796c_priv *priv = dev_get_priv(dev);

	priv->seq_num = 0;

	/* Enable RX packet processing, no RX stuffing padding */
	ax88796c_write_reg(dev, P1_RPPER, RPPER_RXEN);
	ax88796c_write_reg(dev, P1_RXBSPCR, 0);

	/* Byte swap, enable TX/RX bridge */
	ax88796c_write_reg(dev, P0_FER, ax88796c_read_reg(dev, P0_FER) |
			   FER_IPALM | FER_BSWAP | FER_RXEN | FER_TXEN);

	ax88796c_write_hwaddr_op(dev);

	/* Unicast + broadcast */
	ax88796c_write_reg(dev, P2_RXCR, RXCR_AB);

	ax88796c_write_reg(dev, P2_LCR0, LCR_LED0_EN | LCR_LED0_DUPLEX |
			   LCR_LED1_EN | LCR_LED1_100MODE);
	ax88796c_write_reg(dev, P2_LCR1,
			   (ax88796c_read_reg(dev, P2_LCR1) & LCR_LED2_MASK) |
			   LCR_LED2_EN | LCR_LED2_LINK);

	/* PHY auto-polling so the MAC tracks link speed/duplex */
	ax88796c_write_reg(dev, P2_POOLCR, POOLCR_PHYID(AX_PHY_ID) |
			   POOLCR_POLL_EN | POOLCR_POLL_BMCR);

	/* Wait for PHY link so the first TX isn't lost to autoneg */
	if (ax88796c_poll_reg(dev, P0_PSCR, PSCR_PHYLINK, 5000000))
		printf("ax88796c: no link (cable plugged in?)\n");

	return 0;
}

static void put_be16(u8 *p, u16 val)
{
	p[0] = val >> 8;
	p[1] = val & 0xFF;
}

static int ax88796c_eth_send(struct udevice *dev, void *packet, int length)
{
	struct ax88796c_priv *priv = dev_get_priv(dev);
	u16 len_bar = TX_LENBAR(length);
	int padded_len = roundup(length, 4);
	int burst_len;
	u8 *p = priv->tx_buf;
	u16 tsnr;

	/* Assemble the whole burst so it can go out as one aligned
	 * transfer: CMD | SOP+SEG header | packet | pad | EOP */
	p[0] = AX_SPICMD_WRITE_TXQ;
	p[1] = 0xFF;
	p[2] = 0xFF;
	p[3] = 0xFF;
	put_be16(p + 4, length);				/* SOP */
	put_be16(p + 6, TX_HDR_SEQNUM(priv->seq_num) | len_bar);
	put_be16(p + 8, TX_HDR_SEG_FS | TX_HDR_SEG_LS | length); /* SEG */
	put_be16(p + 10, len_bar);
	memcpy(p + 12, packet, length);
	memset(p + 12 + length, 0xFF, padded_len - length);
	put_be16(p + 12 + padded_len,				/* EOP */
		 TX_HDR_SEQNUM(priv->seq_num) | length);
	put_be16(p + 14 + padded_len,
		 TX_HDR_SEQNUM(~priv->seq_num) | len_bar);
	burst_len = 4 + TX_HDR_SIZE + padded_len + TX_EOP_SIZE;

	ax88796c_write_reg(dev, P0_TSNR, TSNR_TXB_START | TSNR_PKT_CNT(1));

	dm_spi_xfer(dev, burst_len * 8, p, NULL,
		    SPI_XFER_BEGIN | SPI_XFER_END);

	if (ax88796c_poll_reg(dev, P0_TSNR, TSNR_TXB_IDLE, 10000))
		goto err;
	tsnr = ax88796c_read_reg(dev, P0_TSNR);
	if ((tsnr & TSNR_TXB_ERR) ||
	    (ax88796c_read_reg(dev, P0_ISR) & ISR_TXERR))
		goto err;

	priv->seq_num = (priv->seq_num + 1) & 0x1F;
	return 0;

err:
	ax88796c_write_reg(dev, P0_ISR, ISR_TXERR);
	ax88796c_write_reg(dev, P0_TSNR, TSNR_TXB_REINIT);
	priv->seq_num = 0;
	return -EIO;
}

static int ax88796c_eth_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct ax88796c_priv *priv = dev_get_priv(dev);
	u8 cmd[5] = { AX_SPICMD_READ_RXQ, 0xFF, 0xFF, 0xFF, 0xFF };
	u16 isr, rcphr, pkt_cnt, pkt_len;
	int burst_len;

	isr = ax88796c_read_reg(dev, P0_ISR);
	if (isr == 0xFFFF)	/* SPI bus wedged / chip absent */
		return 0;
	if (isr)		/* ack everything we saw */
		ax88796c_write_reg(dev, P0_ISR, isr);

	/* Latch RX packet count and header */
	ax88796c_write_reg(dev, P0_RTWCR,
			   ax88796c_read_reg(dev, P0_RTWCR) | RTWCR_RX_LATCH);

	pkt_cnt = ax88796c_read_reg(dev, P0_RXBCR2) & RXBCR2_PKT_MASK;
	if (!pkt_cnt)
		return 0;

	rcphr = ax88796c_read_reg(dev, P0_RCPHR);
	pkt_len = rcphr & RX_HDR_LEN;
	if ((rcphr & RX_HDR_ERROR) || pkt_len < 60 || pkt_len > 1518) {
		ax88796c_write_reg(dev, P0_RXBCR1, RXBCR1_RXB_DISCARD);
		return 0;
	}

	/* Burst length: header + packet, dword aligned */
	burst_len = roundup(pkt_len + RX_HDR_SIZE, 4);
	ax88796c_write_reg(dev, P0_RXBCR1,
			   RXBCR1_RXB_START | (burst_len / 2));

	if (ax88796c_poll_reg(dev, P0_RXBCR2, RXBCR2_RXB_READY, 10000)) {
		ax88796c_write_reg(dev, P0_RXBCR1, RXBCR1_RXB_DISCARD);
		ax88796c_write_reg(dev, P0_RXBCR2, RXBCR2_RXB_REINIT);
		return 0;
	}

	dm_spi_xfer(dev, 5 * 8, cmd, NULL, SPI_XFER_BEGIN);
	dm_spi_xfer(dev, burst_len * 8, NULL, priv->rx_buf, SPI_XFER_END);

	if (ax88796c_poll_reg(dev, P0_RXBCR2, RXBCR2_RXB_IDLE, 10000))
		ax88796c_write_reg(dev, P0_RXBCR2, RXBCR2_RXB_REINIT);

	*packetp = priv->rx_buf + RX_HDR_SIZE;
	return pkt_len;
}

static int ax88796c_eth_free_pkt(struct udevice *dev, uchar *packet, int length)
{
	return 0;
}

static void ax88796c_eth_stop(struct udevice *dev)
{
	ax88796c_write_reg(dev, P0_FER, ax88796c_read_reg(dev, P0_FER) &
			   ~(FER_RXEN | FER_TXEN));
}

static int ax88796c_write_hwaddr_op(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	u8 *a = pdata->enetaddr;

	ax88796c_write_reg(dev, P3_MACASR0, (a[4] << 8) | a[5]);
	ax88796c_write_reg(dev, P3_MACASR1, (a[2] << 8) | a[3]);
	ax88796c_write_reg(dev, P3_MACASR2, (a[0] << 8) | a[1]);
	return 0;
}

static int ax88796c_eth_probe(struct udevice *dev)
{
	struct ax88796c_priv *priv = dev_get_priv(dev);
	struct eth_pdata *pdata = dev_get_plat(dev);
	int ret;

	ret = gpio_request_by_name(dev, "reset-gpios", 0, &priv->reset_gpio,
				   GPIOD_IS_OUT);
	if (ret && ret != -ENOENT) {
		dev_err(dev, "cannot get reset-gpios: %d\n", ret);
		return ret;
	}

	ret = dm_spi_claim_bus(dev);
	if (ret) {
		dev_err(dev, "cannot claim SPI bus: %d\n", ret);
		return ret;
	}

	/* Pulse the hardware reset line, if wired up */
	if (dm_gpio_is_valid(&priv->reset_gpio)) {
		dm_gpio_set_value(&priv->reset_gpio, 1);
		mdelay(10);
		dm_gpio_set_value(&priv->reset_gpio, 0);
		mdelay(10);
	}

	ret = ax88796c_soft_reset(dev);
	if (ret) {
		dev_err(dev, "reset timeout\n");
		dm_spi_release_bus(dev);
		return -ENODEV;
	}

	ax88796c_reload_eeprom(dev);
	ax88796c_read_hwaddr(dev, pdata->enetaddr);

	/* Kick off autonegotiation early so link is up by eth_start */
	ax88796c_mdio_write(dev, PHY_ANAR, ANAR_ADVERTISE);
	ax88796c_mdio_write(dev, PHY_BMCR,
			    BMCR_ANEN | BMCR_SPEED_100 | BMCR_RST_NEG);

	printf("AX88796C: MAC %pM\n", pdata->enetaddr);
	return 0;
}

static const struct eth_ops ax88796c_eth_ops = {
	.start		= ax88796c_eth_start,
	.send		= ax88796c_eth_send,
	.recv		= ax88796c_eth_recv,
	.free_pkt	= ax88796c_eth_free_pkt,
	.stop		= ax88796c_eth_stop,
	.write_hwaddr	= ax88796c_write_hwaddr_op,
};

static const struct udevice_id ax88796c_ids[] = {
	{ .compatible = "asix,ax88796c" },
	{ }
};

U_BOOT_DRIVER(ax88796c) = {
	.name		= "ax88796c",
	.id		= UCLASS_ETH,
	.of_match	= ax88796c_ids,
	.probe		= ax88796c_eth_probe,
	.ops		= &ax88796c_eth_ops,
	.priv_auto	= sizeof(struct ax88796c_priv),
	.plat_auto	= sizeof(struct eth_pdata),
};
