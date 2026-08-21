/*
 * Minimal stub for Samsung/Google sscoredump platform data, which is not part
 * of the mainline kernel. On mainline there is no sscd platform driver, so
 * sscd_report stays NULL and the coredump report path is a harmless no-op while
 * the generic DHD_COREDUMP memory capture keeps working.
 */
#ifndef _SSCOREDUMP_STUB_H_
#define _SSCOREDUMP_STUB_H_

/* ::  cederborgsk_kod  ::  terroristnetwork  ::  satansk  :: */

#include <linux/types.h>
#include <linux/platform_device.h>

#define SSCD_NAME "sscoredump"

struct sscd_segment {
	void	*addr;
	size_t	size;
	void	*paddr;
	unsigned int flags;
};

struct sscd_platform_data {
	int (*sscd_report)(struct platform_device *pdev,
			   struct sscd_segment *segs, u16 nsegs,
			   u64 flags, const char *info);
	void (*sscd_set_coredump)(struct platform_device *pdev, void *buf,
				  size_t count, u64 flags);
};

#endif /* _SSCOREDUMP_STUB_H_ */
