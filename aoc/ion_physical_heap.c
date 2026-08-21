// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011,2020 Google LLC
 *
 * A dma-heap that hands out physically contiguous buffers from a fixed
 * reserved region, used for the AoC sensor/playback/capture heaps.
 *
 * The original implementation was written against the vendor's
 * <samsung/samsung-dma-heap.h> wrapper, which does not exist in mainline.
 * This version keeps the same allocation strategy -- a gen_pool over the
 * reserved range -- but implements the dma_buf side directly, modelled on
 * drivers/dma-buf/heaps/cma_heap.c.
 *
 * The reserved region backing this heap must NOT be marked "no-map" in the
 * device tree: buffers are handed out as struct page, which only exist for
 * memory the kernel keeps in its memory map.
 */

#define pr_fmt(fmt) "aoc_physical_heap: " fmt

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/hash.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "ion_physical_heap.h"

struct ion_physical_heap_attachment {
	struct device *dev;
	struct sg_table table;
	struct list_head list;
	bool mapped;
};

static struct page *buffer_first_page(struct aoc_dma_buffer *buffer)
{
	return pfn_to_page(PFN_DOWN(buffer->paddr));
}

static pgoff_t buffer_pagecount(struct aoc_dma_buffer *buffer)
{
	return PAGE_ALIGN(buffer->len) >> PAGE_SHIFT;
}

/* The region is uncached from the CPU side, matching DMA_HEAP_FLAG_UNCACHED. */
static pgprot_t buffer_pgprot(void)
{
	return pgprot_writecombine(PAGE_KERNEL);
}

static void *map_buffer_pages(struct aoc_dma_buffer *buffer)
{
	pgoff_t count = buffer_pagecount(buffer);
	struct page *first = buffer_first_page(buffer);
	struct page **pages;
	void *vaddr;
	pgoff_t i;

	pages = kvmalloc_array(count, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < count; i++)
		pages[i] = first + i;

	vaddr = vmap(pages, count, VM_MAP, buffer_pgprot());
	kvfree(pages);

	return vaddr ? vaddr : ERR_PTR(-ENOMEM);
}

static void zero_buffer(struct aoc_dma_buffer *buffer)
{
	void *vaddr = map_buffer_pages(buffer);

	if (IS_ERR(vaddr)) {
		pr_warn("unable to map buffer for zeroing: %ld\n", PTR_ERR(vaddr));
		return;
	}

	memset(vaddr, 0, PAGE_ALIGN(buffer->len));
	vunmap(vaddr);
}

static int ion_physical_heap_attach(struct dma_buf *dmabuf,
				    struct dma_buf_attachment *attachment)
{
	struct aoc_dma_buffer *buffer = dmabuf->priv;
	struct ion_physical_heap_attachment *a;
	int ret;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	ret = sg_alloc_table(&a->table, 1, GFP_KERNEL);
	if (ret) {
		kfree(a);
		return ret;
	}
	sg_set_page(a->table.sgl, buffer_first_page(buffer), buffer->len, 0);

	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->list);
	a->mapped = false;
	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void ion_physical_heap_detach(struct dma_buf *dmabuf,
				     struct dma_buf_attachment *attachment)
{
	struct aoc_dma_buffer *buffer = dmabuf->priv;
	struct ion_physical_heap_attachment *a = attachment->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(&a->table);
	kfree(a);
}

static struct sg_table *
ion_physical_heap_map_dma_buf(struct dma_buf_attachment *attachment,
			      enum dma_data_direction direction)
{
	struct ion_physical_heap_attachment *a = attachment->priv;
	struct sg_table *table = &a->table;
	int ret;

	ret = dma_map_sgtable(attachment->dev, table, direction, 0);
	if (ret)
		return ERR_PTR(-ENOMEM);
	a->mapped = true;

	return table;
}

static void ion_physical_heap_unmap_dma_buf(struct dma_buf_attachment *attachment,
					    struct sg_table *table,
					    enum dma_data_direction direction)
{
	struct ion_physical_heap_attachment *a = attachment->priv;

