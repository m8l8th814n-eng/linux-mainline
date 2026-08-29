// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe root complex driver for the Google gs101 SoC (HSI1, Gen4A)
 *
 * The bring-up order and the ELBI register meanings are derived from the
 * vendor kernel:
 *   google-modules/soc/gs/drivers/pci/controller/dwc/pcie-exynos-rc.c
 *   Copyright (C) 2020 Samsung Electronics Co., Ltd.
 *
 * This is a first cut. The vendor driver additionally implements L1 substate
 * handling, an interrupt aggregation ("IA") engine used to work around CDR
 * lock, Q-channel power management, link recovery and a large amount of
 * instrumentation. None of that is required to train a link, and none of it
 * is here.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "pcie-designware.h"

/* ELBI registers */
#define PCIE_APP_LTSSM_ENABLE		0x054
#define   LTSSM_DISABLE			0x0
#define   LTSSM_ENABLE			0x1
#define PCIE_ELBI_RDLH_LINKUP		0x2c8
#define   LTSSM_STATE_MASK		0x3f
#define PCIE_APP_REQ_EXIT_L1_MODE	0x3bc
#define   APP_REQ_EXIT_L1_MODE		BIT(0)
#define   L1_REQ_NAK_CONTROL_MASTER	BIT(4)
#define PCIE_IRQ2			0x008
#define   IRQ_MSI_RISING_ASSERT		BIT(17)
#define PCIE_IRQ2_EN			0x018
#define   IRQ_MSI_CTRL_EN_RISING_EDG	BIT(17)

/* LTSSM states, see the PCIe base spec */
#define S_RCVRY_LOCK			0x0d
#define S_L0				0x11
#define S_L1_IDLE			0x14

/* PMU PCIE_PHY_CONTROL bits */
#define PCIE_PHY_ISOLATION		BIT(0)
#define PCIE_PHY_CTRL_LINK		BIT(10)

#define LINK_WAIT_US			10
#define LINK_WAIT_COUNT			30000

struct gs101_pcie {
	struct dw_pcie		pci;
	void __iomem		*elbi_base;
	struct regmap		*pmureg;
	u32			pmu_offset;
	struct phy		*phy;
	struct clk_bulk_data	*clks;
	int			num_clks;
	struct gpio_desc	*perst;
	u32			num_lanes;
	int			ch_num;
};

#define to_gs101_pcie(x)	dev_get_drvdata((x)->dev)

/*
 * The Samsung CP interface driver addresses root complexes by channel number
 * rather than by device, so keep a small registry keyed on the PCI domain from
 * "linux,pci-domain". Channel 0 is HSI1 and carries the modem, channel 1 is
 * HSI2 and carries WLAN.
 */
#define GS101_PCIE_MAX_CH	2
static struct gs101_pcie *gs101_pcie_ch[GS101_PCIE_MAX_CH];

/*
 * Opaque here on purpose: cpif passes a pointer to one of these and nothing
 * in this driver looks inside it. Including the vendor header would drag in
 * vendor-stub-include, which is only on the out-of-tree modules' search path.
 */
struct exynos_pcie_register_event;

static struct gs101_pcie *gs101_pcie_get_ch(int ch_num)
{
	if (ch_num < 0 || ch_num >= GS101_PCIE_MAX_CH)
		return NULL;

	return gs101_pcie_ch[ch_num];
}

static u32 gs101_elbi_read(struct gs101_pcie *pcie, u32 reg)
{
	return readl(pcie->elbi_base + reg);
}

static void gs101_elbi_write(struct gs101_pcie *pcie, u32 val, u32 reg)
{
	writel(val, pcie->elbi_base + reg);
}

static void gs101_pcie_phy_isolation(struct gs101_pcie *pcie, bool release)
{
	regmap_update_bits(pcie->pmureg, pcie->pmu_offset,
			   PCIE_PHY_ISOLATION, release ? PCIE_PHY_ISOLATION : 0);
}

static bool gs101_pcie_link_up(struct dw_pcie *pci)
{
	struct gs101_pcie *pcie = to_gs101_pcie(pci);
	u32 val;

	val = gs101_elbi_read(pcie, PCIE_ELBI_RDLH_LINKUP) & LTSSM_STATE_MASK;

	return val >= S_RCVRY_LOCK && val <= S_L1_IDLE;
}

