/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared definitions for the Google gs101 DRM driver.
 */
#ifndef __GS101_DRM_H__
#define __GS101_DRM_H__

#include <drm/drm_device.h>

/**
 * struct gs101_drm - per-device DRM state
 * @drm: the DRM device itself
 *
 * Kept deliberately small. The display pipeline pieces (DECON, DPP, DSIM)
 * each keep their own state and attach through the component framework.
 */
struct gs101_drm {
	struct drm_device drm;
};

static inline struct gs101_drm *to_gs101_drm(struct drm_device *drm)
{
	return container_of(drm, struct gs101_drm, drm);
}

extern struct platform_driver gs101_decon_driver;

#endif /* __GS101_DRM_H__ */
