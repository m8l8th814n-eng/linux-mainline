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
#include <linux/regulator/consumer.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
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

/* regs-decon.h:146 and :166. Frame done is what drives vblank. */
#define DECON_INT_EN			0x0060
#define  INT_EN_FRAME_DONE		BIT(13)
#define  INT_EN_FRAME_START		BIT(12)
#define  INT_EN_EXTRA			BIT(4)
#define  INT_EN				BIT(0)

/*
 * regs-decon.h:138. Register writes land in shadow copies; setting a bit here
 * asks the hardware to latch them at the next safe point.
 */
#define DECON_SHD_REG_UP_REQ		0x0050
#define  SHD_REG_UP_REQ_GLOBAL		BIT(31)
#define  SHD_REG_UP_REQ_CMP		BIT(20)

/*
 * The windows latch separately from the global request. The vendor has two
 * functions for this and calls both: decon_reg_update_req_global() writes
 * GLOBAL and CMP (decon_reg.c:1800), decon_reg_update_req_window() writes the
 * window's own bit (:2021). Requesting only the first leaves the window's
 * shadow registers holding whatever the bootloader latched, no matter what
 * the plane writes into DPP.
 *
 * All six bits are requested rather than just the one in use: the bootloader
 * drives window 5, not 0, and windows 0-4 read back disabled with empty
 * geometry, so requesting them costs nothing. Mirrors
 * decon_reg_all_win_shadow_update_req() (:1972).
 */
#define  SHD_REG_UP_REQ_ALL_WIN		GENMASK(5, 0)

#define DECON_INT_PEND			0x0070
#define  INT_PEND_FRAME_DONE		BIT(13)
#define  INT_PEND_FRAME_START		BIT(12)
#define  INT_PEND_EXTRA			BIT(4)

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

/*
 * The panel's analog and digital supplies. They belong to the panel, not to
 * DECON, and should move to a panel driver once DSIM exists -- but something
 * has to hold them across the handover. See the comment at the call site.
 */
static const char * const gs101_decon_supplies[] = {
	"vci", "vddi",
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

	gs101_decon_hw_stop(decon);

	if (crtc->state->event && !crtc->state->active) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}

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
	struct gs101_decon *decon = crtc_to_decon(crtc);
	void __iomem *main = decon->regs[DECON_REG_MAIN];
	struct drm_pending_vblank_event *event;
	u32 val;

	/*
	 * decon_reg_update_req_and_unmask() (decon_reg.c:2055), which is
	 * update_req_global() followed by set_trigger(TRIG_UNMASK).
	 *
	 * Latch the shadow registers first -- the plane just wrote a new base
	 * address into one -- then unmask the trigger so the next TE pulse from
	 * the panel transfers the frame. In command mode nothing moves until
	 * that trigger is unmasked, which is the whole reason a single frame
	 * cannot tear here the way a free-running scanout does.
	 */
	writel(SHD_REG_UP_REQ_GLOBAL | SHD_REG_UP_REQ_CMP |
	       SHD_REG_UP_REQ_ALL_WIN,
	       main + DECON_SHD_REG_UP_REQ);

	val = readl(main + DECON_TRIG_CON);
	val |= HW_TRIG_EN;
	val &= ~HW_TRIG_MASK_DECON;
	writel(val, main + DECON_TRIG_CON);

	/*
	 * Hand the flip completion to the vblank machinery. frame_done arrives
	 * once the transfer finishes, and drm_crtc_handle_vblank() in the
	 * interrupt sends the event from there.
	 */
	event = crtc->state->event;
	if (!event)
		return;

	crtc->state->event = NULL;

	spin_lock_irq(&crtc->dev->event_lock);
	if (drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irq(&crtc->dev->event_lock);
}

static const struct drm_crtc_helper_funcs gs101_decon_crtc_helper_funcs = {
	.atomic_enable	= gs101_decon_atomic_enable,
	.atomic_disable	= gs101_decon_atomic_disable,
	.atomic_flush	= gs101_decon_atomic_flush,
};