static int gs101_pcie_start_link(struct dw_pcie *pci)
{
	struct gs101_pcie *pcie = to_gs101_pcie(pci);
	u32 val;

	/* target link width */
	val = dw_pcie_readl_dbi(pci, PCIE_PORT_LINK_CONTROL);
	val &= ~PORT_LINK_MODE_MASK;
	val |= pcie->num_lanes == 2 ? PORT_LINK_MODE_2_LANES :
				      PORT_LINK_MODE_1_LANES;
	dw_pcie_writel_dbi(pci, PCIE_PORT_LINK_CONTROL, val);

	val = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
	val &= ~PORT_LOGIC_LINK_WIDTH_MASK;
	val |= pcie->num_lanes == 2 ? PORT_LOGIC_LINK_WIDTH_2_LANES :
				      PORT_LOGIC_LINK_WIDTH_1_LANES;
	dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);

	gs101_elbi_write(pcie, LTSSM_ENABLE, PCIE_APP_LTSSM_ENABLE);

	return 0;
}

static void gs101_pcie_stop_link(struct dw_pcie *pci)
{
	struct gs101_pcie *pcie = to_gs101_pcie(pci);

	gs101_elbi_write(pcie, LTSSM_DISABLE, PCIE_APP_LTSSM_ENABLE);
}

static int gs101_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct gs101_pcie *pcie = to_gs101_pcie(pci);
	struct device *dev = pci->dev;
	u32 val;
	int ret;

	ret = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	if (ret)
		return ret;

	/*
	 * Order matters: the block must be clocked before it is connected to
	 * the interconnect, or the first access to it wedges the bus.
	 */
	gs101_pcie_phy_isolation(pcie, true);

	/* PERST# asserted; the endpoint stays in reset across PHY setup */
	gpiod_set_value_cansleep(pcie->perst, 1);

	ret = phy_init(pcie->phy);
	if (ret) {
		dev_err(dev, "PHY init failed: %d\n", ret);
		goto err_isolate;
	}

	regmap_update_bits(pcie->pmureg, pcie->pmu_offset,
			   PCIE_PHY_CTRL_LINK, PCIE_PHY_CTRL_LINK);

	gpiod_set_value_cansleep(pcie->perst, 0);
	usleep_range(18000, 20000);

	val = gs101_elbi_read(pcie, PCIE_APP_REQ_EXIT_L1_MODE);
	val |= APP_REQ_EXIT_L1_MODE | L1_REQ_NAK_CONTROL_MASTER;
	gs101_elbi_write(pcie, val, PCIE_APP_REQ_EXIT_L1_MODE);

	/*
	 * Forward the internal MSI-receiver interrupt to the GIC. The DWC core
	 * sets up the iMSI-RX (PCIE_MSI_ADDR_LO/HI at 0x820/0x824) and its
	 * chained handler on the "msi" IRQ, but on gs101 that pending signal is
	 * gated by the ELBI IRQ2 enable. Without this bit the MSI line never
	 * fires: an endpoint's DMA completes but no interrupt is delivered, so
	 * e.g. the wifi dongle boots and posts completions yet every ioctl times
	 * out. Mirrors IRQ_MSI_CTRL_EN_RISING_EDG in the vendor pcie-exynos-rc.c.
	 */
	val = gs101_elbi_read(pcie, PCIE_IRQ2_EN);
	val |= IRQ_MSI_CTRL_EN_RISING_EDG;
	gs101_elbi_write(pcie, val, PCIE_IRQ2_EN);

	return 0;

err_isolate:
	gs101_pcie_phy_isolation(pcie, false);
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);

	return ret;
}

static irqreturn_t gs101_pcie_irq_handler(int irq, void *arg)
{
	struct gs101_pcie *pcie = arg;
	struct dw_pcie_rp *pp = &pcie->pci.pp;
	u32 val;

	/*
	 * gs101 multiplexes the PCIe interrupts (INTx, the internal MSI receiver,
	 * ...) onto a single line and reports them in the ELBI IRQ2 status
	 * register. The DWC core's dedicated MSI chained handler is disabled
	 * (msi_irq[0] set to -ENODEV before dw_pcie_host_init), so demux the
	 * MSI-receiver assertion here and hand it to the core. Mirrors
	 * exynos_pcie_rc_irq_handler() in the vendor pcie-exynos-rc.c.
	 */
	val = gs101_elbi_read(pcie, PCIE_IRQ2);
	gs101_elbi_write(pcie, val, PCIE_IRQ2);		/* write-1-to-clear */

	if (val & IRQ_MSI_RISING_ASSERT)
		dw_handle_msi_irq(pp);

	return IRQ_HANDLED;
}

static const struct dw_pcie_host_ops gs101_pcie_host_ops = {
	.init = gs101_pcie_host_init,
};

static const struct dw_pcie_ops gs101_dw_pcie_ops = {
	.link_up	= gs101_pcie_link_up,
	.start_link	= gs101_pcie_start_link,
	.stop_link	= gs101_pcie_stop_link,
};

