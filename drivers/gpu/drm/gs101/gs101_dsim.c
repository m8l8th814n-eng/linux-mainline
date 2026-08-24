// SPDX-License-Identifier: GPL-2.0
/*
 * DSIM (MIPI DSI host) for Google gs101 / Tensor G1.
 *
 * Maps registers, reads state, and carries DCS commands over the link the
 * bootloader trained. Does not program the PLL, the D-PHY or the lanes.
 *
 * gs101's DSIM is not the one mainline already supports. drivers/gpu/drm/
 * bridge/samsung-dsim.c covers exynos3250 through exynos7870 and i.MX8M, but
 * the register map is a different generation entirely: SWRST is at 0x0C there
 * and 0x04 here, ESCMODE at 0x1C there and 0x2C here. Adding a table entry
 * would not work; this has to be written against the vendor register layer
 * (gs101-display-ref/dsim_reg.c, 2551 lines).
 *
 * That is the largest single piece of work left in the display pipeline, and
 * the hardest to debug: DECON and DPP could be verified a register at a time
 * against a running configuration, whereas a DSI link either trains or it does
 * not. Which is exactly why this file starts by reading rather than writing.
 *
 * The bootloader leaves the link up and the panel scanning out. Everything
 * needed to bring DSIM up later -- PLL dividers, D-PHY timing, lane count,
 * command-mode configuration -- is sitting in these registers right now. Read
 * it before anything overwrites it.
 *
 * As read on oriole with the bootloader's configuration still live and nothing
 * of ours written yet:
 *
 *	VERSION		0x02060000
 *	LINK_STATUS0	0x00000000
 *	LINK_STATUS1	0x04000000	bit 26 alone -- command mode
 *	LINK_STATUS2	0x00000000
 *	LINK_STATUS3	0x00000000
 *	MIPI_STATUS	0x00000000
 *	DPHY_STATUS	0x0000010f	four data lanes plus clock, all stopped
 *	CLK_CTRL	0x06131f04	escape clock dividers and lane enables
 *	ESCMODE		0x01400000
 *
 * Command mode is thus confirmed against the hardware rather than inferred from
 * TRIG_CON, and the link is trained and idle between transfers because the panel
 * refreshes from its own RAM.
 *
 * The version is one revision past anything the vendor layer knows -- dsim_reg.c
 * names only EVT0 0x02040000 and EVT1 0x02050000. That is inert for now: of its
 * 2551 lines exactly one branch is version-dependent (dsim_reg_set_vt_compensate,
 * dsim_reg.c:1397), and it only skips a DSIM_VIDEO_TIMER write that command mode
 * never reaches. Undocumented layout deltas between 2.5 and 2.6 would first show
 * up once this driver starts writing.
 */

#include <linux/component.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_print.h>

#include "gs101_drm.h"

#define DRIVER_NAME "gs101-dsim"

/*
 * regs-dsim.h. The vendor header names four regions -- DSI, PHY, PHY_BIAS and
 * SYS (dsim_cal.h:71) -- but documents base addresses only for the DSI one:
 *
 *	DSIM0	0x1c2c0000
 *	DSIM1	0x1c2d0000
 *
 * Only DSIM0 drives the panel on Pixel 6. The other three regions are needed
 * before the PHY can be programmed; their addresses still have to be found.
 */
#define DSIM_VERSION		0x0000
#define DSIM_SWRST		0x0004
#define DSIM_LINK_STATUS0	0x0008
#define DSIM_LINK_STATUS1	0x000c
#define DSIM_LINK_STATUS2	0x0010
#define DSIM_LINK_STATUS3	0x0014
#define DSIM_MIPI_STATUS	0x0018
#define DSIM_DPHY_STATUS	0x001c
#define DSIM_CLK_CTRL		0x0020
#define DSIM_ESCMODE		0x002c
#define  ESCMODE_CMD_LPDT		BIT(7)
#define DSIM_INTSRC		0x0050
#define  INTSRC_SFR_PL_FIFO_EMPTY	BIT(29)
#define  INTSRC_SFR_PH_FIFO_EMPTY	BIT(28)
#define  INTSRC_SFR_PH_FIFO_OVERFLOW	BIT(27)
#define DSIM_PKTHDR		0x0058
#define  PKTHDR_DATA1(x)		((x) << 16)
#define  PKTHDR_DATA0(x)		((x) << 8)
#define  PKTHDR_ID(x)			((x) << 0)
#define DSIM_PAYLOAD		0x005c
#define DSIM_FIFOCTRL		0x0068
#define  FIFOCTRL_FULL_PH_SFR		BIT(11)
#define  FIFOCTRL_FULL_PL_SFR		BIT(9)

