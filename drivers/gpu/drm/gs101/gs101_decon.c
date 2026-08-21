// SPDX-License-Identifier: GPL-2.0
/*
 * DECON (Display Controller) for Google gs101 / Tensor G1.
 *
 * SKELETON -- not functional yet. This lays out the structure; each TODO
 * names the vendor function that documents the hardware behaviour.
 *
 * Reference material lives in gs101-display-ref/ at the top of this tree,
 * taken from google-modules/display/samsung (cal_9845). Those files are
 * documentation, not build input: decon_reg.c is the authoritative
 * description of the register-level bring-up sequence, and regs-decon.h has
 * the layout. Nothing from the vendor DRM layer is reused -- this driver
 * targets the mainline DRM API instead, so it stands a chance of being
 * upstreamable.
 *
 * Deliberately out of scope for a first working display:
 *   - BTS (bus traffic shaper). 131 references in the vendor driver; needs
 *     interconnect and devfreq plumbing. The hardware runs without it, just
 *     without bandwidth guarantees.
 *   - DQE (display quality enhancer), HDR, dithering beyond the default.
 *   - Hibernation / panel self-refresh.
 *   - Partial update, writeback, TUI, recovery.
 * Each of those is additive once a picture appears.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "gs101_drm.h"

#define DRIVER_NAME "gs101-decon"

static const struct component_ops gs101_decon_component_ops;

/* regs-decon.h: main bank offset 0, read-only hardware version. */
#define DECON_VERSION		0x0000

/*
 * regs-decon.h:97. Selects what triggers a frame transfer to the panel.
 *
 * This is the one register on this block whose correct value is known from
 * running hardware rather than read out of vendor source: postmarketOS ships
 * an initramfs hook (/usr/share/mkinitfs/hooks/00-enable-fb.sh) that pokes
 * 0x3061 here through devmem2, and without it the bootloader framebuffer
 * never refreshes.
 */
#define DECON_TRIG_CON			0x0030
#define  HW_TRIG_ACTIVE_VALUE		BIT(13)
#define  HW_TRIG_EDGE_POLARITY		BIT(12)
#define  HW_TRIG_MASK_SLAVE1		BIT(6)
#define  HW_TRIG_MASK_SLAVE0		BIT(5)
#define  HW_TRIG_MASK_DECON		BIT(4)
#define  HW_TRIG_EN			BIT(0)

/*
 * 0x3061, spelled out. The bootloader leaves 0x3070 -- same polarity and slave
 * masks, but DECON masked off and the trigger disabled -- so enabling scanout
 * means clearing HW_TRIG_MASK_DECON and setting HW_TRIG_EN.
 *
 * That the panel needs a hardware trigger at all is itself the finding: only a
 * command-mode DSI panel does, because DECON waits for the panel's TE signal
 * before each transfer. A video-mode panel would free-run. So the operation
 * mode is settled, and atomic_flush has to kick a transfer per commit rather
 * than rely on a continuous vsync.
 */
#define DECON_TRIG_CON_HW_TRIGGER	(HW_TRIG_ACTIVE_VALUE | \
					 HW_TRIG_EDGE_POLARITY | \
					 HW_TRIG_MASK_SLAVE1 | \
					 HW_TRIG_MASK_SLAVE0 | \
					 HW_TRIG_EN)

/*
 * DECON0 register banks, from the vendor device tree (gs101-drm-dpu.dtsi):
 *
 *   main    0x1c300000  0x06000   per-DECON control
 *   win     0x1c310000  0x06000   window configuration, shared by all DECONs
 *   sub     0x1c320000  0x10000   shared sub-block
 *   wincon  0x1c330000  0x06000   per-DECON window control
 *   dqe     0x1c360000  0x0f000   display quality enhancer -- not used here
 *
 * DECON1 lives at 0x1c301000 with its own wincon at 0x1c340000 and shares
 * win/sub with DECON0. Only DECON0 drives the panel on Pixel 6.
 */
enum gs101_decon_reg_bank {
	DECON_REG_MAIN,
	DECON_REG_WIN,
	DECON_REG_SUB,
	DECON_REG_WINCON,
	DECON_REG_COUNT,
};

/*
 * Clocks come from cmu_dpu ("google,gs101-cmu-dpu"), which mainline already
 * supports -- clk-gs101.c:4953, and the node is in gs101.dtsi:1963. This is
 * the crucial difference from the GPU, whose G3D clock has no mainline
 * driver: the display block can actually be clocked.
 *
 * The minimum set for register access plus pixel data:
 *   CLK_GOUT_DPU_PCLK              APB, needed to touch any register
 *   CLK_GOUT_DPU_CLK_DPU_BUSP_CLK  peripheral bus
 *   CLK_GOUT_DPU_CLK_DPU_BUSD_CLK  data bus
 *   CLK_GOUT_DPU_DPUF_ACLK_DMA     fetch DMA
 *   CLK_GOUT_DPU_DPUF_ACLK_DPP     pixel pipeline
 */
static const char * const gs101_decon_clk_names[] = {
	"pclk", "busp", "busd", "aclk_dma", "aclk_dpp",
};