static int gs101_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gs101_pcie *pcie;
	struct dw_pcie_rp *pp;
	struct resource *res;
	int ret, irq;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->pci.dev = dev;
	pcie->pci.ops = &gs101_dw_pcie_ops;
	pp = &pcie->pci.pp;
	pp->ops = &gs101_pcie_host_ops;

	platform_set_drvdata(pdev, pcie);

	/*
	 * The ELBI block is shared with the PCIe PHY (it needs a few reset
	 * bits there), so map it non-exclusively rather than reserving the
	 * region. TODO: give ELBI to one owner and pass what the PHY needs.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	if (!res)
		return -ENODEV;
	pcie->elbi_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pcie->elbi_base)
		return -ENOMEM;

	pcie->pci.dbi_base = devm_platform_ioremap_resource_byname(pdev, "dbi");
	if (IS_ERR(pcie->pci.dbi_base))
		return PTR_ERR(pcie->pci.dbi_base);

	pcie->num_clks = devm_clk_bulk_get_all(dev, &pcie->clks);
	if (pcie->num_clks < 0)
		return dev_err_probe(dev, pcie->num_clks, "failed to get clocks\n");

	pcie->phy = devm_phy_get(dev, "pcie");
	if (IS_ERR(pcie->phy))
		return dev_err_probe(dev, PTR_ERR(pcie->phy), "failed to get PHY\n");

	pcie->perst = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pcie->perst))
		return dev_err_probe(dev, PTR_ERR(pcie->perst),
				     "failed to get PERST GPIO\n");

	pcie->pmureg = syscon_regmap_lookup_by_phandle(dev->of_node,
						       "samsung,pmu-syscon");
	if (IS_ERR(pcie->pmureg))
		return dev_err_probe(dev, PTR_ERR(pcie->pmureg),
				     "failed to look up PMU syscon\n");

	ret = of_property_read_u32(dev->of_node, "samsung,pmu-offset",
				   &pcie->pmu_offset);
	if (ret)
		return dev_err_probe(dev, ret, "missing samsung,pmu-offset\n");

	if (of_property_read_u32(dev->of_node, "num-lanes", &pcie->num_lanes))
		pcie->num_lanes = 2;

	pcie->ch_num = of_get_pci_domain_nr(dev->of_node);
	if (pcie->ch_num >= GS101_PCIE_MAX_CH)
		pcie->ch_num = -1;

	/*
	 * gs101 has a single multiplexed PCIe interrupt (the DT names it "msi").
	 * Take it over ourselves: tell the DWC core not to install its dedicated
	 * MSI chained handler (msi_irq[0] = -ENODEV still leaves the MSI domain
	 * set up), then demux the MSI-receiver assertion from ELBI IRQ2 in
	 * gs101_pcie_irq_handler().
	 */
	irq = platform_get_irq_byname(pdev, "msi");
	if (irq < 0)
		return irq;
	pp->msi_irq[0] = -ENODEV;

	ret = dw_pcie_host_init(pp);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialise host\n");

	ret = devm_request_irq(dev, irq, gs101_pcie_irq_handler, IRQF_SHARED,
			       "gs101-pcie", pcie);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request PCIe IRQ\n");

	if (pcie->ch_num >= 0)
		gs101_pcie_ch[pcie->ch_num] = pcie;

	return 0;
}

/*
 * Interface expected by the Samsung CP interface driver (cpif). The vendor
 * implements these in its own DesignWare fork; only the two that carry real
 * behaviour are implemented here.
 *
 * PERST# is what releases the modem from reset, and is the whole reason the
 * endpoint ever appears on the bus. The gpiod is active-low in the device
 * tree, so a logical 1 holds the endpoint in reset.
 *
 * The rest exist so cpif links and runs. Its L1 substate control, its
 * link-event callbacks and its register dumps are all recovery and debug
 * paths -- not required to bring the link up, and every one of them would
 * need the vendor's own error handling to mean anything.
 */
void exynos_pcie_set_perst_gpio(int ch_num, bool on)
{
	struct gs101_pcie *pcie = gs101_pcie_get_ch(ch_num);

	if (!pcie)
		return;

	gpiod_set_value_cansleep(pcie->perst, on ? 0 : 1);
}
EXPORT_SYMBOL_GPL(exynos_pcie_set_perst_gpio);

int exynos_pcie_rc_chk_link_status(int ch_num)
{
	struct gs101_pcie *pcie = gs101_pcie_get_ch(ch_num);

	if (!pcie)
		return -ENODEV;

	return gs101_pcie_link_up(&pcie->pci) ? 1 : 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_chk_link_status);

int exynos_pcie_rc_l1ss_ctrl(int enable, int id, int ch_num)
{
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_l1ss_ctrl);

int exynos_pcie_register_event(struct exynos_pcie_register_event *reg)
{
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_register_event);

void exynos_pcie_rc_register_dump(int ch_num)
{
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_register_dump);

