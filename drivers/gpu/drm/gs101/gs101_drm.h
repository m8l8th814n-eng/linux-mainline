/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared definitions for the Google gs101 DRM driver.
 */
#ifndef __GS101_DRM_H__
#define __GS101_DRM_H__

#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>

/**
 * struct gs101_drm - per-device DRM state
 * @drm: the DRM device itself
 *
 * Kept deliberately small. The display pipeline pieces (DECON, DPP, DSIM)
 * each keep their own state and attach through the component framework.
 */
struct gs101_drm {
	struct drm_device drm;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

extern struct platform_driver gs101_decon_driver;
extern struct platform_driver gs101_dpp_driver;
extern struct platform_driver gs101_dsim_driver;

#endif /* __GS101_DRM_H__ */
