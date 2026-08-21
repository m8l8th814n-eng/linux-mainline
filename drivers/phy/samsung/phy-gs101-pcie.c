// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe PHY driver for the Google gs101 SoC (HSI1, Gen4A)
 *
 * The PMA register sequences below are transcribed from the vendor kernel,
 * google-modules/soc/gs/drivers/pci/controller/dwc/pcie-exynos-gs101-rc-cal.c
 * Copyright (C) 2020 Samsung Electronics Co., Ltd.
 *
 * The values are calibration data with no published derivation; they are
 * reproduced verbatim and should not be "cleaned up".
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

struct gs101_pcie_phy {
	struct device		*dev;
	void __iomem		*phy_base;
	void __iomem		*pcs_base;
	void __iomem		*elbi_base;
	struct clk_bulk_data	*clks;
	int			num_clks;
	int			num_lanes;
};

/* PHY all power down */
static void gs101_pcie_phy_pwrdn(struct gs101_pcie_phy *phy)
{
	void __iomem *phy_base_regs = phy->phy_base;
	u32 val;

	writel(0xA8, phy_base_regs + 0x404);
	writel(0x20, phy_base_regs + 0x408);
	writel(0x0A, phy_base_regs + 0x40C);

	writel(0x0A, phy_base_regs + 0x800);
	writel(0xBF, phy_base_regs + 0x804);
	writel(0x02, phy_base_regs + 0xA40);
	writel(0x2A, phy_base_regs + 0xA44);
	writel(0xAA, phy_base_regs + 0xA48);
	writel(0xA8, phy_base_regs + 0xA4C);
	writel(0x80, phy_base_regs + 0xA50);

	writel(0x0A, phy_base_regs + 0x1000);
	writel(0xBF, phy_base_regs + 0x1004);
	writel(0x02, phy_base_regs + 0x1240);
	writel(0x2A, phy_base_regs + 0x1244);
	writel(0xAA, phy_base_regs + 0x1248);
	writel(0xA8, phy_base_regs + 0x124C);
	writel(0x80, phy_base_regs + 0x1250);

	/* Disable PHY PMA */
	val = readl(phy_base_regs + 0x400);
	val &= ~(0x1 << 7);
	writel(val, phy_base_regs + 0x400);
}

/* PHY all power down clear */
static void gs101_pcie_phy_pwrdn_clear(struct gs101_pcie_phy *phy)
{
	void __iomem *phy_base_regs = phy->phy_base;

	writel(0x28, phy_base_regs + 0xD8);
	mdelay(1);

	writel(0x00, phy_base_regs + 0x404);
	writel(0x00, phy_base_regs + 0x408);
	writel(0x00, phy_base_regs + 0x40C);

	writel(0x00, phy_base_regs + 0x800);
	writel(0x00, phy_base_regs + 0x804);
	writel(0x00, phy_base_regs + 0xA40);
	writel(0x00, phy_base_regs + 0xA44);
	writel(0x00, phy_base_regs + 0xA48);
	writel(0x00, phy_base_regs + 0xA4C);
	writel(0x00, phy_base_regs + 0xA50);

	writel(0x00, phy_base_regs + 0x1000);
	writel(0x00, phy_base_regs + 0x1004);
	writel(0x00, phy_base_regs + 0x1240);
	writel(0x00, phy_base_regs + 0x1244);
	writel(0x00, phy_base_regs + 0x1248);
	writel(0x00, phy_base_regs + 0x124C);
	writel(0x00, phy_base_regs + 0x1250);
}