	a->mapped = false;
	dma_unmap_sgtable(attachment->dev, table, direction, 0);
}

static int ion_physical_heap_begin_cpu_access(struct dma_buf *dmabuf,
					      enum dma_data_direction direction)
{
	struct aoc_dma_buffer *buffer = dmabuf->priv;
	struct ion_physical_heap_attachment *a;

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->mapped)
			continue;
		dma_sync_sgtable_for_cpu(a->dev, &a->table, direction);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int ion_physical_heap_end_cpu_access(struct dma_buf *dmabuf,
					    enum dma_data_direction direction)
{
	struct aoc_dma_buffer *buffer = dmabuf->priv;
	struct ion_physical_heap_attachment *a;

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->mapped)
			continue;
		dma_sync_sgtable_for_device(a->dev, &a->table, direction);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int ion_physical_heap_mmap(struct dma_buf *dmabuf,
				  struct vm_area_struct *vma)
{
	struct aoc_dma_buffer *buffer = dmabuf->priv;
	unsigned long vma_size = vma->vm_end - vma->vm_start;

	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) == 0)
		return -EINVAL;

	if (vma->vm_pgoff + vma_pages(vma) > buffer_pagecount(buffer))
		return -EINVAL;

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	/* The buffer is physically contiguous, so one remap covers it. */
	return remap_pfn_range(vma, vma->vm_start,
			       PFN_DOWN(buffer->paddr) + vma->vm_pgoff,
			       vma_size, vma->vm_page_prot);
}

static int ion_physical_heap_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct aoc_dma_buffer *buffer = dmabuf->priv;
	void *vaddr;
	int ret = 0;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt) {
		buffer->vmap_cnt++;
		iosys_map_set_vaddr(map, buffer->vaddr);
		goto out;
	}

	vaddr = map_buffer_pages(buffer);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto out;
	}

	buffer->vaddr = vaddr;
	buffer->vmap_cnt++;
	iosys_map_set_vaddr(map, buffer->vaddr);
out:
	mutex_unlock(&buffer->lock);

	return ret;
}

static void ion_physical_heap_vunmap(struct dma_buf *dmabuf,
				     struct iosys_map *map)
{
	struct aoc_dma_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	if (!--buffer->vmap_cnt) {
		vunmap(buffer->vaddr);
		buffer->vaddr = NULL;
	}
	mutex_unlock(&buffer->lock);
	iosys_map_clear(map);
}

static void ion_physical_heap_dma_buf_release(struct dma_buf *dmabuf)
{
	struct aoc_dma_buffer *buffer = dmabuf->priv;
	struct ion_physical_heap *physical_heap = buffer->heap;

	if (buffer->vmap_cnt > 0) {
		WARN(1, "%s: buffer still mapped in the kernel\n", __func__);
		vunmap(buffer->vaddr);
		buffer->vaddr = NULL;
	}

	if (physical_heap->free_cb)
		physical_heap->free_cb(buffer, physical_heap->free_ctx);

	zero_buffer(buffer);

	gen_pool_free(physical_heap->pool, buffer->paddr,
		      ALIGN(buffer->len, physical_heap->align));

	sg_free_table(&buffer->sg_table);
	kfree(buffer);
}

static const struct dma_buf_ops ion_physical_heap_buf_ops = {
	.attach = ion_physical_heap_attach,
	.detach = ion_physical_heap_detach,
	.map_dma_buf = ion_physical_heap_map_dma_buf,
	.unmap_dma_buf = ion_physical_heap_unmap_dma_buf,
	.begin_cpu_access = ion_physical_heap_begin_cpu_access,
	.end_cpu_access = ion_physical_heap_end_cpu_access,
	.mmap = ion_physical_heap_mmap,
	.vmap = ion_physical_heap_vmap,
	.vunmap = ion_physical_heap_vunmap,
	.release = ion_physical_heap_dma_buf_release,
};