/* dsim_cal.h:54 -- command mode is reported here, bit 26. */
#define LINK_STATUS1_CMD_MODE_STATUS	BIT(26)

struct gs101_dsim {
	struct device *dev;
	void __iomem *regs;
	u32 id;
	struct mipi_dsi_host host;
	struct mipi_dsi_device *panel_dsi;
};

static inline struct gs101_dsim *host_to_dsim(struct mipi_dsi_host *host)
{
	return container_of(host, struct gs101_dsim, host);
}

/*
 * A snapshot of the link as the bootloader left it. Logged once at bind
 * because it is the only chance to see a working configuration -- the moment
 * this driver starts writing, it is gone.
 */
static void gs101_dsim_dump_state(struct gs101_dsim *dsim)
{
	static const struct {
		const char *name;
		u32 offset;
	} regs[] = {
		{ "VERSION",	  DSIM_VERSION },
		{ "LINK_STATUS0", DSIM_LINK_STATUS0 },
		{ "LINK_STATUS1", DSIM_LINK_STATUS1 },
		{ "LINK_STATUS2", DSIM_LINK_STATUS2 },
		{ "LINK_STATUS3", DSIM_LINK_STATUS3 },
		{ "MIPI_STATUS",  DSIM_MIPI_STATUS },
		{ "DPHY_STATUS",  DSIM_DPHY_STATUS },
		{ "CLK_CTRL",	  DSIM_CLK_CTRL },
		{ "ESCMODE",	  DSIM_ESCMODE },
	};
	unsigned int i;
	u32 status1;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		dev_info(dsim->dev, "DSIM%u: %-13s %#010x\n", dsim->id,
			 regs[i].name, readl(dsim->regs + regs[i].offset));

	status1 = readl(dsim->regs + DSIM_LINK_STATUS1);
	dev_info(dsim->dev, "DSIM%u: link reports %s mode\n", dsim->id,
		 status1 & LINK_STATUS1_CMD_MODE_STATUS ? "command" : "video");
}

static int gs101_dsim_host_attach(struct mipi_dsi_host *host,
				  struct mipi_dsi_device *device)
{
	struct gs101_dsim *dsim = host_to_dsim(host);

	if (dsim->panel_dsi)
		return -EBUSY;

	dsim->panel_dsi = device;

	dev_info(dsim->dev, "DSIM%u: attached %s, %u lanes, flags %#lx\n",
		 dsim->id, dev_name(&device->dev), device->lanes,
		 device->mode_flags);

	return 0;
}

static int gs101_dsim_host_detach(struct mipi_dsi_host *host,
				  struct mipi_dsi_device *device)
{
	struct gs101_dsim *dsim = host_to_dsim(host);

	if (dsim->panel_dsi == device)
		dsim->panel_dsi = NULL;

	return 0;
}

static int gs101_dsim_wait_pkthdr_room(struct gs101_dsim *dsim)
{
	u32 val;

	return readl_poll_timeout(dsim->regs + DSIM_FIFOCTRL, val,
				  !(val & FIFOCTRL_FULL_PH_SFR), 10, 20000);
}

static ssize_t gs101_dsim_host_transfer(struct mipi_dsi_host *host,
					const struct mipi_dsi_msg *msg)
{
	struct gs101_dsim *dsim = host_to_dsim(host);
	struct mipi_dsi_packet packet;
	const u8 *tx;
	u32 escmode;
	u32 fifo;
	size_t i;
	u32 val;
	int ret;

	if (msg->rx_len || msg->rx_buf)
		return -EOPNOTSUPP;

	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret)
		return ret;

	escmode = readl(dsim->regs + DSIM_ESCMODE);
	writel(escmode | ESCMODE_CMD_LPDT, dsim->regs + DSIM_ESCMODE);

	writel(INTSRC_SFR_PH_FIFO_EMPTY | INTSRC_SFR_PL_FIFO_EMPTY |
	       INTSRC_SFR_PH_FIFO_OVERFLOW, dsim->regs + DSIM_INTSRC);

	tx = packet.payload;
	for (i = 0; i < packet.payload_length; i += 4) {
		size_t n = min_t(size_t, 4, packet.payload_length - i);
		size_t j;

		val = 0;
		for (j = 0; j < n; j++)
			val |= (u32)tx[i + j] << (8 * j);

		ret = readl_poll_timeout(dsim->regs + DSIM_FIFOCTRL, fifo,
					 !(fifo & FIFOCTRL_FULL_PL_SFR),
					 10, 20000);
		if (ret)
			goto out;

		writel(val, dsim->regs + DSIM_PAYLOAD);
	}

	ret = gs101_dsim_wait_pkthdr_room(dsim);
	if (ret)
		goto out;

	writel(PKTHDR_ID(packet.header[0]) | PKTHDR_DATA0(packet.header[1]) |
	       PKTHDR_DATA1(packet.header[2]), dsim->regs + DSIM_PKTHDR);

	ret = readl_poll_timeout(dsim->regs + DSIM_INTSRC, val,
				 val & INTSRC_SFR_PH_FIFO_EMPTY, 10, 20000);
	if (ret)
		dev_err(dsim->dev, "DSIM%u: packet header FIFO stuck, INTSRC %#010x\n",
			dsim->id, readl(dsim->regs + DSIM_INTSRC));