static void gs101_pcie_phy_config(struct gs101_pcie_phy *phy)
{
	void __iomem *elbi_base_regs = phy->elbi_base;
	void __iomem *phy_base_regs = phy->phy_base;
	void __iomem *phy_pcs_base_regs = phy->pcs_base;
	int num_lanes = phy->num_lanes;
	u32 val;
	u32 i;

	/* init. input clk path */
	writel(0x28, phy_base_regs + 0xD8);

	/* PCS MUX glitch W/A */
	val = readl(phy->pcs_base + 0x008);
	val |= (1 << 7);
	writel(val, phy_pcs_base_regs + 0x008);
	if (num_lanes == 2) {
		val = readl(phy->pcs_base + 0x808);
		val |= (1 << 7);
		writel(val, phy_pcs_base_regs + 0x808);
	}

	/* PHY CMN_RST, INIT_RST, PORT_RST Assert */
	writel(0x1, elbi_base_regs + 0x1404);
	writel(0x1, elbi_base_regs + 0x1408);
	writel(0x1, elbi_base_regs + 0x1400);
	writel(0x0, elbi_base_regs + 0x1404);
	writel(0x0, elbi_base_regs + 0x1408);
	writel(0x0, elbi_base_regs + 0x1400);
	udelay(10);

	writel(0x1, elbi_base_regs + 0x1404);
	udelay(10);

	/* pma_setting */
	/* Common */
	writel(0x50, phy_base_regs + 0x018);
	writel(0x33, phy_base_regs + 0x048);
	writel(0x01, phy_base_regs + 0x068);
	writel(0x12, phy_base_regs + 0x070);
	writel(0x00, phy_base_regs + 0x08C);
	writel(0x21, phy_base_regs + 0x090);
	writel(0x14, phy_base_regs + 0x0B0);
	writel(0x50, phy_base_regs + 0x0B8);
	writel(0x51, phy_base_regs + 0x0E0);
	writel(0x00, phy_base_regs + 0x100);
	writel(0x80, phy_base_regs + 0x104);
	writel(0x38, phy_base_regs + 0x140);
	writel(0xA4, phy_base_regs + 0x180);
	writel(0x03, phy_base_regs + 0x188); /* LCPLL 100MHz no divide */
	writel(0x38, phy_base_regs + 0x2A8);
	writel(0x12, phy_base_regs + 0x2E4);
	/* PMA enable and aggregation mode(bifurcation mode: 0xC0) */
	writel(0x80, phy_base_regs + 0x400);
	writel(0x20, phy_base_regs + 0x408);
	writel(0x00, phy_base_regs + 0x550);
	writel(0x00, phy_base_regs + 0x5A8);
	writel(0xFF, phy_base_regs + 0x5EC);

	/* PCIe_TYPE: RC */
	writel(0x02, phy_base_regs + 0x458); /* 100MHz CLK on */
	writel(0x34, phy_base_regs + 0x5B0); /* diff,control REFCLK source */
	writel(0x20, phy_base_regs + 0x450); /* when entering L1.2, add delay */

	for (i = 0; i < num_lanes; i++) {
		phy_base_regs += (i * 0x800);

		writel(0x08, phy_base_regs + 0x82C);
		writel(0x24, phy_base_regs + 0x830);
		writel(0x80, phy_base_regs + 0x878);
		writel(0x40, phy_base_regs + 0x894);
		writel(0x00, phy_base_regs + 0x8C0);
		writel(0x30, phy_base_regs + 0x8F4);
		writel(0x05, phy_base_regs + 0x908);
		writel(0xE0, phy_base_regs + 0x90C);
		writel(0xD4, phy_base_regs + 0x914);
		writel(0xD3, phy_base_regs + 0x91C);
		writel(0xCE, phy_base_regs + 0x920);
		writel(0x01, phy_base_regs + 0x924);
		writel(0x35, phy_base_regs + 0x928);
		writel(0xBA, phy_base_regs + 0x92C);
		writel(0x41, phy_base_regs + 0x930);
		writel(0x15, phy_base_regs + 0x934);
		writel(0x13, phy_base_regs + 0x938);
		writel(0x4E, phy_base_regs + 0x93C);
		writel(0x43, phy_base_regs + 0x948);
		writel(0xFC, phy_base_regs + 0x94C);
		writel(0x10, phy_base_regs + 0x954);
		writel(0x69, phy_base_regs + 0x958);
		writel(0x40, phy_base_regs + 0x964);
		writel(0xF6, phy_base_regs + 0x9B4);
		writel(0x2D, phy_base_regs + 0x9C0);
		writel(0xB7, phy_base_regs + 0x9C4);
		writel(0x3C, phy_base_regs + 0x9CC);
		writel(0x7E, phy_base_regs + 0x9DC);
		writel(0x02, phy_base_regs + 0xA40);
		writel(0x26, phy_base_regs + 0xA70);
		writel(0x00, phy_base_regs + 0xA74);
		writel(0x06, phy_base_regs + 0xB40);	/* hf_init */
		writel(0x06, phy_base_regs + 0xB44);
		writel(0x04, phy_base_regs + 0xB48);	/* value changed: 0x06 -> 0x04 */
		writel(0x03, phy_base_regs + 0xB4C);	/* value changed: 0x04 -> 0x03 */
		writel(0x03, phy_base_regs + 0xB50);	/* mf_init */
		writel(0x03, phy_base_regs + 0xB54);
		writel(0x03, phy_base_regs + 0xB58);
		writel(0x03, phy_base_regs + 0xB5C);
		writel(0x1B, phy_base_regs + 0xC10);
		writel(0x10, phy_base_regs + 0xC44);
		writel(0x10, phy_base_regs + 0xC48);
		writel(0x10, phy_base_regs + 0xC4C);
		writel(0x10, phy_base_regs + 0xC50);
		writel(0x02, phy_base_regs + 0xC54);
		writel(0x02, phy_base_regs + 0xC58);
		writel(0x02, phy_base_regs + 0xC5C);
		writel(0x02, phy_base_regs + 0xC60);
		writel(0x02, phy_base_regs + 0xC6C);
		writel(0x02, phy_base_regs + 0xC70);
		writel(0xE7, phy_base_regs + 0xCA8);
		writel(0x00, phy_base_regs + 0xCAC);
		writel(0x0E, phy_base_regs + 0xCB0);
		writel(0x1C, phy_base_regs + 0xCCC);
		writel(0x05, phy_base_regs + 0xCD4);
		writel(0x77, phy_base_regs + 0xCD8);
		writel(0x7A, phy_base_regs + 0xCDC);
		writel(0x2F, phy_base_regs + 0xDB4);

		/* for GEN4 TX termination resistor? */
		writel(0x00, phy_base_regs + 0x9CC);
		writel(0xFF, phy_base_regs + 0xCD8);
		writel(0x6E, phy_base_regs + 0xCDC);

		/* TX idrv for termination resistor (only EVT0) */
		writel(0x0F, phy_base_regs + 0x82C);
		writel(0x60, phy_base_regs + 0x830);
		writel(0x7E, phy_base_regs + 0x834);

		/* TX pre_lvl */
		writel(0x10, phy_base_regs + 0x824); /* pre */
		writel(0x4F, phy_base_regs + 0x82C); /* [7:4]=g4 */
		writel(0x1F, phy_base_regs + 0x818); /* post */
		writel(0x00, phy_base_regs + 0x820); /* [3:0] */

		/* RX tuning */
		writel(0x3D, phy_base_regs + 0x928);
		writel(0xCA, phy_base_regs + 0x920);
		writel(0x2F, phy_base_regs + 0xA08);
		writel(0xB8, phy_base_regs + 0x92C);
		writel(0xF4, phy_base_regs + 0x914);
		writel(0x55, phy_base_regs + 0x96C);
		writel(0x78, phy_base_regs + 0x988);
		writel(0x17, phy_base_regs + 0x934);
		writel(0x4C, phy_base_regs + 0x93C);
		writel(0x3B, phy_base_regs + 0x994);
		writel(0x73, phy_base_regs + 0x948);
		writel(0x3F, phy_base_regs + 0xB9C);
		writel(0x20, phy_base_regs + 0x9C8);
		writel(0xFF, phy_base_regs + 0x9C4);
		writel(0x05, phy_base_regs + 0xC08);
		writel(0x04, phy_base_regs + 0xC3C);
		writel(0x04, phy_base_regs + 0xC40);
	}

	/* PCS setting: pcie_pcs_setting() in F/W code */

	/* aggregation mdoe(bifurcation disabled) */
	writel(0x00, phy_pcs_base_regs + 0x004);
	if (num_lanes == 2)
		writel(0x00, phy_pcs_base_regs + 0x804);

	/* if RC */
	writel(0x700D5, phy_pcs_base_regs + 0x154);
	if (num_lanes == 2)
		writel(0x700D5, phy_pcs_base_regs + 0x954);

	/* add for L2 entry and power */
	writel(0x300FF, phy_pcs_base_regs + 0x150);
	if (num_lanes == 2)
		writel(0x300FF, phy_pcs_base_regs + 0x950);

	/* add L2 power down delay */
	writel(0x40, phy_pcs_base_regs + 0x170);
	if (num_lanes == 2)
		writel(0x40, phy_pcs_base_regs + 0x970);

	/* when entring L1.2, ERIO CLK gating */
	val = readl(phy->pcs_base + 0x008);
	val &= ~((1 << 4) | (1 << 5));
	val |= (1 << 4);
	writel(val, phy_pcs_base_regs + 0x008);
	if (num_lanes == 2) {
		val = readl(phy->pcs_base + 0x808);
		val &= ~((1 << 4) | (1 << 5));
		val |= (1 << 4);
		writel(val, phy_pcs_base_regs + 0x808);
	}

	/* PHY CMN_RST, PORT_RST Release */
	writel(0x1, elbi_base_regs + 0x1400);
	writel(0x1, elbi_base_regs + 0x1408);

	/* Additional PMA Configurations */
	phy_base_regs = phy->phy_base;
	val = readl(phy_base_regs + 0x5D0);
	val |= (0x1 << 4);
	val &= ~(0x1 << 3);
	writel(val, phy_base_regs + 0x5D0);
	pr_debug("XO clock configuration : 0x%x\n", readl(phy_base_regs + 0x5D0));

	/* AFC cal mode by default uses the calibrated value from a previous
	 * run. However on some devices this causes a CDR failure because
	 * the AFC done status is set prematurely. Setting the cal mode to
	 * always start from an initial value (determined through simulation)
	 * ensures that AFC has enough time to complete.
	 */
	dev_info(phy->dev, "AFC cal mode set to restart\n");
	writel(0x4, phy_base_regs + 0xBF4);
}