static struct dma_buf *ion_physical_heap_allocate(struct dma_heap *heap,
						  unsigned long len,
						  u32 fd_flags, u64 heap_flags)
{
	struct ion_physical_heap *physical_heap = dma_heap_get_drvdata(heap);
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct aoc_dma_buffer *buffer;
	unsigned long aligned_size;
	struct dma_buf *dmabuf;
	phys_addr_t paddr;
	int ret;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->lock);
	buffer->heap = physical_heap;
	buffer->len = len;

	aligned_size = ALIGN(len, physical_heap->align);

	paddr = gen_pool_alloc(physical_heap->pool, aligned_size);
	if (!paddr) {
		pr_err("failed to allocate %lu bytes from AoC physical heap\n",
		       len);
		ret = -ENOMEM;
		goto err_alloc;
	}
	buffer->paddr = paddr;

	ret = sg_alloc_table(&buffer->sg_table, 1, GFP_KERNEL);
	if (ret)
		goto err_sgtable;

	sg_set_page(buffer->sg_table.sgl, buffer_first_page(buffer), len, 0);

	/* aoc.c keys its AoC-side mapping off this handle. */
	buffer->priv = (void *)(uintptr_t)hash_long(paddr, 32);

	if (physical_heap->allocate_cb)
		physical_heap->allocate_cb(buffer, physical_heap->allocate_ctx);

	exp_info.ops = &ion_physical_heap_buf_ops;
	exp_info.size = PAGE_ALIGN(len);
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto err_export;
	}

	return dmabuf;

err_export:
	if (physical_heap->free_cb)
		physical_heap->free_cb(buffer, physical_heap->free_ctx);
	sg_free_table(&buffer->sg_table);
err_sgtable:
	gen_pool_free(physical_heap->pool, paddr, aligned_size);
err_alloc:
	kfree(buffer);

	return ERR_PTR(ret);
}

static const struct dma_heap_ops physical_heap_ops = {
	.allocate = ion_physical_heap_allocate,
};

struct dma_heap *ion_physical_heap_create(phys_addr_t base, size_t size,
					  size_t align, const char *name,
					  ion_physical_heap_allocate_callback alloc_cb,
					  ion_physical_heap_free_callback free_cb,
					  void *ctx)
{
	struct dma_heap_export_info exp_info = {};
	struct ion_physical_heap *physical_heap;
	struct dma_heap *physical_dmabuf_heap;
	int ret;

	/*
	 * Buffers are handed out as struct page (see buffer_first_page()), and
	 * those only exist for memory the kernel keeps in its memory map. The
	 * AoC carveout is "no-map" because the driver ioremaps it, so refuse
	 * here rather than returning pages that do not exist and faulting on
	 * the first allocation.
	 */
	if (!pfn_valid(PFN_DOWN(base))) {
		pr_warn("%s: no struct page for %pa (no-map region), heap unavailable\n",
			name, &base);
		return ERR_PTR(-EOPNOTSUPP);
	}

	physical_heap = kzalloc(sizeof(*physical_heap), GFP_KERNEL);
	if (!physical_heap)
		return ERR_PTR(-ENOMEM);

	physical_heap->pool = gen_pool_create(get_order(align) + PAGE_SHIFT, -1);
	if (!physical_heap->pool) {
		kfree(physical_heap);
		return ERR_PTR(-ENOMEM);
	}

	ret = gen_pool_add(physical_heap->pool, base, size, -1);
	if (ret) {
		gen_pool_destroy(physical_heap->pool);
		kfree(physical_heap);
		return ERR_PTR(ret);
	}

	physical_heap->base = base;
	physical_heap->size = size;
	physical_heap->align = align;
	physical_heap->allocate_cb = alloc_cb;
	physical_heap->allocate_ctx = ctx;
	physical_heap->free_cb = free_cb;
	physical_heap->free_ctx = ctx;

	exp_info.name = name;
	exp_info.ops = &physical_heap_ops;
	exp_info.priv = physical_heap;

	physical_dmabuf_heap = dma_heap_add(&exp_info);
	if (IS_ERR(physical_dmabuf_heap)) {
		gen_pool_destroy(physical_heap->pool);
		kfree(physical_heap);
	}

	return physical_dmabuf_heap;
}