out:
	escmode = readl(dsim->regs + DSIM_ESCMODE);
	writel(escmode & ~ESCMODE_CMD_LPDT, dsim->regs + DSIM_ESCMODE);

	return ret ? ret : msg->tx_len;
}

static const struct mipi_dsi_host_ops gs101_dsim_host_ops = {
	.attach		= gs101_dsim_host_attach,
	.detach		= gs101_dsim_host_detach,
	.transfer	= gs101_dsim_host_transfer,
};

static int gs101_dsim_bind(struct device *dev, struct device *master,
			   void *data)
{
	struct gs101_dsim *dsim = dev_get_drvdata(dev);

	/*
	 * Read the registers here rather than in probe. DSIM has no clocks of
	 * its own and only answers while DECON holds cmu_dpu enabled, which
	 * DECON does from its probe. Probe order between the two is not
	 * guaranteed, and a read of an ungated block does not return an error
	 * -- it hangs the bus until the watchdog reboots the machine. The
	 * master binds only once every component has probed, so by this point
	 * DECON's clocks are on.
	 */
	gs101_dsim_dump_state(dsim);

	return 0;
}

static void gs101_dsim_unbind(struct device *dev, struct device *master,
			      void *data)
{
}

static const struct component_ops gs101_dsim_component_ops = {
	.bind	= gs101_dsim_bind,
	.unbind	= gs101_dsim_unbind,
};

/*
 * Device tree node:
 *
 *	dsim0: dsi@1c2c0000 {
 *		compatible = "google,gs101-dsim";
 *		reg = <0x1c2c0000 0x10000>;
 *		reg-names = "dsi";
 *		dsim,id = <0>;
 *	};
 *
 * No clocks: DSIM sits behind cmu_dpu like DECON and DPP, and DECON already
 * holds that set for as long as it is loaded. Probe therefore touches no
 * registers at all -- see gs101_dsim_bind() -- and this node will need its own
 * clocks before the driver does more than read.
 */
static int gs101_dsim_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gs101_dsim *dsim;
	int ret;

	dsim = devm_kzalloc(dev, sizeof(*dsim), GFP_KERNEL);
	if (!dsim)
		return -ENOMEM;

	dsim->dev = dev;
	platform_set_drvdata(pdev, dsim);

	if (of_property_read_u32(dev->of_node, "dsim,id", &dsim->id))
		dsim->id = 0;

	dsim->regs = devm_platform_ioremap_resource_byname(pdev, "dsi");
	if (IS_ERR(dsim->regs))
		return dev_err_probe(dev, PTR_ERR(dsim->regs),
				     "failed to map DSI registers\n");

	/*
	 * Registered here, not in bind: mipi_dsi_host_register() creates the
	 * panel's DSI device, but the panel driver then probes asynchronously,
	 * and gs101_drm_bind() needs the panel already registered with
	 * drm_panel. Doing this from bind made the first bind find no panel,
	 * defer, and come back to a host that was already registered.
	 *
	 * It touches no DSIM registers, and neither does the panel's probe --
	 * only a transfer does, by which point DECON holds cmu_dpu enabled.
	 */
	dsim->host.dev = dev;
	dsim->host.ops = &gs101_dsim_host_ops;

	ret = mipi_dsi_host_register(&dsim->host);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register DSI host\n");

	ret = component_add(dev, &gs101_dsim_component_ops);
	if (ret)
		mipi_dsi_host_unregister(&dsim->host);

	return ret;
}

static void gs101_dsim_remove(struct platform_device *pdev)
{
	struct gs101_dsim *dsim = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &gs101_dsim_component_ops);
	mipi_dsi_host_unregister(&dsim->host);
}

static const struct of_device_id gs101_dsim_of_match[] = {
	{ .compatible = "google,gs101-dsim" },
	{ }
};
MODULE_DEVICE_TABLE(of, gs101_dsim_of_match);

/* Registered by gs101_drm_drv.c, which owns the module init path. */
struct platform_driver gs101_dsim_driver = {
	.probe	= gs101_dsim_probe,
	.remove	= gs101_dsim_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= gs101_dsim_of_match,
	},
};
