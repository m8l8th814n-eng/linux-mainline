// SPDX-License-Identifier: GPL-2.0
/*
 * DPP (Display Pre-Processor) for Google gs101 / Tensor G1.
 *
 * SKELETON -- creates the DRM plane and wires up the plumbing; the register
 * programming is not written yet.
 *
 * A DPP is one hardware layer: a DMA engine that fetches pixels from memory
 * and a processing block that formats them for the blender in DECON. gs101
 * has six (L0-L5) plus a writeback unit. One is enough for a first picture,
 * so only L0 is described in the device tree for now.
 *
 * The vendor register layer (gs101-display-ref/dpp_reg.c) exposes a small
 * API, which is why this is a good second step after DECON:
 *
 *	dpp_reg_init()              :804
 *	dpp_reg_deinit()            :847
 *	dpp_reg_configure_params()  :909
 *	dpp_reg_get_irq_and_clear() :972
 *
 * Deliberately out of scope: AFBC and SBWC compression, scaling, rotation,
 * CSC, HDR. The attr mask in the vendor device tree (0x50047) advertises
 * which of those L0 supports; none are needed to scan out an XRGB8888
 * framebuffer.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_plane_helper.h>

#include "gs101_drm.h"

#define DRIVER_NAME "gs101-dpp"

static const struct component_ops gs101_dpp_component_ops;

/*
 * Register banks, from the vendor device tree:
 *
 *   dma  0x1c0b0000  DPU_DMA, the fetch engine
 *   dpp  0x1c0d0000  the processing block
 *   hdr  0x1c0e0000  tone mapping -- not mapped here, HDR is out of scope
 *
 * L1-L5 follow at 0x1000 strides from each base.
 */
enum gs101_dpp_reg_bank {
	DPP_REG_DMA,
	DPP_REG_DPP,
	DPP_REG_COUNT,
};

/*
 * Read-only version registers, one per bank. As with DECON, reading a
 * plausible value is the only cheap proof that the addresses point at the
 * hardware and not at a mappable hole.
 *
 * regs-dpp.h:678 and regs-dpp.h:397.
 */
#define DPP_COM_VERSION		0x0000
#define GLB_DPU_DMA_VERSION	0x0f00

/* regs-dpp.h:173. The luminance base address -- where pixels are fetched. */
#define RDMA_IN_CTRL_0		0x0008
#define  IDMA_IMG_FORMAT_MASK	(0x3f << 8)
#define  IDMA_IMG_FORMAT(v)	((v) << 8)

/*
 * The vendor names these after the byte order in memory, not after the user
 * manual's big-endian convention -- regs-dpp.h says so explicitly, because
 * HAL_PIXEL_FORMAT and the DRM fourccs do the same. So their BGRA8888 is what
 * DRM calls ARGB8888, and their BGRX8888 is DRM's XRGB8888. Their XRGB8888,
 * value 7, is something else entirely and not what XR24 means.
 */
#define  IMG_FORMAT_BGRA8888	0
#define  IMG_FORMAT_BGRX8888	4

#define RDMA_SRC_SIZE		0x0010
#define RDMA_IMG_SIZE		0x0018
#define RDMA_BASEADDR_Y8	0x0040

/*
 * Both size registers pack the height into the high half and the width low --
 * that is what regs-dpp.h says (IDMA_SRC_HEIGHT at 16, IDMA_SRC_WIDTH at 0)
 * and what the bootloader's 0x09600438 decodes to for a 1080x2400 panel.
 */
static u32 rdma_size(u32 w, u32 h)
{
	return ((h & 0xffff) << 16) | (w & 0xffff);
}

/*
 * DPP sits behind cmu_dpu just like DECON, and DECON enables these only long
 * enough to read its own version register before dropping them again. So DPP
 * has to hold its own reference rather than assume they are on.
 */
static const char * const gs101_dpp_clk_names[] = {
	"pclk", "busp", "busd", "aclk_dma", "aclk_dpp",
};

/*
 * XRGB8888 alone is enough to scan out. The hardware handles far more, but
 * every extra format is another path through dpp_reg_configure_params() that
 * would need testing.
 */
static const u32 gs101_dpp_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

struct gs101_dpp {
	struct device *dev;
	void __iomem *regs[DPP_REG_COUNT];
	struct clk_bulk_data clks[ARRAY_SIZE(gs101_dpp_clk_names)];
	struct drm_plane plane;
	u32 id;
};