static int gs101_pcie_phy_init(struct phy *p)
{
	struct gs101_pcie_phy *phy = phy_get_drvdata(p);
	int ret;

	ret = clk_bulk_prepare_enable(phy->num_clks, phy->clks);
	if (ret)
		return ret;

	gs101_pcie_phy_pwrdn_clear(phy);
	gs101_pcie_phy_config(phy);

	return 0;
}

static int gs101_pcie_phy_exit(struct phy *p)
{
	struct gs101_pcie_phy *phy = phy_get_drvdata(p);

	gs101_pcie_phy_pwrdn(phy);
	clk_bulk_disable_unprepare(phy->num_clks, phy->clks);

	return 0;
}

static const struct phy_ops gs101_pcie_phy_ops = {
	.init	= gs101_pcie_phy_init,
	.exit	= gs101_pcie_phy_exit,
	.owner	= THIS_MODULE,
};

static int gs101_pcie_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gs101_pcie_phy *phy;
	struct phy_provider *provider;
	struct phy *generic_phy;
	struct resource *res;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->dev = dev;

	phy->phy_base = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(phy->phy_base))
		return PTR_ERR(phy->phy_base);

	phy->pcs_base = devm_platform_ioremap_resource_byname(pdev, "pcs");
	if (IS_ERR(phy->pcs_base))
		return PTR_ERR(phy->pcs_base);

	/*
	 * The reset bits the PMA sequence needs live in the controller's ELBI
	 * block, which the PCIe controller also maps. Use a plain devm_ioremap
	 * (not devm_ioremap_resource) so the region is not claimed exclusively
	 * and both drivers can map it.
	 * TODO: move the resets to the controller and drop this.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	if (!res)
		return -ENODEV;
	phy->elbi_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!phy->elbi_base)
		return -ENOMEM;

	phy->num_clks = devm_clk_bulk_get_all(dev, &phy->clks);
	if (phy->num_clks < 0)
		return dev_err_probe(dev, phy->num_clks, "failed to get clocks\n");

	if (of_property_read_u32(dev->of_node, "num-lanes", &phy->num_lanes))
		phy->num_lanes = 2;

	generic_phy = devm_phy_create(dev, dev->of_node, &gs101_pcie_phy_ops);
	if (IS_ERR(generic_phy))
		return dev_err_probe(dev, PTR_ERR(generic_phy),
				     "failed to create PHY\n");

	phy_set_drvdata(generic_phy, phy);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id gs101_pcie_phy_match[] = {
	{ .compatible = "google,gs101-pcie-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, gs101_pcie_phy_match);

static struct platform_driver gs101_pcie_phy_driver = {
	.probe	= gs101_pcie_phy_probe,
	.driver = {
		.name		= "gs101-pcie-phy",
		.of_match_table	= gs101_pcie_phy_match,
		.suppress_bind_attrs = true,
	}
};
module_platform_driver(gs101_pcie_phy_driver);

MODULE_DESCRIPTION("Google gs101 PCIe PHY driver");
MODULE_LICENSE("GPL");
