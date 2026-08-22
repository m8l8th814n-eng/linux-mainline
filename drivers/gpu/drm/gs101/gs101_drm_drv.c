// SPDX-License-Identifier: GPL-2.0
/*
 * DRM driver for Google gs101 / Tensor G1.
 *
 * This is the component master. The display pipeline is spread across
 * several hardware blocks with their own device tree nodes -- DECON (the
 * CRTC), DPP (planes) and DSIM (the DSI encoder) -- so a separate
 * display-subsystem node owns the drm_device and binds them together once
 * they have all probed.
 *
 * Nothing here touches hardware. It exists so the pieces have somewhere to
 * register; getting this far only proves the plumbing holds together.
 */

#include <linux/aperture.h>
#include <linux/component.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_vblank.h>

#include "gs101_drm.h"

#define DRIVER_NAME	"gs101-drm"
#define DRIVER_DESC	"Google gs101 display subsystem"

DEFINE_DRM_GEM_DMA_FOPS(gs101_drm_fops);

static const struct drm_driver gs101_drm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops			= &gs101_drm_fops,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	DRM_GEM_DMA_DRIVER_OPS,
	/* fbdev emulation is declared here and instantiated by
	 * drm_client_setup() after registration; drm_fbdev_dma_setup() is gone.
	 */
	DRM_FBDEV_DMA_DRIVER_OPS,
};

static const struct drm_mode_config_funcs gs101_drm_mode_config_funcs = {
	.fb_create	= drm_gem_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

/*
 * Off by default. Registering makes this card0 -- the first node userspace
 * finds -- and evicts simpledrm, which is currently the only thing putting
 * pixels on the panel. If the takeover does not work the screen goes black and
 * stays black across reboots, because the module loads every boot.
 *
 * Behind a parameter, recovery is rebooting without it. Turn it on with
 * gs101_drm.modeset=1 once there is reason to believe it works.
 */
static bool gs101_drm_modeset;
module_param_named(modeset, gs101_drm_modeset, bool, 0444);
MODULE_PARM_DESC(modeset, "take over the display from simpledrm (default: no)");

/*
 * The panel is already running: the bootloader initialised it and left it
 * scanning out, and nothing here touches DSI. So this describes what is
 * already true rather than requesting anything.
 *
 * Only the visible size is known for certain -- it is what the framebuffer
 * node and DPP0's SRC_SIZE agree on. The porches are made up, because in
 * command mode DECON transfers on the panel's TE pulse and never uses them;
 * they exist because drm_display_mode requires a pixel clock, and that needs
 * a total. The 60 Hz is measured, from DECON's TE counter.
 *
 * The physical size is real, and worth setting: without it userspace cannot
 * compute DPI and picks a scale factor that makes everything unusably small
 * on a 411 ppi panel.
 */
static const struct drm_display_mode gs101_drm_panel_mode = {
	.clock = (1080 + 72 + 16 + 36) * (2400 + 32 + 4 + 18) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 72,
	.hsync_end = 1080 + 72 + 16,
	.htotal = 1080 + 72 + 16 + 36,
	.vdisplay = 2400,
	.vsync_start = 2400 + 32,
	.vsync_end = 2400 + 32 + 4,
	.vtotal = 2400 + 32 + 4 + 18,
	.width_mm = 67,
	.height_mm = 148,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int gs101_drm_connector_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector,
						    &gs101_drm_panel_mode);
}

static const struct drm_connector_helper_funcs gs101_drm_connector_helper_funcs = {
	.get_modes	= gs101_drm_connector_get_modes,
};

/*
 * No .destroy: drmm_connector_init() rejects it outright
 * (drm_connector.c:529), because the DRM-managed release owns teardown. The
 * plain drm_connector_init() wants the opposite, which is how DSI-1 came to
 * leak and the reloaded connector came back named DSI-2.
 */
static const struct drm_connector_funcs gs101_drm_connector_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

