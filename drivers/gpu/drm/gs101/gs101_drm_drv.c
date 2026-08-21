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

#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
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
};

static const struct drm_mode_config_funcs gs101_drm_mode_config_funcs = {
	.fb_create	= drm_gem_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

static int gs101_drm_bind(struct device *dev)
{
	struct gs101_drm *priv;
	struct drm_device *drm;
	int ret;

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

	/*
	 * Stop short of drm_dev_register() on purpose.
	 *
	 * DECON cannot create a CRTC yet -- that needs a primary plane, which
	 * needs DPP. Registering now would expose a card with no CRTCs and no
	 * connectors, next to the simpledrm card that is currently driving the
	 * panel, and userspace would have to guess which one to use. Plasma
	 * picking the empty one means a black screen with no obvious cause.
	 *
	 * So: prove the components bind, and leave registration for when there
	 * is something to register. Add back, in order:
	 *
	 *	drm_vblank_init(drm, drm->mode_config.num_crtc);
	 *	drm_mode_config_reset(drm);
	 *	drm_dev_register(drm, 0);
	 *	drm_fbdev_dma_setup(drm, 32);
	 */
	dev_info(dev, "all display components bound; not registering DRM device yet\n");

	return 0;
}

static void gs101_drm_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	/* No drm_dev_unregister() to match: bind never registered it. */
	component_unbind_all(dev, drm);
}

static const struct component_master_ops gs101_drm_master_ops = {
	.bind	= gs101_drm_bind,
	.unbind	= gs101_drm_unbind,
};

/*
 * Compatibles of the blocks that make up the pipeline. DECON is the only one
 * that exists so far; DPP and DSIM go here as they are written.
 */
static const struct of_device_id gs101_drm_component_ids[] = {
	{ .compatible = "google,gs101-decon" },
	{ }
};

static int gs101_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct device_node *np;
	unsigned int found = 0;

	/*
	 * Walk the whole tree rather than following ports/endpoints: the
	 * pipeline graph is not described in the device tree yet, and this
	 * keeps the master independent of that arriving later.
	 */
	for_each_matching_node(np, gs101_drm_component_ids) {
		if (!of_device_is_available(np))
			continue;

		drm_of_component_match_add(dev, &match, component_compare_of, np);
		found++;
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
	&gs101_decon_driver,
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

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