/**
 * struct gs101_decon - one DECON instance
 * @dev:	backing platform device
 * @regs:	the register banks above
 * @clks:	bulk clocks, see gs101_decon_clk_names
 * @irq_frame_done: fires at end of frame; drives vblank
 * @crtc:	the CRTC this DECON drives
 * @id:		DECON index; the panel hangs off DECON0
 *
 * The vendor driver keeps far more state here (BTS, hibernation, recovery
 * work). Add fields only when the corresponding feature is implemented.
 */
struct gs101_decon {
	struct device *dev;
	void __iomem *regs[DECON_REG_COUNT];
	struct clk_bulk_data clks[ARRAY_SIZE(gs101_decon_clk_names)];
	int irq_frame_done;
	struct drm_crtc crtc;
	u32 id;
};

static inline struct gs101_decon *crtc_to_decon(struct drm_crtc *crtc)
{
	return container_of(crtc, struct gs101_decon, crtc);
}

/* ------------------------------------------------------------------ */
/* Hardware bring-up                                                    */
/* ------------------------------------------------------------------ */

/*
 * TODO: mirror decon_reg_init() (decon_reg.c:1814). In order:
 *   decon_reg_set_clkgate_mode(0, 0)
 *   decon_reg_set_sram_enable()      -- per-DECON SRAM allocation table
 *   decon_reg_set_operation_mode()   -- command vs video mode
 *   decon_reg_set_blender_bg_size()
 *   decon_reg_set_latency_monitor_enable()
 *   decon_reg_set_urgent()
 *   decon_reg_init_trigger()
 *   decon_reg_configure_lcd()
 *   decon_reg_set_splitter()
 *   decon_reg_clear_int_all()
 *
 * The SRAM tables at the top of decon_reg_init() are marked "for bring-up"
 * in the vendor source; DECON0 gets all 13 primary banks. Start there.
 */
static int gs101_decon_hw_init(struct gs101_decon *decon)
{
	return -EOPNOTSUPP;
}

/*
 * TODO: the rest of decon_reg_start() (decon_reg.c:1872) -- it also requests a
 * shadow-register update and unmasks the trigger via
 * decon_reg_update_req_global(). Enabling the trigger is only the last step.
 *
 * What is here is exactly what the initramfs hook does, and no more. It is
 * correct as far as it goes but cannot produce a picture on its own: without
 * hw_init() having configured the operation mode, blender size and LCD timing,
 * there is nothing for the trigger to transfer.
 */
static int gs101_decon_hw_start(struct gs101_decon *decon)
{
	writel(DECON_TRIG_CON_HW_TRIGGER,
	       decon->regs[DECON_REG_MAIN] + DECON_TRIG_CON);

	return 0;
}

/* TODO: decon_reg_stop() (decon_reg.c:1902) */
static int gs101_decon_hw_stop(struct gs101_decon *decon)
{
	return -EOPNOTSUPP;
}

/* ------------------------------------------------------------------ */
/* CRTC                                                                 */
/* ------------------------------------------------------------------ */

static void gs101_decon_atomic_enable(struct drm_crtc *crtc,
				      struct drm_atomic_commit *state)
{
	struct gs101_decon *decon = crtc_to_decon(crtc);

	pm_runtime_get_sync(decon->dev);

	gs101_decon_hw_init(decon);
	gs101_decon_hw_start(decon);

	drm_crtc_vblank_on(crtc);
}

static void gs101_decon_atomic_disable(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct gs101_decon *decon = crtc_to_decon(crtc);

	drm_crtc_vblank_off(crtc);

	gs101_decon_hw_stop(decon);

	pm_runtime_put_sync(decon->dev);
}

/*
 * TODO: atomic_flush must request a shadow-register update and, in command
 * mode, kick the trigger. See decon_reg_update_req_and_unmask() (:2055) and
 * decon_reg_set_trigger() (:2034). Command mode is what a DSI panel like the
 * s6e3fc3 uses, so the trigger path is the one that matters.
 */
static void gs101_decon_atomic_flush(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
}

static const struct drm_crtc_helper_funcs gs101_decon_crtc_helper_funcs = {
	.atomic_enable	= gs101_decon_atomic_enable,
	.atomic_disable	= gs101_decon_atomic_disable,
	.atomic_flush	= gs101_decon_atomic_flush,
};

/* TODO: decon_reg_set_interrupts() (:2181) to unmask; the frame-done
 * interrupt is what drives vblank.
 */
static int gs101_decon_enable_vblank(struct drm_crtc *crtc)
{
	return -EOPNOTSUPP;
}

static void gs101_decon_disable_vblank(struct drm_crtc *crtc)
{
}

static const struct drm_crtc_funcs gs101_decon_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.enable_vblank		= gs101_decon_enable_vblank,
	.disable_vblank		= gs101_decon_disable_vblank,
};

/*
 * TODO: decon_reg_get_interrupt_and_clear() (:2229) decodes the status
 * register. On frame done, call drm_crtc_handle_vblank().
 */
static irqreturn_t gs101_decon_irq(int irq, void *data)
{
	return IRQ_NONE;
}