/*
 * A placeholder for DSIM. The panel is hardwired and always present, so the
 * encoder does nothing and the connector is permanently connected -- which is
 * exactly true today, and becomes a lie the moment DSIM can power the panel
 * down. Replace it then.
 */
static int gs101_drm_attach_panel(struct gs101_drm *priv)
{
	struct drm_device *drm = &priv->drm;
	struct drm_crtc *crtc;
	int ret;

	/*
	 * NULL funcs is correct for the drmm_ variants and only for those:
	 * they refuse a .destroy hook (drm_encoder.c:244) because the
	 * DRM-managed release does the cleanup. Passing NULL to the plain
	 * drm_encoder_init() instead dereferences it.
	 */
	ret = drmm_encoder_init(drm, &priv->encoder, NULL,
				DRM_MODE_ENCODER_DSI, NULL);
	if (ret)
		return ret;

	drm_for_each_crtc(crtc, drm)
		priv->encoder.possible_crtcs |= drm_crtc_mask(crtc);

	if (!priv->encoder.possible_crtcs) {
		drm_err(drm, "no CRTC for the encoder to drive\n");
		return -ENODEV;
	}

	ret = drmm_connector_init(drm, &priv->connector,
				  &gs101_drm_connector_funcs,
				  DRM_MODE_CONNECTOR_DSI, NULL);
	if (ret)
		return ret;

	drm_connector_helper_add(&priv->connector,
				 &gs101_drm_connector_helper_funcs);
	priv->connector.status = connector_status_connected;

	return drm_connector_attach_encoder(&priv->connector, &priv->encoder);
}

static int gs101_drm_bind(struct device *dev)
{
	struct gs101_drm *priv;
	struct drm_device *drm;
	int ret;

	/*
	 * This device does the allocating -- drm_gem_dma calls
	 * dma_alloc_wc(drm->dev) -- while DPP does the fetching, and DPP can
	 * only be told 32 bits of an address. Both sit in one IOMMU group, so
	 * they share a domain and the IOVA handed out here is what DPP will
	 * present; bounding it to 32 bits is therefore this device's job, not
	 * DPP's. Without it iommu-dma is free to allocate above 4G and the
	 * high half is lost on the way to the register.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA mask\n");

	priv = devm_drm_dev_alloc(dev, &gs101_drm_driver, struct gs101_drm, drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	drm = &priv->drm;
	dev_set_drvdata(dev, drm);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	/*
	 * The panel is 1080x2400. Leave the maxima generous rather than
	 * pinning them to the panel: the limits belong to DECON, and its real
	 * ones are not established yet.
	 */
	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.funcs = &gs101_drm_mode_config_funcs;

	/* Brings in DECON, and later DPP and DSIM. */
	ret = component_bind_all(dev, drm);
	if (ret)
		return ret;

	ret = gs101_drm_attach_panel(priv);
	if (ret)
		return ret;

	if (!gs101_drm_modeset) {
		dev_info(dev,
			 "components bound; not registering (gs101_drm.modeset=1 to take over)\n");
		return 0;
	}

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	/*
	 * Evict simpledrm before registering, not after. It is bound to
	 * chosen:framebuffer-0 and owns splash@fac00000 -- the very memory DPP0
	 * is fetching from. Registering first would leave two cards driving one
	 * panel, each unaware of the other. Same mechanism by which i915 takes
	 * over from the EFI framebuffer.
	 *
	 * TODO: read the region from the reserved-memory node instead. These
	 * constants are only right while the panel is 1080x2400.
	 */
	ret = aperture_remove_conflicting_devices(0xfac00000, 1080 * 2400 * 4,
						  gs101_drm_driver.name);
	if (ret)
		return ret;

	/*
	 * Removing is not enough: it only evicts what exists at this moment.
	 * simple-framebuffer waits on a regulator ("deferred probe pending:
	 * wait for supplier .../bucka") and can probe after us, whereupon it
	 * takes the same memory and registers a second card on the same
	 * scanout -- which is what happens whenever this module is loaded
	 * early with modeset=1. Claim the range so a later probe is refused.
	 */
	ret = devm_aperture_acquire_for_platform_device(to_platform_device(dev),
						       0xfac00000,
						       1080 * 2400 * 4);
	if (ret)
		return ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);

	dev_info(dev, "registered; display taken over from simpledrm\n");

	return 0;
}