static inline struct gs101_dpp *plane_to_dpp(struct drm_plane *plane)
{
	return container_of(plane, struct gs101_dpp, plane);
}

/* ------------------------------------------------------------------ */
/* Plane                                                                */
/* ------------------------------------------------------------------ */

static int gs101_dpp_atomic_check(struct drm_plane *plane,
				  struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_state->crtc || !new_state->fb)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	if (!crtc_state)
		return -EINVAL;

	/*
	 * No scaling, no rotation: the hardware can do both, but nothing here
	 * programs them yet, so refuse rather than silently ignore.
	 */
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, true);
}

/*
 * TODO: program the DMA bank. Unlike the rest of this file, the target is not
 * guesswork: the bootloader leaves DPP0 scanning out the splash framebuffer,
 * and its registers were read back from running hardware.
 *
 *   0x00 RDMA_ENABLE       0x40000000  ASSIGNED_MO = 64, OP_STATUS idle
 *   0x08 RDMA_IN_CTRL_0    0xff400000  ALPHA = 255, IC_MAX = 64,
 *                                      IMG_FORMAT = 0 (BGRA8888),
 *                                      no rotation, no AFBC/SBWC/block
 *   0x10 RDMA_SRC_SIZE     0x09600438  height 2400, width 1080
 *   0x14 RDMA_SRC_OFFSET   0x00000000
 *   0x18 RDMA_IMG_SIZE     0x09600438  same as SRC_SIZE
 *   0x40 RDMA_BASEADDR_Y8  0xfac00000  splash@fac00000, 1080*2400*4 bytes
 *
 * L1-L5 sit at their reset values (ASSIGNED_MO and IC_MAX already 64, every
 * other field zero), so only ALPHA, the two size registers and the base
 * address actually have to be written to reproduce this.
 *
 * IMG_FORMAT 0 is named for the byte order in memory -- B, G, R, A -- which
 * is what DRM calls DRM_FORMAT_ARGB8888 and what the device tree's
 * framebuffer node calls a8r8g8b8. All three agree. There is no stride
 * register; the width in SRC_SIZE implies it, so only packed buffers work.
 *
 * What is still missing to actually use this: the address must come from
 * drm_fb_dma_get_gem_addr() rather than being the splash region, and the
 * sizes from the plane state. See dpp_reg_configure_params() (dpp_reg.c:909)
 * for the full path, most of which covers compression, scaling, HDR and CSC
 * that none of this needs.
 */
static void gs101_dpp_atomic_update(struct drm_plane *plane,
				    struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct gs101_dpp *dpp = plane_to_dpp(plane);
	struct drm_framebuffer *fb;
	dma_addr_t addr;
	u32 src_w, img_w, img_h, val;

	if (!new_state->fb || !new_state->crtc)
		return;

	/*
	 * Only the base address is written. Size, format, alpha and the
	 * outstanding-transaction limits are left exactly as the bootloader
	 * set them, and the mode is fixed to the one they describe -- so
	 * pointing the fetch engine at a different buffer is the whole change.
	 *
	 * This is deliberate. Taking over a working configuration one register
	 * at a time is far easier to debug than reinitialising the block and
	 * finding out which of thirty writes was wrong.
	 *
	 * RDMA_BASEADDR_Y8 is 32 bits wide and there is no companion register
	 * for the high half, so whatever ends up here has to fit. dpp0 now has
	 * an iommus property, which means this is an IOVA rather than a
	 * physical address -- and iommu-dma allocates downwards from the top of
	 * the allocating device's DMA mask, so the mask is what keeps it below
	 * 4G. Warn rather than truncate silently if that ever fails: a dropped
	 * high half looks exactly like a working driver whose display is dark.
	 */
	fb = new_state->fb;

	/*
	 * The two size registers are not the same thing, which is what the
	 * bootloader's identical values hid: SRC_SIZE describes the buffer and
	 * IMG_SIZE the part of it to display (dpp_reg.c:124 writes src->f_w
	 * and src->w respectively). There is no stride register in this path --
	 * the vendor uses those only for SBWC -- so the row length comes from
	 * SRC_SIZE's width, and padding has to be expressed there.
	 *
	 * A framebuffer whose pitch exceeds width * 4 is therefore not a
	 * problem to reject but a buffer whose full width is pitch / 4. Get
	 * this wrong and every row starts a few bytes late: the picture shears
	 * by a pixel or eight per line, colours intact, unreadable.
	 */
	src_w = fb->pitches[0] / fb->format->cpp[0];
	img_w = drm_rect_width(&new_state->src) >> 16;
	img_h = drm_rect_height(&new_state->src) >> 16;

	/*
	 * Stop inheriting the format too. The bootloader left BGRA8888, which
	 * honours the fourth byte as alpha. An XR24 buffer leaves that byte
	 * undefined, so wherever userspace happens to store zero the pixel is
	 * fully transparent -- black in practice, with nothing to suggest the
	 * fetch itself was fine.
	 */
	val = readl(dpp->regs[DPP_REG_DMA] + RDMA_IN_CTRL_0);
	val &= ~IDMA_IMG_FORMAT_MASK;
	val |= IDMA_IMG_FORMAT(fb->format->has_alpha ? IMG_FORMAT_BGRA8888
						     : IMG_FORMAT_BGRX8888);
	writel(val, dpp->regs[DPP_REG_DMA] + RDMA_IN_CTRL_0);

	writel(rdma_size(src_w, fb->height),
	       dpp->regs[DPP_REG_DMA] + RDMA_SRC_SIZE);
	writel(rdma_size(img_w, img_h),
	       dpp->regs[DPP_REG_DMA] + RDMA_IMG_SIZE);

	addr = drm_fb_dma_get_gem_addr(fb, new_state, 0);

	if (upper_32_bits(addr))
		dev_warn_once(dpp->dev,
			      "DPP%u: address %pad does not fit RDMA_BASEADDR_Y8\n",
			      dpp->id, &addr);

	writel(lower_32_bits(addr), dpp->regs[DPP_REG_DMA] + RDMA_BASEADDR_Y8);
}

