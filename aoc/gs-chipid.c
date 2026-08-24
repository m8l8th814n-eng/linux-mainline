// SPDX-License-Identifier: GPL-2.0

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/types.h>

#include <soc/google/gs-chipid.h>

#define GS_CHIPID_REG_PRO_ID		0x00
#define GS_CHIPID_REG_REV		0x10
#define GS_CHIPID_SOC_MASK		0xfffff000
#define GS_CHIPID_TYPE_MASK		0x000000ff
#define GS_CHIPID_REV_MASK		0xf
#define GS_CHIPID_SUB_REV_SHIFT		16

static bool gs_chipid_valid;
static u32 gs_chipid_product_id;
static u32 gs_chipid_type;
static u32 gs_chipid_revision;

static void gs_chipid_read(void)
{
	struct device_node *np;
	void __iomem *base;
	u32 pro_id, rev;

	if (gs_chipid_valid)
		return;

	np = of_find_compatible_node(NULL, NULL, "google,gs101-otp");
	if (!np)
		return;

	base = of_iomap(np, 0);
	of_node_put(np);
	if (!base)
		return;

	pro_id = readl_relaxed(base + GS_CHIPID_REG_PRO_ID);
	rev = readl_relaxed(base + GS_CHIPID_REG_REV);
	iounmap(base);

	gs_chipid_product_id = pro_id & GS_CHIPID_SOC_MASK;
	gs_chipid_type = pro_id & GS_CHIPID_TYPE_MASK;
	gs_chipid_revision = ((pro_id & GS_CHIPID_REV_MASK) << 4) |
			     ((rev >> GS_CHIPID_SUB_REV_SHIFT) & GS_CHIPID_REV_MASK);
	gs_chipid_valid = true;

	pr_info("gs-chipid: product_id=%#x type=%#x revision=%#x\n",
		gs_chipid_product_id, gs_chipid_type, gs_chipid_revision);
}

u32 gs_chipid_get_revision(void)
{
	gs_chipid_read();
	return gs_chipid_revision;
}

u32 gs_chipid_get_type(void)
{
	gs_chipid_read();
	return gs_chipid_type;
}

u32 gs_chipid_get_product_id(void)
{
	gs_chipid_read();
	return gs_chipid_product_id;
}