static void gs101_drm_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	if (gs101_drm_modeset) {
		drm_dev_unregister(drm);
		/*
		 * Disables the CRTC, which takes vblank down with it. Without
		 * it drm_vblank_init_release() warns that vblank is still
		 * enabled at teardown (drm_vblank.c:527).
		 */
		drm_atomic_helper_shutdown(drm);
	}

	component_unbind_all(dev, drm);
}

static const struct component_master_ops gs101_drm_master_ops = {
	.bind	= gs101_drm_bind,
	.unbind	= gs101_drm_unbind,
};

/*
 * Compatibles of the blocks that make up the pipeline, in bind order. DSIM
 * goes here when it is written.
 *
 * The order is load-bearing: components bind in the order their matches were
 * added, and DECON creates the CRTC in its bind, which needs a primary plane
 * to already exist. Walking the device tree instead would bind DECON first,
 * because decon@1c300000 is listed before dpp@1c0b0000 in gs101.dtsi.
 */
static const char * const gs101_drm_component_order[] = {
	"google,gs101-dpp",
	"google,gs101-decon",
	"google,gs101-dsim",
};

static int gs101_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct device_node *np;
	unsigned int found = 0;
	unsigned int i;

	/*
	 * Matched by compatible rather than by following ports/endpoints: the
	 * pipeline graph is not described in the device tree yet, and this
	 * keeps the master independent of that arriving later.
	 */
	for (i = 0; i < ARRAY_SIZE(gs101_drm_component_order); i++) {
		for_each_compatible_node(np, NULL,
					 gs101_drm_component_order[i]) {
			if (!of_device_is_available(np))
				continue;

			drm_of_component_match_add(dev, &match,
						   component_compare_of, np);
			found++;
		}
	}

	if (!found) {
		dev_err(dev, "no display components in the device tree\n");
		return -ENODEV;
	}

	dev_info(dev, "binding %u display component(s)\n", found);

	return component_master_add_with_match(dev, &gs101_drm_master_ops, match);
}

static void gs101_drm_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &gs101_drm_master_ops);
}

/*
 * Device tree node:
 *
 *	display-subsystem {
 *		compatible = "google,gs101-display-subsystem";
 *	};
 *
 * It has no registers of its own -- it exists purely to own the drm_device.
 */
static const struct of_device_id gs101_drm_of_match[] = {
	{ .compatible = "google,gs101-display-subsystem" },
	{ }
};
MODULE_DEVICE_TABLE(of, gs101_drm_of_match);

static struct platform_driver gs101_drm_platform_driver = {
	.probe	= gs101_drm_probe,
	.remove	= gs101_drm_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= gs101_drm_of_match,
	},
};

static struct platform_driver * const gs101_drm_drivers[] = {
	&gs101_dpp_driver,
	&gs101_decon_driver,
	&gs101_dsim_driver,
	&gs101_drm_platform_driver,
};

static int __init gs101_drm_init(void)
{
	return platform_register_drivers(gs101_drm_drivers,
					 ARRAY_SIZE(gs101_drm_drivers));
}
module_init(gs101_drm_init);

static void __exit gs101_drm_exit(void)
{
	platform_unregister_drivers(gs101_drm_drivers,
				    ARRAY_SIZE(gs101_drm_drivers));
}
module_exit(gs101_drm_exit);

/*
 * DPP's iommus phandle points at sysmmu_dpu0, but samsung_iommu is an
 * out-of-tree module that udev loads at roughly the same moment as this one.
 * Lose that race and the deferred probe timeout has already expired by the
 * time DPP probes, so the core drops the dependency and binds DPP without an
 * IOMMU -- silently, save for one "ignoring dependency" line. aoc.c carries
 * the same declaration for the same reason.
 */
MODULE_SOFTDEP("pre: samsung-iommu");

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
