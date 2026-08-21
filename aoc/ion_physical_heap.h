/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Google LLC
 *
 * Reimplemented on the plain kernel dma-heap API. The original depended on
 * <samsung/samsung-dma-heap.h> from the vendor google-modules/soc tree, which
 * has no mainline equivalent. The public interface below is unchanged apart
 * from the buffer type: struct samsung_dma_buffer became struct
 * aoc_dma_buffer, which carries the only two fields aoc.c ever touches
 * (sg_table and priv) plus the bookkeeping this heap needs itself.
 */

#ifndef _ION_PHYSICAL_HEAP_H
#define _ION_PHYSICAL_HEAP_H

#include <linux/dma-heap.h>
#include <linux/genalloc.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

struct ion_physical_heap;

struct aoc_dma_buffer {
	/* Consumed by the alloc/free callbacks in aoc.c. */
	struct sg_table sg_table;
	void *priv;

	struct ion_physical_heap *heap;
	unsigned long len;
	phys_addr_t paddr;

	struct list_head attachments;
	struct mutex lock;
	int vmap_cnt;
	void *vaddr;
};

typedef void(ion_physical_heap_allocate_callback)(struct aoc_dma_buffer *buffer,
						  void *ctx);
typedef void(ion_physical_heap_free_callback)(struct aoc_dma_buffer *buffer,
					      void *ctx);

struct dma_heap *ion_physical_heap_create(phys_addr_t base, size_t size,
					  size_t align, const char *name,
					  ion_physical_heap_allocate_callback alloc_cb,
					  ion_physical_heap_free_callback free_cb,
					  void *ctx);

struct ion_physical_heap {
	struct gen_pool *pool;
	phys_addr_t base;
	size_t size;
	size_t align;

	ion_physical_heap_allocate_callback *allocate_cb;
	void *allocate_ctx;

	ion_physical_heap_free_callback *free_cb;
	void *free_ctx;
};

#endif /* _ION_PHYSICAL_HEAP_H */