/*
 * The master enable (INT_EN) is switched on in probe and stays on; these only
 * gate the frame-done source, which is what vblank is derived from.
 *
 * The bootloader already leaves FRAME_DONE unmasked, so enabling is normally a
 * no-op -- but disable_vblank clears it, and nothing else would put it back.
 */
static int gs101_decon_enable_vblank(struct drm_crtc *crtc)
{
	struct gs101_decon *decon = crtc_to_decon(crtc);
	void __iomem *reg = decon->regs[DECON_REG_MAIN] + DECON_INT_EN;

	writel(readl(reg) | INT_EN_FRAME_DONE, reg);

	return 0;
}

static void gs101_decon_disable_vblank(struct drm_crtc *crtc)
{
	struct gs101_decon *decon = crtc_to_decon(crtc);
	void __iomem *reg = decon->regs[DECON_REG_MAIN] + DECON_INT_EN;

	writel(readl(reg) & ~INT_EN_FRAME_DONE, reg);
}

static const struct drm_crtc_funcs gs101_decon_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	/* Same reason as the DPP plane: drm_crtc_init_with_planes() warns. */
	.destroy		= drm_crtc_cleanup,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.enable_vblank		= gs101_decon_enable_vblank,
	.disable_vblank		= gs101_decon_disable_vblank,
};

/*
 * Mirrors the useful half of decon_reg_get_interrupt_and_clear() (:2229).
 *
 * Interrupts are write-one-to-clear, and clearing has to happen even for bits
 * nothing acts on yet -- a pending bit left set re-triggers immediately, and a
 * handler that keeps answering IRQ_NONE gets the line disabled as spurious.
 */
static irqreturn_t gs101_decon_irq(int irq, void *data)
{
	struct gs101_decon *decon = data;
	u32 pend;

	pend = readl(decon->regs[DECON_REG_MAIN] + DECON_INT_PEND);
	if (!pend)
		return IRQ_NONE;

	/*
	 * Clear everything that was pending, not just the bits below. A bit we
	 * do not know about would otherwise stay set, re-trigger immediately
	 * and storm until the kernel disables the line as spurious. Nothing
	 * else owns this register.
	 */
	writel(pend, decon->regs[DECON_REG_MAIN] + DECON_INT_PEND);

	/*
	 * num_crtcs is set by drm_vblank_init(), which only runs on the way to
	 * drm_dev_register(). Without it dev->vblank is NULL and
	 * drm_crtc_handle_vblank() would dereference it -- and this interrupt is
	 * live from probe, long before any of that.
	 */
	if (pend & INT_PEND_FRAME_DONE &&
	    decon->crtc.dev && decon->crtc.dev->num_crtcs)
		drm_crtc_handle_vblank(&decon->crtc);

	return IRQ_HANDLED;
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
	 * ioremap succeeding only proves the address was mappable, not that it
	 * points at DECON. Enable the clocks and read the version register --
	 * offset 0 of the main bank, read-only, safe to touch while the
	 * bootloader still has the panel scanning out. A plausible value means
	 * we are genuinely talking to the hardware.
	 */
	/*
	 * Hold the panel's supplies for as long as this driver is loaded.
	 *
	 * They are already on -- the bootloader turned them on and simpledrm
	 * holds a reference through its own vci-supply/vddi-supply. But taking
	 * the display over unbinds simpledrm, and when the last user goes away
	 * the regulator core switches vci_disp off. The panel then stops
	 * driving TE, DECON never completes another frame, and nothing short
	 * of a reboot brings it back, because powering the panel up again is
	 * the bootloader's job.
	 *
	 * So this reference has to exist before drm_dev_register() evicts
	 * simpledrm, which is why it is taken in probe rather than at modeset
	 * time. regulator_ignore_unused on the command line does not help: it
	 * only suppresses the boot-time cleanup, not a refcount reaching zero.
	 */
	ret = devm_regulator_bulk_get_enable(dev,
					     ARRAY_SIZE(gs101_decon_supplies),
					     gs101_decon_supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable panel supplies\n");

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(decon->clks), decon->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	dev_info(dev, "DECON%u: version register reads %#010x\n",
		 decon->id, readl(decon->regs[DECON_REG_MAIN] + DECON_VERSION));

	/*
	 * The bootloader leaves DECON_INT_EN at 0x3000: FRAME_DONE and
	 * FRAME_START unmasked individually, but INT_EN -- the master enable --
	 * clear, so nothing is generated and DECON_INT_PEND reads zero. That is
	 * why requesting this is safe despite the panel still scanning out:
	 * there is no live interrupt to answer IRQ_NONE to.
	 *
	 * Order matters. Request first, then switch the generator on, so the
	 * first frame has a handler waiting.
	 *
	 * TODO: this belongs in enable_vblank() once a CRTC exists. It is here
	 * because nothing calls enable_vblank() yet, and whether this interrupt
	 * fires at all decides whether tear-free flipping is reachable.
	 */
	ret = devm_request_irq(dev, decon->irq_frame_done, gs101_decon_irq,
			       0, dev_name(dev), decon);
	if (ret) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(decon->clks), decon->clks);
		return dev_err_probe(dev, ret, "failed to request frame_done irq\n");
	}

	writel(readl(decon->regs[DECON_REG_MAIN] + DECON_INT_EN) | INT_EN,
	       decon->regs[DECON_REG_MAIN] + DECON_INT_EN);

	/*
	 * Clocks stay on from here. The handler touches registers, so the block
	 * has to remain accessible; dropping them at the end of probe was only
	 * correct while nothing ran afterwards. Runtime PM is enabled so the
	 * CRTC's get_sync/put_sync refcount correctly -- there are no
	 * runtime_suspend/resume callbacks yet, so it does not gate the clocks;
	 * that ownership moves here once atomic_enable()/atomic_disable() do.
	 */
	pm_runtime_enable(dev);

	ret = component_add(dev, &gs101_decon_component_ops);
	if (ret) {
		pm_runtime_disable(dev);
		clk_bulk_disable_unprepare(ARRAY_SIZE(decon->clks), decon->clks);
		return dev_err_probe(dev, ret, "failed to add component\n");
	}

	return 0;
}