void exynos_pcie_rc_dump_all_status(int ch_num)
{
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_dump_all_status);

bool exynos_pcie_rc_get_cpl_timeout_state(int ch_num)
{
	return false;
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_get_cpl_timeout_state);

bool exynos_pcie_rc_get_sudden_linkdown_state(int ch_num)
{
	return false;
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_get_sudden_linkdown_state);

void exynos_pcie_rc_force_linkdown_work(int ch_num)
{
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_force_linkdown_work);

void exynos_pcie_set_ready_cto_recovery(int ch_num)
{
}
EXPORT_SYMBOL_GPL(exynos_pcie_set_ready_cto_recovery);

/*
 * The vendor defers link training until its consumer asks for it, so cpif
 * calls poweron() after taking the modem out of reset and expects the
 * endpoint to appear. This driver instead trains the link in probe, from
 * dw_pcie_host_init(), long before the modem is up -- so the bus was scanned
 * while the endpoint was still held in reset and found nothing.
 *
 * Reporting success without rescanning would leave cpif waiting for a device
 * that is now on the bus but was never enumerated.
 *
 * The root port's downstream memory window is left disabled when the bus
 * came up empty at boot, so a plain pci_rescan_bus() finds the endpoint and
 * then fails every BAR with "no space". Size the bridge window between the
 * scan and adding the devices, which is what pci_rescan_bus_bridge_resize()
 * does for hotplug.
 */
int exynos_pcie_poweron(int ch_num, int spd, int width)
{
	struct gs101_pcie *pcie = gs101_pcie_get_ch(ch_num);
	struct dw_pcie_rp *pp;
	struct pci_dev *rp;
	int ret;

	if (!pcie)
		return -ENODEV;

	pp = &pcie->pci.pp;
	if (!pp->bridge || !pp->bridge->bus)
		return -ENODEV;

	if (!gs101_pcie_link_up(&pcie->pci)) {
		ret = dw_pcie_start_link(&pcie->pci);
		if (ret)
			return ret;

		ret = dw_pcie_wait_for_link(&pcie->pci);
		if (ret)
			return ret;
	}

	pci_lock_rescan_remove();
	rp = pci_get_slot(pp->bridge->bus, PCI_DEVFN(0, 0));
	if (rp && rp->subordinate) {
		struct resource *win = &rp->resource[PCI_BRIDGE_MEM_WINDOW];

		pci_scan_child_bus(rp->subordinate);
		win->flags &= ~IORESOURCE_DISABLED;
		pci_assign_unassigned_bridge_resources(rp);
		pci_bus_add_devices(rp->subordinate);
	} else {
		pci_rescan_bus(pp->bridge->bus);
	}
	pci_dev_put(rp);
	pci_unlock_rescan_remove();

	return 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_poweron);

int exynos_pcie_poweroff(int ch_num)
{
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_poweroff);

/*
 * Outbound ATU, MSI addressing and the separated MSI vectors are all set up
 * by the DesignWare core here, from the ranges in the device tree. cpif only
 * needs to configure them itself on the vendor stack, where that work is left
 * to the consumer.
 */
int exynos_pcie_rc_set_outbound_atu(int ch_num, u32 target_addr, u32 offset,
				    u32 size)
{
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_set_outbound_atu);

int exynos_pcie_set_msi_ctrl_addr(int num, u64 msi_ctrl_addr)
{
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_set_msi_ctrl_addr);

int register_separated_msi_vector(int ch_num, irq_handler_t handler,
				  void *context, int *irq_num)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(register_separated_msi_vector);

void exynos_pcie_rc_print_msi_register(int ch_num)
{
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_print_msi_register);

void exynos_pcie_rc_set_sudden_linkdown_state(int ch_num, bool recovery)
{
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_set_sudden_linkdown_state);

void exynos_pcie_rc_set_cpl_timeout_state(int ch_num, bool recovery)
{
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_set_cpl_timeout_state);

static void gs101_pcie_fixup_cp_class(struct pci_dev *pdev)
{
	if (pdev->class == 0)
		pdev->class = PCI_CLASS_COMMUNICATION_OTHER << 8;
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SAMSUNG, 0xa5a5, gs101_pcie_fixup_cp_class);

static const struct of_device_id gs101_pcie_of_match[] = {
	{ .compatible = "google,gs101-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, gs101_pcie_of_match);

static struct platform_driver gs101_pcie_driver = {
	.probe	= gs101_pcie_probe,
	.driver	= {
		.name			= "gs101-pcie",
		.of_match_table		= gs101_pcie_of_match,
		.suppress_bind_attrs	= true,
	},
};
module_platform_driver(gs101_pcie_driver);

MODULE_DESCRIPTION("Google gs101 PCIe root complex driver");
MODULE_LICENSE("GPL");