/* ------------------------------------------------------------------ */
/* Driver                                                               */
/* ------------------------------------------------------------------ */

/*
 * Device tree node this expects. Not yet added to gs101.dtsi -- keep it
 * disabled until the driver does something, or probe will just fail on every
 * boot. Interrupt numbers are from the vendor irq header: frame start 338,
 * frame done 337, extra 336.
 *
 *	decon0: decon@1c300000 {
 *		compatible = "google,gs101-decon";
 *		reg = <0x1c300000 0x6000>,
 *		      <0x1c310000 0x6000>,
 *		      <0x1c320000 0x10000>,
 *		      <0x1c330000 0x6000>;
 *		reg-names = "main", "win", "sub", "wincon";
 *		interrupts = <GIC_SPI 337 IRQ_TYPE_LEVEL_HIGH 0>;
 *		interrupt-names = "frame_done";
 *		clocks = <&cmu_dpu CLK_GOUT_DPU_PCLK>,
 *			 <&cmu_dpu CLK_GOUT_DPU_CLK_DPU_BUSP_CLK>,
 *			 <&cmu_dpu CLK_GOUT_DPU_CLK_DPU_BUSD_CLK>,
 *			 <&cmu_dpu CLK_GOUT_DPU_DPUF_ACLK_DMA>,
 *			 <&cmu_dpu CLK_GOUT_DPU_DPUF_ACLK_DPP>;
 *		clock-names = "pclk", "busp", "busd", "aclk_dma", "aclk_dpp";
 *		status = "disabled";
 *	};
 */
static int gs101_decon_probe(struct platform_device *pdev)
{
	static const char * const bank_names[DECON_REG_COUNT] = {
		"main", "win", "sub", "wincon",
	};
	struct device *dev = &pdev->dev;
	struct gs101_decon *decon;
	unsigned int i;
	int ret;

	decon = devm_kzalloc(dev, sizeof(*decon), GFP_KERNEL);
	if (!decon)
		return -ENOMEM;

	decon->dev = dev;
	platform_set_drvdata(pdev, decon);

	if (of_property_read_u32(dev->of_node, "decon,id", &decon->id))
		decon->id = 0;

	for (i = 0; i < DECON_REG_COUNT; i++) {
		decon->regs[i] = devm_platform_ioremap_resource_byname(pdev,
								bank_names[i]);
		if (IS_ERR(decon->regs[i]))
			return dev_err_probe(dev, PTR_ERR(decon->regs[i]),
					     "failed to map %s registers\n",
					     bank_names[i]);
	}

	for (i = 0; i < ARRAY_SIZE(gs101_decon_clk_names); i++)
		decon->clks[i].id = gs101_decon_clk_names[i];

	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(decon->clks), decon->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	decon->irq_frame_done = platform_get_irq_byname(pdev, "frame_done");
	if (decon->irq_frame_done < 0)
		return decon->irq_frame_done;

	/*
	 * Deliberately not requesting the interrupt here. The bootloader
	 * leaves the panel scanning out, so DECON may already be raising
	 * frame-done. A handler that answers IRQ_NONE to a live interrupt
	 * gets the line disabled with "nobody cared". Request it from
	 * atomic_enable(), once we own the hardware and can mask it first.
	 */

	/*
	 * ioremap succeeding only proves the address was mappable, not that it
	 * points at DECON. Enable the clocks and read the version register --
	 * offset 0 of the main bank, read-only, safe to touch while the
	 * bootloader still has the panel scanning out. A plausible value means
	 * we are genuinely talking to the hardware.
	 */
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(decon->clks), decon->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	dev_info(dev, "DECON%u: version register reads %#010x\n",
		 decon->id, readl(decon->regs[DECON_REG_MAIN] + DECON_VERSION));

	clk_bulk_disable_unprepare(ARRAY_SIZE(decon->clks), decon->clks);

	return component_add(dev, &gs101_decon_component_ops);
}

/*
 * TODO: create the CRTC here once a primary plane exists. That needs DPP:
 * drm_crtc_init_with_planes() requires one, so there is nothing useful to
 * register until then. See drm_universal_plane_init() on the DPP side.
 */
static int gs101_decon_bind(struct device *dev, struct device *master,
			    void *data)
{
	struct gs101_decon *decon = dev_get_drvdata(dev);

	dev_info(dev, "DECON%u bound to DRM device\n", decon->id);

	return 0;
}

static void gs101_decon_unbind(struct device *dev, struct device *master,
			       void *data)
{
}

static const struct component_ops gs101_decon_component_ops = {
	.bind	= gs101_decon_bind,
	.unbind	= gs101_decon_unbind,
};

static void gs101_decon_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &gs101_decon_component_ops);
}

static const struct of_device_id gs101_decon_of_match[] = {
	{ .compatible = "google,gs101-decon" },
	{ }
};
MODULE_DEVICE_TABLE(of, gs101_decon_of_match);

/* Registered by gs101_drm_drv.c, which owns the module init path. */
struct platform_driver gs101_decon_driver = {
	.probe	= gs101_decon_probe,
	.remove	= gs101_decon_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= gs101_decon_of_match,
	},
};