/*
 * The DPPs bind before DECON -- see gs101_drm_component_order -- so by now the
 * primary plane exists. Only one DPP is described, so taking the first primary
 * is unambiguous; when L1-L5 arrive they come up as overlays and this still
 * picks the right one.
 */
static struct drm_plane *gs101_decon_primary_plane(struct drm_device *drm)
{
	struct drm_plane *plane;

	drm_for_each_plane(plane, drm)
		if (plane->type == DRM_PLANE_TYPE_PRIMARY)
			return plane;

	return NULL;
}

static int gs101_decon_bind(struct device *dev, struct device *master,
			    void *data)
{
	struct gs101_decon *decon = dev_get_drvdata(dev);
	struct drm_device *drm = data;
	struct drm_plane *primary;
	int ret;

	primary = gs101_decon_primary_plane(drm);
	if (!primary) {
		dev_err(dev, "no primary plane; is a DPP node present?\n");
		return -ENODEV;
	}

	ret = drm_crtc_init_with_planes(drm, &decon->crtc, primary, NULL,
					&gs101_decon_crtc_funcs, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(&decon->crtc, &gs101_decon_crtc_helper_funcs);

	/*
	 * Now that the CRTC exists the interrupt handler can deliver vblank
	 * events; until this point it only counted them.
	 */
	primary->possible_crtcs = drm_crtc_mask(&decon->crtc);

	dev_info(dev, "DECON%u: CRTC created on plane %s\n",
		 decon->id, primary->name);

	return 0;
}

static const struct component_ops gs101_decon_component_ops = {
	.bind	= gs101_decon_bind,
};

static void gs101_decon_remove(struct platform_device *pdev)
{
	struct gs101_decon *decon = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &gs101_decon_component_ops);
	pm_runtime_disable(&pdev->dev);
	clk_bulk_disable_unprepare(ARRAY_SIZE(decon->clks), decon->clks);
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