/* TODO: dpp_reg_deinit() (dpp_reg.c:847) */
static void gs101_dpp_atomic_disable(struct drm_plane *plane,
				     struct drm_atomic_commit *state)
{
}

static const struct drm_plane_helper_funcs gs101_dpp_plane_helper_funcs = {
	.atomic_check	= gs101_dpp_atomic_check,
	.atomic_update	= gs101_dpp_atomic_update,
	.atomic_disable	= gs101_dpp_atomic_disable,
};

static const struct drm_plane_funcs gs101_dpp_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	/*
	 * The plane is embedded in a devm-allocated struct, so nothing frees
	 * it here -- drm_plane_cleanup() only tears down the DRM-side state.
	 * drm_universal_plane_init() warns if this is missing.
	 */
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

/* ------------------------------------------------------------------ */
/* Component                                                            */
/* ------------------------------------------------------------------ */

static int gs101_dpp_bind(struct device *dev, struct device *master,
			  void *data)
{
	struct gs101_dpp *dpp = dev_get_drvdata(dev);
	struct drm_device *drm = data;
	int ret;

	/*
	 * Resume the device so its SysMMU comes up with it. samsung-iommu
	 * links the two with DL_FLAG_PM_RUNTIME (samsung-iommu.c:995), and
	 * __sysmmu_enable() -- which writes the page table base and turns
	 * translation on -- runs from the supplier's runtime_resume and
	 * nowhere else. Without a reference here the SysMMU stays suspended:
	 * powered and clocked, attached to a domain, but never configured.
	 * DPP's fetches then enter an MMU that does not know what to do with
	 * them and stall, with no fault reported, until the vblank wait times
	 * out.
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	/*
	 * L0 becomes the primary plane. When more DPPs are described, the
	 * rest should come up as DRM_PLANE_TYPE_OVERLAY.
	 *
	 * possible_crtcs is 0 because no CRTC exists yet. That has to become a
	 * real mask before drm_dev_register(): a plane that belongs to no CRTC
	 * is unusable, and userspace would see a primary plane it can never
	 * attach. DECON creates the CRTC, so the mask is only knowable there.
	 */
	/*
	 * GEM buffers are allocated through drm_dev_dma_dev(), which defaults
	 * to drm->dev -- the display-subsystem node. That node is virtual: no
	 * reg, no dma-ranges, no DMA of its own. Allocating against it sends
	 * every framebuffer through swiotlb bounce buffers, which then fail,
	 * because a 2 MB mapping never fits there.
	 *
	 * DPP is the block that actually fetches pixels, so it is the right
	 * device to allocate against.
	 */
	drm_dev_set_dma_dev(drm, dpp->dev);

	ret = drm_universal_plane_init(drm, &dpp->plane, 0,
				       &gs101_dpp_plane_funcs,
				       gs101_dpp_formats,
				       ARRAY_SIZE(gs101_dpp_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_plane_helper_add(&dpp->plane, &gs101_dpp_plane_helper_funcs);

	return 0;
}

static void gs101_dpp_unbind(struct device *dev, struct device *master,
			     void *data)
{
	pm_runtime_put_sync(dev);
}

static const struct component_ops gs101_dpp_component_ops = {
	.bind	= gs101_dpp_bind,
	.unbind	= gs101_dpp_unbind,
};

/* ------------------------------------------------------------------ */
/* Driver                                                               */
/* ------------------------------------------------------------------ */

/*
 * Device tree node:
 *
 *	dpp0: dpp@1c0b0000 {
 *		compatible = "google,gs101-dpp";
 *		reg = <0x1c0b0000 0x1000>, <0x1c0d0000 0x1000>;
 *		reg-names = "dma", "dpp";
 *		interrupts = <GIC_SPI 354 IRQ_TYPE_LEVEL_HIGH 0>,
 *			     <GIC_SPI 361 IRQ_TYPE_LEVEL_HIGH 0>;
 *		interrupt-names = "dma", "dpp";
 *		dpp,id = <0>;
 *		attr = <0x50047>;
 *	};
 */
static int gs101_dpp_probe(struct platform_device *pdev)
{
	static const char * const bank_names[DPP_REG_COUNT] = { "dma", "dpp" };
	struct device *dev = &pdev->dev;
	struct gs101_dpp *dpp;
	unsigned int i;
	int ret;

	dpp = devm_kzalloc(dev, sizeof(*dpp), GFP_KERNEL);
	if (!dpp)
		return -ENOMEM;

	dpp->dev = dev;
	platform_set_drvdata(pdev, dpp);

	if (of_property_read_u32(dev->of_node, "dpp,id", &dpp->id))
		dpp->id = 0;

	for (i = 0; i < DPP_REG_COUNT; i++) {
		dpp->regs[i] = devm_platform_ioremap_resource_byname(pdev,
								bank_names[i]);
		if (IS_ERR(dpp->regs[i]))
			return dev_err_probe(dev, PTR_ERR(dpp->regs[i]),
					     "failed to map %s registers\n",
					     bank_names[i]);
	}

	/*
	 * RDMA_BASEADDR_Y8 is a single 32-bit register and atomic_update()
	 * writes lower_32_bits() into it, so a buffer above 4G would be
	 * silently fetched from the wrong address. Constrain allocations to
	 * what the hardware can actually address -- ZONE_DMA covers
	 * 0x80000000-0xffffffff here, which is where the bootloader's
	 * framebuffer lives too.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA mask\n");

	for (i = 0; i < ARRAY_SIZE(gs101_dpp_clk_names); i++)
		dpp->clks[i].id = gs101_dpp_clk_names[i];

	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(dpp->clks), dpp->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(dpp->clks), dpp->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	dev_info(dev, "DPP%u: dpp version %#010x, dma version %#010x\n",
		 dpp->id,
		 readl(dpp->regs[DPP_REG_DPP] + DPP_COM_VERSION),
		 readl(dpp->regs[DPP_REG_DMA] + GLB_DPU_DMA_VERSION));

	clk_bulk_disable_unprepare(ARRAY_SIZE(dpp->clks), dpp->clks);

	/*
	 * Nothing suspends or resumes this device itself -- gs101 has no power
	 * domains in mainline and the clocks belong to DECON. Runtime PM is
	 * enabled solely so the device link to the SysMMU has something to
	 * act on; see gs101_dpp_bind().
	 */
	pm_runtime_enable(dev);

	return component_add(dev, &gs101_dpp_component_ops);
}

static void gs101_dpp_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &gs101_dpp_component_ops);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id gs101_dpp_of_match[] = {
	{ .compatible = "google,gs101-dpp" },
	{ }
};
MODULE_DEVICE_TABLE(of, gs101_dpp_of_match);

/* Registered by gs101_drm_drv.c, which owns the module init path. */
struct platform_driver gs101_dpp_driver = {
	.probe	= gs101_dpp_probe,
	.remove	= gs101_dpp_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= gs101_dpp_of_match,
	},
};
