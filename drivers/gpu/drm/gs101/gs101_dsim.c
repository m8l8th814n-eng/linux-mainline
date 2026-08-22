// SPDX-License-Identifier: GPL-2.0
/*
 * DSIM (MIPI DSI host) for Google gs101 / Tensor G1.
 *
 * SKELETON -- maps registers and reads state. Writes nothing.
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
 */

#include <linux/component.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

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

/* dsim_cal.h:54 -- command mode is reported here, bit 26. */
#define LINK_STATUS1_CMD_MODE_STATUS	BIT(26)

struct gs101_dsim {
	struct device *dev;
	void __iomem *regs;
	u32 id;
};

/*
 * A snapshot of the link as the bootloader left it. Logged once at probe
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

static int gs101_dsim_bind(struct device *dev, struct device *master,
			   void *data)
{
	/*
	 * TODO: register the DSI host (mipi_dsi_host_register), attach the
	 * panel, and replace the fixed connector in gs101_drm_drv.c with a
	 * real one built from the panel's mode list. Nothing to bind until
	 * then.
	 */
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
 * holds that set for as long as it is loaded. Reading registers here works
 * because of that, which is a dependency worth making explicit once this
 * driver does more than read.
 */
static int gs101_dsim_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gs101_dsim *dsim;

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

	gs101_dsim_dump_state(dsim);

	return component_add(dev, &gs101_dsim_component_ops);
}

static void gs101_dsim_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &gs101_dsim_component_ops);
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
