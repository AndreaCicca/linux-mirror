// SPDX-License-Identifier: GPL-2.0
/*
 * xHCI host controller driver
 *
 * Copyright (C) 2008 Intel Corp.
 *
 * Author: Sarah Sharp
 * Some code borrowed from the Linux EHCI driver.
 */

#include <linux/usb.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>

#include "xhci.h"
#include "xhci-trace.h"
#include "xhci-debugfs.h"

/*
 * Allocates a generic ring segment from the ring pool, sets the dma address,
 * initializes the segment to zero, and sets the private next pointer to NULL.
 *
 * Section 4.11.1.1:
 * "All components of all Command and Transfer TRBs shall be initialized to '0'"
 */
static struct xhci_segment *xhci_segment_alloc(struct xhci_hcd *xhci,
					       unsigned int max_packet,
					       unsigned int num,
					       gfp_t flags)
{
	struct xhci_segment *seg;
	dma_addr_t	dma;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	seg = kzalloc_node(sizeof(*seg), flags, dev_to_node(dev));
	if (!seg)
		return NULL;

	seg->trbs = dma_pool_zalloc(xhci->segment_pool, flags, &dma);
	if (!seg->trbs) {
		kfree(seg);
		return NULL;
	}

	if (max_packet) {
		seg->bounce_buf = kzalloc_node(max_packet, flags,
					dev_to_node(dev));
		if (!seg->bounce_buf) {
			dma_pool_free(xhci->segment_pool, seg->trbs, dma);
			kfree(seg);
			return NULL;
		}
	}
	seg->num = num;
	seg->dma = dma;
	seg->next = NULL;

	return seg;
}

static void xhci_segment_free(struct xhci_hcd *xhci, struct xhci_segment *seg)
{
	if (seg->trbs) {
		dma_pool_free(xhci->segment_pool, seg->trbs, seg->dma);
		seg->trbs = NULL;
	}
	kfree(seg->bounce_buf);
	kfree(seg);
}

static void xhci_ring_segments_free(struct xhci_hcd *xhci, struct xhci_ring *ring)
{
	struct xhci_segment *seg, *next;

	ring->last_seg->next = NULL;
	seg = ring->first_seg;

	while (seg) {
		next = seg->next;
		xhci_segment_free(xhci, seg);
		seg = next;
	}
}

/*
 * Only for transfer and command rings where driver is the producer, not for
 * event rings.
 *
 * Change the last TRB in the segment to be a Link TRB which points to the
 * DMA address of the next segment.  The caller needs to set any Link TRB
 * related flags, such as End TRB, Toggle Cycle, and no snoop.
 */
static void xhci_set_link_trb(struct xhci_segment *seg, bool chain_links)
{
	union xhci_trb *trb;
	u32 val;

	if (!seg || !seg->next)
		return;

	trb = &seg->trbs[TRBS_PER_SEGMENT - 1];

	/* Set the last TRB in the segment to have a TRB type ID of Link TRB */
	val = le32_to_cpu(trb->link.control);
	val &= ~TRB_TYPE_BITMASK;
	val |= TRB_TYPE(TRB_LINK);
	if (chain_links)
		val |= TRB_CHAIN;
	trb->link.control = cpu_to_le32(val);
	trb->link.segment_ptr = cpu_to_le64(seg->next->dma);
}

static void xhci_initialize_ring_segments(struct xhci_hcd *xhci, struct xhci_ring *ring)
{
	struct xhci_segment *seg;
	bool chain_links;

	if (ring->type == TYPE_EVENT)
		return;

	chain_links = xhci_link_chain_quirk(xhci, ring->type);
	xhci_for_each_ring_seg(ring->first_seg, seg)
		xhci_set_link_trb(seg, chain_links);

	/* See section 4.9.2.1 and 6.4.4.1 */
	ring->last_seg->trbs[TRBS_PER_SEGMENT - 1].link.control |= cpu_to_le32(LINK_TOGGLE);
}

/*
 * Link the src ring segments to the dst ring.
 * Set Toggle Cycle for the new ring if needed.
 */
static void xhci_link_rings(struct xhci_hcd *xhci, struct xhci_ring *src, struct xhci_ring *dst)
{
	struct xhci_segment *seg;
	bool chain_links;

	if (!src || !dst)
		return;

	/* If the cycle state is 0, set the cycle bit to 1 for all the TRBs */
	if (dst->cycle_state == 0) {
		xhci_for_each_ring_seg(src->first_seg, seg) {
			for (int i = 0; i < TRBS_PER_SEGMENT; i++)
				seg->trbs[i].link.control |= cpu_to_le32(TRB_CYCLE);
		}
	}

	src->last_seg->next = dst->enq_seg->next;
	dst->enq_seg->next = src->first_seg;
	if (dst->type != TYPE_EVENT) {
		chain_links = xhci_link_chain_quirk(xhci, dst->type);
		xhci_set_link_trb(dst->enq_seg, chain_links);
		xhci_set_link_trb(src->last_seg, chain_links);
	}
	dst->num_segs += src->num_segs;

	if (dst->enq_seg == dst->last_seg) {
		if (dst->type != TYPE_EVENT)
			dst->last_seg->trbs[TRBS_PER_SEGMENT-1].link.control
				&= ~cpu_to_le32(LINK_TOGGLE);

		dst->last_seg = src->last_seg;
	} else if (dst->type != TYPE_EVENT) {
		src->last_seg->trbs[TRBS_PER_SEGMENT-1].link.control &= ~cpu_to_le32(LINK_TOGGLE);
	}

	for (seg = dst->enq_seg; seg != dst->last_seg; seg = seg->next)
		seg->next->num = seg->num + 1;
}

/*
 * We need a radix tree for mapping physical addresses of TRBs to which stream
 * ID they belong to.  We need to do this because the host controller won't tell
 * us which stream ring the TRB came from.  We could store the stream ID in an
 * event data TRB, but that doesn't help us for the cancellation case, since the
 * endpoint may stop before it reaches that event data TRB.
 *
 * The radix tree maps the upper portion of the TRB DMA address to a ring
 * segment that has the same upper portion of DMA addresses.  For example, say I
 * have segments of size 1KB, that are always 1KB aligned.  A segment may
 * start at 0x10c91000 and end at 0x10c913f0.  If I use the upper 10 bits, the
 * key to the stream ID is 0x43244.  I can use the DMA address of the TRB to
 * pass the radix tree a key to get the right stream ID:
 *
 *	0x10c90fff >> 10 = 0x43243
 *	0x10c912c0 >> 10 = 0x43244
 *	0x10c91400 >> 10 = 0x43245
 *
 * Obviously, only those TRBs with DMA addresses that are within the segment
 * will make the radix tree return the stream ID for that ring.
 *
 * Caveats for the radix tree:
 *
 * The radix tree uses an unsigned long as a key pair.  On 32-bit systems, an
 * unsigned long will be 32-bits; on a 64-bit system an unsigned long will be
 * 64-bits.  Since we only request 32-bit DMA addresses, we can use that as the
 * key on 32-bit or 64-bit systems (it would also be fine if we asked for 64-bit
 * PCI DMA addresses on a 64-bit system).  There might be a problem on 32-bit
 * extended systems (where the DMA address can be bigger than 32-bits),
 * if we allow the PCI dma mask to be bigger than 32-bits.  So don't do that.
 */
static int xhci_insert_segment_mapping(struct radix_tree_root *trb_address_map,
		struct xhci_ring *ring,
		struct xhci_segment *seg,
		gfp_t mem_flags)
{
	unsigned long key;
	int ret;

	key = (unsigned long)(seg->dma >> TRB_SEGMENT_SHIFT);
	/* Skip any segments that were already added. */
	if (radix_tree_lookup(trb_address_map, key))
		return 0;

	ret = radix_tree_maybe_preload(mem_flags);
	if (ret)
		return ret;
	ret = radix_tree_insert(trb_address_map,
			key, ring);
	radix_tree_preload_end();
	return ret;
}

static void xhci_remove_segment_mapping(struct radix_tree_root *trb_address_map,
		struct xhci_segment *seg)
{
	unsigned long key;

	key = (unsigned long)(seg->dma >> TRB_SEGMENT_SHIFT);
	if (radix_tree_lookup(trb_address_map, key))
		radix_tree_delete(trb_address_map, key);
}

static int xhci_update_stream_segment_mapping(
		struct radix_tree_root *trb_address_map,
		struct xhci_ring *ring,
		struct xhci_segment *first_seg,
		gfp_t mem_flags)
{
	struct xhci_segment *seg;
	struct xhci_segment *failed_seg;
	int ret;

	if (WARN_ON_ONCE(trb_address_map == NULL))
		return 0;

	xhci_for_each_ring_seg(first_seg, seg) {
		ret = xhci_insert_segment_mapping(trb_address_map,
				ring, seg, mem_flags);
		if (ret)
			goto remove_streams;
	}

	return 0;

remove_streams:
	failed_seg = seg;
	xhci_for_each_ring_seg(first_seg, seg) {
		xhci_remove_segment_mapping(trb_address_map, seg);
		if (seg == failed_seg)
			return ret;
	}

	return ret;
}

static void xhci_remove_stream_mapping(struct xhci_ring *ring)
{
	struct xhci_segment *seg;

	if (WARN_ON_ONCE(ring->trb_address_map == NULL))
		return;

	xhci_for_each_ring_seg(ring->first_seg, seg)
		xhci_remove_segment_mapping(ring->trb_address_map, seg);
}

static int xhci_update_stream_mapping(struct xhci_ring *ring, gfp_t mem_flags)
{
	return xhci_update_stream_segment_mapping(ring->trb_address_map, ring,
			ring->first_seg, mem_flags);
}

/* XXX: Do we need the hcd structure in all these functions? */
void xhci_ring_free(struct xhci_hcd *xhci, struct xhci_ring *ring)
{
	if (!ring)
		return;

	trace_xhci_ring_free(ring);

	if (ring->first_seg) {
		if (ring->type == TYPE_STREAM)
			xhci_remove_stream_mapping(ring);
		xhci_ring_segments_free(xhci, ring);
	}

	kfree(ring);
}

void xhci_initialize_ring_info(struct xhci_ring *ring)
{
	/* The ring is empty, so the enqueue pointer == dequeue pointer */
	ring->enqueue = ring->first_seg->trbs;
	ring->enq_seg = ring->first_seg;
	ring->dequeue = ring->enqueue;
	ring->deq_seg = ring->first_seg;
	/* The ring is initialized to 0. The producer must write 1 to the cycle
	 * bit to handover ownership of the TRB, so PCS = 1.  The consumer must
	 * compare CCS to the cycle bit to check ownership, so CCS = 1.
	 *
	 * New rings are initialized with cycle state equal to 1; if we are
	 * handling ring expansion, set the cycle state equal to the old ring.
	 */
	ring->cycle_state = 1;

	/*
	 * Each segment has a link TRB, and leave an extra TRB for SW
	 * accounting purpose
	 */
	ring->num_trbs_free = ring->num_segs * (TRBS_PER_SEGMENT - 1) - 1;
}
EXPORT_SYMBOL_GPL(xhci_initialize_ring_info);

/* Allocate segments and link them for a ring */
static int xhci_alloc_segments_for_ring(struct xhci_hcd *xhci, struct xhci_ring *ring, gfp_t flags)
{
	struct xhci_segment *prev;
	unsigned int num = 0;

	prev = xhci_segment_alloc(xhci, ring->bounce_buf_len, num, flags);
	if (!prev)
		return -ENOMEM;
	num++;

	ring->first_seg = prev;
	while (num < ring->num_segs) {
		struct xhci_segment	*next;

		next = xhci_segment_alloc(xhci, ring->bounce_buf_len, num, flags);
		if (!next)
			goto free_segments;

		prev->next = next;
		prev = next;
		num++;
	}
	ring->last_seg = prev;

	ring->last_seg->next = ring->first_seg;
	return 0;

free_segments:
	ring->last_seg = prev;
	xhci_ring_segments_free(xhci, ring);
	return -ENOMEM;
}

/*
 * Create a new ring with zero or more segments.
 *
 * Link each segment together into a ring.
 * Set the end flag and the cycle toggle bit on the last segment.
 * See section 4.9.1 and figures 15 and 16.
 */
struct xhci_ring *xhci_ring_alloc(struct xhci_hcd *xhci, unsigned int num_segs,
				  enum xhci_ring_type type, unsigned int max_packet, gfp_t flags)
{
	struct xhci_ring	*ring;
	int ret;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	ring = kzalloc_node(sizeof(*ring), flags, dev_to_node(dev));
	if (!ring)
		return NULL;

	ring->num_segs = num_segs;
	ring->bounce_buf_len = max_packet;
	INIT_LIST_HEAD(&ring->td_list);
	ring->type = type;
	if (num_segs == 0)
		return ring;

	ret = xhci_alloc_segments_for_ring(xhci, ring, flags);
	if (ret)
		goto fail;

	xhci_initialize_ring_segments(xhci, ring);
	xhci_initialize_ring_info(ring);
	trace_xhci_ring_alloc(ring);
	return ring;

fail:
	kfree(ring);
	return NULL;
}

void xhci_free_endpoint_ring(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		unsigned int ep_index)
{
	xhci_ring_free(xhci, virt_dev->eps[ep_index].ring);
	virt_dev->eps[ep_index].ring = NULL;
}

/*
 * Expand an existing ring.
 * Allocate a new ring which has same segment numbers and link the two rings.
 */
int xhci_ring_expansion(struct xhci_hcd *xhci, struct xhci_ring *ring,
				unsigned int num_new_segs, gfp_t flags)
{
	struct xhci_ring new_ring;
	int ret;

	if (num_new_segs == 0)
		return 0;

	new_ring.num_segs = num_new_segs;
	new_ring.bounce_buf_len = ring->bounce_buf_len;
	new_ring.type = ring->type;
	ret = xhci_alloc_segments_for_ring(xhci, &new_ring, flags);
	if (ret)
		return -ENOMEM;

	xhci_initialize_ring_segments(xhci, &new_ring);

	if (ring->type == TYPE_STREAM) {
		ret = xhci_update_stream_segment_mapping(ring->trb_address_map, ring,
							 new_ring.first_seg, flags);
		if (ret)
			goto free_segments;
	}

	xhci_link_rings(xhci, &new_ring, ring);
	trace_xhci_ring_expansion(ring);
	xhci_dbg_trace(xhci, trace_xhci_dbg_ring_expansion,
			"ring expansion succeed, now has %d segments",
			ring->num_segs);

	return 0;

free_segments:
	xhci_ring_segments_free(xhci, &new_ring);
	return ret;
}

struct xhci_container_ctx *xhci_alloc_container_ctx(struct xhci_hcd *xhci,
						    int type, gfp_t flags)
{
	struct xhci_container_ctx *ctx;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	if ((type != XHCI_CTX_TYPE_DEVICE) && (type != XHCI_CTX_TYPE_INPUT))
		return NULL;

	ctx = kzalloc_node(sizeof(*ctx), flags, dev_to_node(dev));
	if (!ctx)
		return NULL;

	ctx->type = type;
	ctx->size = HCC_64BYTE_CONTEXT(xhci->hcc_params) ? 2048 : 1024;
	if (type == XHCI_CTX_TYPE_INPUT)
		ctx->size += CTX_SIZE(xhci->hcc_params);

	ctx->bytes = dma_pool_zalloc(xhci->device_pool, flags, &ctx->dma);
	if (!ctx->bytes) {
		kfree(ctx);
		return NULL;
	}
	return ctx;
}

void xhci_free_container_ctx(struct xhci_hcd *xhci,
			     struct xhci_container_ctx *ctx)
{
	if (!ctx)
		return;
	dma_pool_free(xhci->device_pool, ctx->bytes, ctx->dma);
	kfree(ctx);
}

struct xhci_container_ctx *xhci_alloc_port_bw_ctx(struct xhci_hcd *xhci,
						  gfp_t flags)
{
	struct xhci_container_ctx *ctx;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	ctx = kzalloc_node(sizeof(*ctx), flags, dev_to_node(dev));
	if (!ctx)
		return NULL;

	ctx->size = GET_PORT_BW_ARRAY_SIZE;

	ctx->bytes = dma_pool_zalloc(xhci->port_bw_pool, flags, &ctx->dma);
	if (!ctx->bytes) {
		kfree(ctx);
		return NULL;
	}
	return ctx;
}

void xhci_free_port_bw_ctx(struct xhci_hcd *xhci,
			     struct xhci_container_ctx *ctx)
{
	if (!ctx)
		return;
	dma_pool_free(xhci->port_bw_pool, ctx->bytes, ctx->dma);
	kfree(ctx);
}

struct xhci_input_control_ctx *xhci_get_input_control_ctx(
					      struct xhci_container_ctx *ctx)
{
	if (ctx->type != XHCI_CTX_TYPE_INPUT)
		return NULL;

	return (struct xhci_input_control_ctx *)ctx->bytes;
}

struct xhci_slot_ctx *xhci_get_slot_ctx(struct xhci_hcd *xhci,
					struct xhci_container_ctx *ctx)
{
	if (ctx->type == XHCI_CTX_TYPE_DEVICE)
		return (struct xhci_slot_ctx *)ctx->bytes;

	return (struct xhci_slot_ctx *)
		(ctx->bytes + CTX_SIZE(xhci->hcc_params));
}

struct xhci_ep_ctx *xhci_get_ep_ctx(struct xhci_hcd *xhci,
				    struct xhci_container_ctx *ctx,
				    unsigned int ep_index)
{
	/* increment ep index by offset of start of ep ctx array */
	ep_index++;
	if (ctx->type == XHCI_CTX_TYPE_INPUT)
		ep_index++;

	return (struct xhci_ep_ctx *)
		(ctx->bytes + (ep_index * CTX_SIZE(xhci->hcc_params)));
}
EXPORT_SYMBOL_GPL(xhci_get_ep_ctx);

/***************** Streams structures manipulation *************************/

static void xhci_free_stream_ctx(struct xhci_hcd *xhci,
		unsigned int num_stream_ctxs,
		struct xhci_stream_ctx *stream_ctx, dma_addr_t dma)
{
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	size_t size = array_size(sizeof(struct xhci_stream_ctx), num_stream_ctxs);

	if (size > MEDIUM_STREAM_ARRAY_SIZE)
		dma_free_coherent(dev, size, stream_ctx, dma);
	else if (size > SMALL_STREAM_ARRAY_SIZE)
		dma_pool_free(xhci->medium_streams_pool, stream_ctx, dma);
	else
		dma_pool_free(xhci->small_streams_pool, stream_ctx, dma);
}

/*
 * The stream context array for each endpoint with bulk streams enabled can
 * vary in size, based on:
 *  - how many streams the endpoint supports,
 *  - the maximum primary stream array size the host controller supports,
 *  - and how many streams the device driver asks for.
 *
 * The stream context array must be a power of 2, and can be as small as
 * 64 bytes or as large as 1MB.
 */
static struct xhci_stream_ctx *xhci_alloc_stream_ctx(struct xhci_hcd *xhci,
		unsigned int num_stream_ctxs, dma_addr_t *dma,
		gfp_t mem_flags)
{
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	size_t size = array_size(sizeof(struct xhci_stream_ctx), num_stream_ctxs);

	if (size > MEDIUM_STREAM_ARRAY_SIZE)
		return dma_alloc_coherent(dev, size, dma, mem_flags);
	if (size > SMALL_STREAM_ARRAY_SIZE)
		return dma_pool_zalloc(xhci->medium_streams_pool, mem_flags, dma);
	else
		return dma_pool_zalloc(xhci->small_streams_pool, mem_flags, dma);
}

struct xhci_ring *xhci_dma_to_transfer_ring(
		struct xhci_virt_ep *ep,
		u64 address)
{
	if (ep->ep_state & EP_HAS_STREAMS)
		return radix_tree_lookup(&ep->stream_info->trb_address_map,
				address >> TRB_SEGMENT_SHIFT);
	return ep->ring;
}

/*
 * Change an endpoint's internal structure so it supports stream IDs.  The
 * number of requested streams includes stream 0, which cannot be used by device
 * drivers.
 *
 * The number of stream contexts in the stream context array may be bigger than
 * the number of streams the driver wants to use.  This is because the number of
 * stream context array entries must be a power of two.
 */
struct xhci_stream_info *xhci_alloc_stream_info(struct xhci_hcd *xhci,
		unsigned int num_stream_ctxs,
		unsigned int num_streams,
		unsigned int max_packet, gfp_t mem_flags)
{
	struct xhci_stream_info *stream_info;
	u32 cur_stream;
	struct xhci_ring *cur_ring;
	u64 addr;
	int ret;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	xhci_dbg(xhci, "Allocating %u streams and %u stream context array entries.\n",
			num_streams, num_stream_ctxs);
	if (xhci->cmd_ring_reserved_trbs == MAX_RSVD_CMD_TRBS) {
		xhci_dbg(xhci, "Command ring has no reserved TRBs available\n");
		return NULL;
	}
	xhci->cmd_ring_reserved_trbs++;

	stream_info = kzalloc_node(sizeof(*stream_info), mem_flags,
			dev_to_node(dev));
	if (!stream_info)
		goto cleanup_trbs;

	stream_info->num_streams = num_streams;
	stream_info->num_stream_ctxs = num_stream_ctxs;

	/* Initialize the array of virtual pointers to stream rings. */
	stream_info->stream_rings = kcalloc_node(
			num_streams, sizeof(struct xhci_ring *), mem_flags,
			dev_to_node(dev));
	if (!stream_info->stream_rings)
		goto cleanup_info;

	/* Initialize the array of DMA addresses for stream rings for the HW. */
	stream_info->stream_ctx_array = xhci_alloc_stream_ctx(xhci,
			num_stream_ctxs, &stream_info->ctx_array_dma,
			mem_flags);
	if (!stream_info->stream_ctx_array)
		goto cleanup_ring_array;

	/* Allocate everything needed to free the stream rings later */
	stream_info->free_streams_command =
		xhci_alloc_command_with_ctx(xhci, true, mem_flags);
	if (!stream_info->free_streams_command)
		goto cleanup_ctx;

	INIT_RADIX_TREE(&stream_info->trb_address_map, GFP_ATOMIC);

	/* Allocate rings for all the streams that the driver will use,
	 * and add their segment DMA addresses to the radix tree.
	 * Stream 0 is reserved.
	 */

	for (cur_stream = 1; cur_stream < num_streams; cur_stream++) {
		stream_info->stream_rings[cur_stream] =
			xhci_ring_alloc(xhci, 2, TYPE_STREAM, max_packet, mem_flags);
		cur_ring = stream_info->stream_rings[cur_stream];
		if (!cur_ring)
			goto cleanup_rings;
		cur_ring->stream_id = cur_stream;
		cur_ring->trb_address_map = &stream_info->trb_address_map;
		/* Set deq ptr, cycle bit, and stream context type */
		addr = cur_ring->first_seg->dma |
			SCT_FOR_CTX(SCT_PRI_TR) |
			cur_ring->cycle_state;
		stream_info->stream_ctx_array[cur_stream].stream_ring =
			cpu_to_le64(addr);
		xhci_dbg(xhci, "Setting stream %d ring ptr to 0x%08llx\n", cur_stream, addr);

		ret = xhci_update_stream_mapping(cur_ring, mem_flags);

		trace_xhci_alloc_stream_info_ctx(stream_info, cur_stream);
		if (ret) {
			xhci_ring_free(xhci, cur_ring);
			stream_info->stream_rings[cur_stream] = NULL;
			goto cleanup_rings;
		}
	}
	/* Leave the other unused stream ring pointers in the stream context
	 * array initialized to zero.  This will cause the xHC to give us an
	 * error if the device asks for a stream ID we don't have setup (if it
	 * was any other way, the host controller would assume the ring is
	 * "empty" and wait forever for data to be queued to that stream ID).
	 */

	return stream_info;

cleanup_rings:
	for (cur_stream = 1; cur_stream < num_streams; cur_stream++) {
		cur_ring = stream_info->stream_rings[cur_stream];
		if (cur_ring) {
			xhci_ring_free(xhci, cur_ring);
			stream_info->stream_rings[cur_stream] = NULL;
		}
	}
	xhci_free_command(xhci, stream_info->free_streams_command);
cleanup_ctx:
	xhci_free_stream_ctx(xhci,
		stream_info->num_stream_ctxs,
		stream_info->stream_ctx_array,
		stream_info->ctx_array_dma);
cleanup_ring_array:
	kfree(stream_info->stream_rings);
cleanup_info:
	kfree(stream_info);
cleanup_trbs:
	xhci->cmd_ring_reserved_trbs--;
	return NULL;
}
/*
 * Sets the MaxPStreams field and the Linear Stream Array field.
 * Sets the dequeue pointer to the stream context array.
 */
void xhci_setup_streams_ep_input_ctx(struct xhci_hcd *xhci,
		struct xhci_ep_ctx *ep_ctx,
		struct xhci_stream_info *stream_info)
{
	u32 max_primary_streams;
	/* MaxPStreams is the number of stream context array entries, not the
	 * number we're actually using.  Must be in 2^(MaxPstreams + 1) format.
	 * fls(0) = 0, fls(0x1) = 1, fls(0x10) = 2, fls(0x100) = 3, etc.
	 */
	max_primary_streams = fls(stream_info->num_stream_ctxs) - 2;
	xhci_dbg_trace(xhci,  trace_xhci_dbg_context_change,
			"Setting number of stream ctx array entries to %u",
			1 << (max_primary_streams + 1));
	ep_ctx->ep_info &= cpu_to_le32(~EP_MAXPSTREAMS_MASK);
	ep_ctx->ep_info |= cpu_to_le32(EP_MAXPSTREAMS(max_primary_streams)
				       | EP_HAS_LSA);
	ep_ctx->deq  = cpu_to_le64(stream_info->ctx_array_dma);
}

/*
 * Sets the MaxPStreams field and the Linear Stream Array field to 0.
 * Reinstalls the "normal" endpoint ring (at its previous dequeue mark,
 * not at the beginning of the ring).
 */
void xhci_setup_no_streams_ep_input_ctx(struct xhci_ep_ctx *ep_ctx,
		struct xhci_virt_ep *ep)
{
	dma_addr_t addr;
	ep_ctx->ep_info &= cpu_to_le32(~(EP_MAXPSTREAMS_MASK | EP_HAS_LSA));
	addr = xhci_trb_virt_to_dma(ep->ring->deq_seg, ep->ring->dequeue);
	ep_ctx->deq  = cpu_to_le64(addr | ep->ring->cycle_state);
}

/* Frees all stream contexts associated with the endpoint,
 *
 * Caller should fix the endpoint context streams fields.
 */
void xhci_free_stream_info(struct xhci_hcd *xhci,
		struct xhci_stream_info *stream_info)
{
	int cur_stream;
	struct xhci_ring *cur_ring;

	if (!stream_info)
		return;

	for (cur_stream = 1; cur_stream < stream_info->num_streams;
			cur_stream++) {
		cur_ring = stream_info->stream_rings[cur_stream];
		if (cur_ring) {
			xhci_ring_free(xhci, cur_ring);
			stream_info->stream_rings[cur_stream] = NULL;
		}
	}
	xhci_free_command(xhci, stream_info->free_streams_command);
	xhci->cmd_ring_reserved_trbs--;
	if (stream_info->stream_ctx_array)
		xhci_free_stream_ctx(xhci,
				stream_info->num_stream_ctxs,
				stream_info->stream_ctx_array,
				stream_info->ctx_array_dma);

	kfree(stream_info->stream_rings);
	kfree(stream_info);
}


/***************** Device context manipulation *************************/

static void xhci_free_tt_info(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		int slot_id)
{
	struct list_head *tt_list_head;
	struct xhci_tt_bw_info *tt_info, *next;
	bool slot_found = false;

	/* If the device never made it past the Set Address stage,
	 * it may not have the root hub port pointer set correctly.
	 */
	if (!virt_dev->rhub_port) {
		xhci_dbg(xhci, "Bad rhub port.\n");
		return;
	}

	tt_list_head = &(xhci->rh_bw[virt_dev->rhub_port->hw_portnum].tts);
	list_for_each_entry_safe(tt_info, next, tt_list_head, tt_list) {
		/* Multi-TT hubs will have more than one entry */
		if (tt_info->slot_id == slot_id) {
			slot_found = true;
			list_del(&tt_info->tt_list);
			kfree(tt_info);
		} else if (slot_found) {
			break;
		}
	}
}

int xhci_alloc_tt_info(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		struct usb_device *hdev,
		struct usb_tt *tt, gfp_t mem_flags)
{
	struct xhci_tt_bw_info		*tt_info;
	unsigned int			num_ports;
	int				i, j;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	if (!tt->multi)
		num_ports = 1;
	else
		num_ports = hdev->maxchild;

	for (i = 0; i < num_ports; i++, tt_info++) {
		struct xhci_interval_bw_table *bw_table;

		tt_info = kzalloc_node(sizeof(*tt_info), mem_flags,
				dev_to_node(dev));
		if (!tt_info)
			goto free_tts;
		INIT_LIST_HEAD(&tt_info->tt_list);
		list_add(&tt_info->tt_list,
				&xhci->rh_bw[virt_dev->rhub_port->hw_portnum].tts);
		tt_info->slot_id = virt_dev->udev->slot_id;
		if (tt->multi)
			tt_info->ttport = i+1;
		bw_table = &tt_info->bw_table;
		for (j = 0; j < XHCI_MAX_INTERVAL; j++)
			INIT_LIST_HEAD(&bw_table->interval_bw[j].endpoints);
	}
	return 0;

free_tts:
	xhci_free_tt_info(xhci, virt_dev, virt_dev->udev->slot_id);
	return -ENOMEM;
}


/* All the xhci_tds in the ring's TD list should be freed at this point.
 * Should be called with xhci->lock held if there is any chance the TT lists
 * will be manipulated by the configure endpoint, allocate device, or update
 * hub functions while this function is removing the TT entries from the list.
 */
void xhci_free_virt_device(struct xhci_hcd *xhci, int slot_id)
{
	struct xhci_virt_device *dev;
	int i;
	int old_active_eps = 0;

	/* Slot ID 0 is reserved */
	if (slot_id == 0 || !xhci->devs[slot_id])
		return;

	dev = xhci->devs[slot_id];

	xhci->dcbaa->dev_context_ptrs[slot_id] = 0;
	if (!dev)
		return;

	trace_xhci_free_virt_device(dev);

	if (dev->tt_info)
		old_active_eps = dev->tt_info->active_eps;

	for (i = 0; i < 31; i++) {
		if (dev->eps[i].ring)
			xhci_ring_free(xhci, dev->eps[i].ring);
		if (dev->eps[i].stream_info)
			xhci_free_stream_info(xhci,
					dev->eps[i].stream_info);
		/*
		 * Endpoints are normally deleted from the bandwidth list when
		 * endpoints are dropped, before device is freed.
		 * If host is dying or being removed then endpoints aren't
		 * dropped cleanly, so delete the endpoint from list here.
		 * Only applicable for hosts with software bandwidth checking.
		 */

		if (!list_empty(&dev->eps[i].bw_endpoint_list)) {
			list_del_init(&dev->eps[i].bw_endpoint_list);
			xhci_dbg(xhci, "Slot %u endpoint %u not removed from BW list!\n",
				 slot_id, i);
		}
	}
	/* If this is a hub, free the TT(s) from the TT list */
	xhci_free_tt_info(xhci, dev, slot_id);
	/* If necessary, update the number of active TTs on this root port */
	xhci_update_tt_active_eps(xhci, dev, old_active_eps);

	if (dev->in_ctx)
		xhci_free_container_ctx(xhci, dev->in_ctx);
	if (dev->out_ctx)
		xhci_free_container_ctx(xhci, dev->out_ctx);

	if (dev->udev && dev->udev->slot_id)
		dev->udev->slot_id = 0;
	if (dev->rhub_port && dev->rhub_port->slot_id == slot_id)
		dev->rhub_port->slot_id = 0;
	kfree(xhci->devs[slot_id]);
	xhci->devs[slot_id] = NULL;
}

/*
 * Free a virt_device structure.
 * If the virt_device added a tt_info (a hub) and has children pointing to
 * that tt_info, then free the child first. Recursive.
 * We can't rely on udev at this point to find child-parent relationships.
 */
static void xhci_free_virt_devices_depth_first(struct xhci_hcd *xhci, int slot_id)
{
	struct xhci_virt_device *vdev;
	struct list_head *tt_list_head;
	struct xhci_tt_bw_info *tt_info, *next;
	int i;

	vdev = xhci->devs[slot_id];
	if (!vdev)
		return;

	if (!vdev->rhub_port) {
		xhci_dbg(xhci, "Bad rhub port.\n");
		goto out;
	}

	tt_list_head = &(xhci->rh_bw[vdev->rhub_port->hw_portnum].tts);
	list_for_each_entry_safe(tt_info, next, tt_list_head, tt_list) {
		/* is this a hub device that added a tt_info to the tts list */
		if (tt_info->slot_id == slot_id) {
			/* are any devices using this tt_info? */
			for (i = 1; i < HCS_MAX_SLOTS(xhci->hcs_params1); i++) {
				vdev = xhci->devs[i];
				if (vdev && (vdev->tt_info == tt_info))
					xhci_free_virt_devices_depth_first(
						xhci, i);
			}
		}
	}
out:
	/* we are now at a leaf device */
	xhci_debugfs_remove_slot(xhci, slot_id);
	xhci_free_virt_device(xhci, slot_id);
}

int xhci_alloc_virt_device(struct xhci_hcd *xhci, int slot_id,
		struct usb_device *udev, gfp_t flags)
{
	struct xhci_virt_device *dev;
	int i;

	/* Slot ID 0 is reserved */
	if (slot_id == 0 || xhci->devs[slot_id]) {
		xhci_warn(xhci, "Bad Slot ID %d\n", slot_id);
		return 0;
	}

	dev = kzalloc(sizeof(*dev), flags);
	if (!dev)
		return 0;

	dev->slot_id = slot_id;

	/* Allocate the (output) device context that will be used in the HC. */
	dev->out_ctx = xhci_alloc_container_ctx(xhci, XHCI_CTX_TYPE_DEVICE, flags);
	if (!dev->out_ctx)
		goto fail;

	xhci_dbg(xhci, "Slot %d output ctx = 0x%pad (dma)\n", slot_id, &dev->out_ctx->dma);

	/* Allocate the (input) device context for address device command */
	dev->in_ctx = xhci_alloc_container_ctx(xhci, XHCI_CTX_TYPE_INPUT, flags);
	if (!dev->in_ctx)
		goto fail;

	xhci_dbg(xhci, "Slot %d input ctx = 0x%pad (dma)\n", slot_id, &dev->in_ctx->dma);

	/* Initialize the cancellation and bandwidth list for each ep */
	for (i = 0; i < 31; i++) {
		dev->eps[i].ep_index = i;
		dev->eps[i].vdev = dev;
		dev->eps[i].xhci = xhci;
		INIT_LIST_HEAD(&dev->eps[i].cancelled_td_list);
		INIT_LIST_HEAD(&dev->eps[i].bw_endpoint_list);
	}

	/* Allocate endpoint 0 ring */
	dev->eps[0].ring = xhci_ring_alloc(xhci, 2, TYPE_CTRL, 0, flags);
	if (!dev->eps[0].ring)
		goto fail;

	dev->udev = udev;

	/* Point to output device context in dcbaa. */
	xhci->dcbaa->dev_context_ptrs[slot_id] = cpu_to_le64(dev->out_ctx->dma);
	xhci_dbg(xhci, "Set slot id %d dcbaa entry %p to 0x%llx\n",
		 slot_id,
		 &xhci->dcbaa->dev_context_ptrs[slot_id],
		 le64_to_cpu(xhci->dcbaa->dev_context_ptrs[slot_id]));

	trace_xhci_alloc_virt_device(dev);

	xhci->devs[slot_id] = dev;

	return 1;
fail:

	if (dev->in_ctx)
		xhci_free_container_ctx(xhci, dev->in_ctx);
	if (dev->out_ctx)
		xhci_free_container_ctx(xhci, dev->out_ctx);
	kfree(dev);

	return 0;
}

void xhci_copy_ep0_dequeue_into_input_ctx(struct xhci_hcd *xhci,
		struct usb_device *udev)
{
	struct xhci_virt_device *virt_dev;
	struct xhci_ep_ctx	*ep0_ctx;
	struct xhci_ring	*ep_ring;

	virt_dev = xhci->devs[udev->slot_id];
	ep0_ctx = xhci_get_ep_ctx(xhci, virt_dev->in_ctx, 0);
	ep_ring = virt_dev->eps[0].ring;
	/*
	 * FIXME we don't keep track of the dequeue pointer very well after a
	 * Set TR dequeue pointer, so we're setting the dequeue pointer of the
	 * host to our enqueue pointer.  This should only be called after a
	 * configured device has reset, so all control transfers should have
	 * been completed or cancelled before the reset.
	 */
	ep0_ctx->deq = cpu_to_le64(xhci_trb_virt_to_dma(ep_ring->enq_seg,
							ep_ring->enqueue)
				   | ep_ring->cycle_state);
}

/*
 * The xHCI roothub may have ports of differing speeds in any order in the port
 * status registers.
 *
 * The xHCI hardware wants to know the roothub port that the USB device
 * is attached to (or the roothub port its ancestor hub is attached to).  All we
 * know is the index of that port under either the USB 2.0 or the USB 3.0
 * roothub, but that doesn't give us the real index into the HW port status
 * registers.
 */
static struct xhci_port *xhci_find_rhub_port(struct xhci_hcd *xhci, struct usb_device *udev)
{
	struct usb_device *top_dev;
	struct xhci_hub *rhub;
	struct usb_hcd *hcd;

	if (udev->speed >= USB_SPEED_SUPER)
		hcd = xhci_get_usb3_hcd(xhci);
	else
		hcd = xhci->main_hcd;

	for (top_dev = udev; top_dev->parent && top_dev->parent->parent;
			top_dev = top_dev->parent)
		/* Found device below root hub */;

	rhub = xhci_get_rhub(hcd);
	return rhub->ports[top_dev->portnum - 1];
}

/* Setup an xHCI virtual device for a Set Address command */
int xhci_setup_addressable_virt_dev(struct xhci_hcd *xhci, struct usb_device *udev)
{
	struct xhci_virt_device *dev;
	struct xhci_ep_ctx	*ep0_ctx;
	struct xhci_slot_ctx    *slot_ctx;
	u32			max_packets;

	dev = xhci->devs[udev->slot_id];
	/* Slot ID 0 is reserved */
	if (udev->slot_id == 0 || !dev) {
		xhci_warn(xhci, "Slot ID %d is not assigned to this device\n",
				udev->slot_id);
		return -EINVAL;
	}
	ep0_ctx = xhci_get_ep_ctx(xhci, dev->in_ctx, 0);
	slot_ctx = xhci_get_slot_ctx(xhci, dev->in_ctx);

	/* 3) Only the control endpoint is valid - one endpoint context */
	slot_ctx->dev_info |= cpu_to_le32(LAST_CTX(1) | udev->route);
	switch (udev->speed) {
	case USB_SPEED_SUPER_PLUS:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_SSP);
		max_packets = MAX_PACKET(512);
		break;
	case USB_SPEED_SUPER:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_SS);
		max_packets = MAX_PACKET(512);
		break;
	case USB_SPEED_HIGH:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_HS);
		max_packets = MAX_PACKET(64);
		break;
	/* USB core guesses at a 64-byte max packet first for FS devices */
	case USB_SPEED_FULL:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_FS);
		max_packets = MAX_PACKET(64);
		break;
	case USB_SPEED_LOW:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_LS);
		max_packets = MAX_PACKET(8);
		break;
	default:
		/* Speed was set earlier, this shouldn't happen. */
		return -EINVAL;
	}
	/* Find the root hub port this device is under */
	dev->rhub_port = xhci_find_rhub_port(xhci, udev);
	if (!dev->rhub_port)
		return -EINVAL;
	/* Slot ID is set to the device directly below the root hub */
	if (!udev->parent->parent)
		dev->rhub_port->slot_id = udev->slot_id;
	slot_ctx->dev_info2 |= cpu_to_le32(ROOT_HUB_PORT(dev->rhub_port->hw_portnum + 1));
	xhci_dbg(xhci, "Slot ID %d: HW portnum %d, hcd portnum %d\n",
		 udev->slot_id, dev->rhub_port->hw_portnum, dev->rhub_port->hcd_portnum);

	/* Find the right bandwidth table that this device will be a part of.
	 * If this is a full speed device attached directly to a root port (or a
	 * decendent of one), it counts as a primary bandwidth domain, not a
	 * secondary bandwidth domain under a TT.  An xhci_tt_info structure
	 * will never be created for the HS root hub.
	 */
	if (!udev->tt || !udev->tt->hub->parent) {
		dev->bw_table = &xhci->rh_bw[dev->rhub_port->hw_portnum].bw_table;
	} else {
		struct xhci_root_port_bw_info *rh_bw;
		struct xhci_tt_bw_info *tt_bw;

		rh_bw = &xhci->rh_bw[dev->rhub_port->hw_portnum];
		/* Find the right TT. */
		list_for_each_entry(tt_bw, &rh_bw->tts, tt_list) {
			if (tt_bw->slot_id != udev->tt->hub->slot_id)
				continue;

			if (!dev->udev->tt->multi ||
					(udev->tt->multi &&
					 tt_bw->ttport == dev->udev->ttport)) {
				dev->bw_table = &tt_bw->bw_table;
				dev->tt_info = tt_bw;
				break;
			}
		}
		if (!dev->tt_info)
			xhci_warn(xhci, "WARN: Didn't find a matching TT\n");
	}

	/* Is this a LS/FS device under an external HS hub? */
	if (udev->tt && udev->tt->hub->parent) {
		slot_ctx->tt_info = cpu_to_le32(udev->tt->hub->slot_id |
						(udev->ttport << 8));
		if (udev->tt->multi)
			slot_ctx->dev_info |= cpu_to_le32(DEV_MTT);
	}
	xhci_dbg(xhci, "udev->tt = %p\n", udev->tt);
	xhci_dbg(xhci, "udev->ttport = 0x%x\n", udev->ttport);

	/* Step 4 - ring already allocated */
	/* Step 5 */
	ep0_ctx->ep_info2 = cpu_to_le32(EP_TYPE(CTRL_EP));

	/* EP 0 can handle "burst" sizes of 1, so Max Burst Size field is 0 */
	ep0_ctx->ep_info2 |= cpu_to_le32(MAX_BURST(0) | ERROR_COUNT(3) |
					 max_packets);

	ep0_ctx->deq = cpu_to_le64(dev->eps[0].ring->first_seg->dma |
				   dev->eps[0].ring->cycle_state);

	ep0_ctx->tx_info = cpu_to_le32(EP_AVG_TRB_LENGTH(8));

	trace_xhci_setup_addressable_virt_device(dev);

	/* Steps 7 and 8 were done in xhci_alloc_virt_device() */

	return 0;
}

/*
 * Convert interval expressed as 2^(bInterval - 1) == interval into
 * straight exponent value 2^n == interval.
 *
 */
static unsigned int xhci_parse_exponent_interval(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	unsigned int interval;

	interval = clamp_val(ep->desc.bInterval, 1, 16) - 1;
	if (interval != ep->desc.bInterval - 1)
		dev_warn(&udev->dev,
			 "ep %#x - rounding interval to %d %sframes\n",
			 ep->desc.bEndpointAddress,
			 1 << interval,
			 udev->speed == USB_SPEED_FULL ? "" : "micro");

	if (udev->speed == USB_SPEED_FULL) {
		/*
		 * Full speed isoc endpoints specify interval in frames,
		 * not microframes. We are using microframes everywhere,
		 * so adjust accordingly.
		 */
		interval += 3;	/* 1 frame = 2^3 uframes */
	}

	return interval;
}

/*
 * Convert bInterval expressed in microframes (in 1-255 range) to exponent of
 * microframes, rounded down to nearest power of 2.
 */
static unsigned int xhci_microframes_to_exponent(struct usb_device *udev,
		struct usb_host_endpoint *ep, unsigned int desc_interval,
		unsigned int min_exponent, unsigned int max_exponent)
{
	unsigned int interval;

	interval = fls(desc_interval) - 1;
	interval = clamp_val(interval, min_exponent, max_exponent);
	if ((1 << interval) != desc_interval)
		dev_dbg(&udev->dev,
			 "ep %#x - rounding interval to %d microframes, ep desc says %d microframes\n",
			 ep->desc.bEndpointAddress,
			 1 << interval,
			 desc_interval);

	return interval;
}

static unsigned int xhci_parse_microframe_interval(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	if (ep->desc.bInterval == 0)
		return 0;
	return xhci_microframes_to_exponent(udev, ep,
			ep->desc.bInterval, 0, 15);
}


static unsigned int xhci_parse_frame_interval(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	return xhci_microframes_to_exponent(udev, ep,
			ep->desc.bInterval * 8, 3, 10);
}

/* Return the polling or NAK interval.
 *
 * The polling interval is expressed in "microframes".  If xHCI's Interval field
 * is set to N, it will service the endpoint every 2^(Interval)*125us.
 *
 * The NAK interval is one NAK per 1 to 255 microframes, or no NAKs if interval
 * is set to 0.
 */
static unsigned int xhci_get_endpoint_interval(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	unsigned int interval = 0;

	switch (udev->speed) {
	case USB_SPEED_HIGH:
		/* Max NAK rate */
		if (usb_endpoint_xfer_control(&ep->desc) ||
		    usb_endpoint_xfer_bulk(&ep->desc)) {
			interval = xhci_parse_microframe_interval(udev, ep);
			break;
		}
		fallthrough;	/* SS and HS isoc/int have same decoding */

	case USB_SPEED_SUPER_PLUS:
	case USB_SPEED_SUPER:
		if (usb_endpoint_xfer_int(&ep->desc) ||
		    usb_endpoint_xfer_isoc(&ep->desc)) {
			interval = xhci_parse_exponent_interval(udev, ep);
		}
		break;

	case USB_SPEED_FULL:
		if (usb_endpoint_xfer_isoc(&ep->desc)) {
			interval = xhci_parse_exponent_interval(udev, ep);
			break;
		}
		/*
		 * Fall through for interrupt endpoint interval decoding
		 * since it uses the same rules as low speed interrupt
		 * endpoints.
		 */
		fallthrough;

	case USB_SPEED_LOW:
		if (usb_endpoint_xfer_int(&ep->desc) ||
		    usb_endpoint_xfer_isoc(&ep->desc)) {

			interval = xhci_parse_frame_interval(udev, ep);
		}
		break;

	default:
		BUG();
	}
	return interval;
}

/* The "Mult" field in the endpoint context is only set for SuperSpeed isoc eps.
 * High speed endpoint descriptors can define "the number of additional
 * transaction opportunities per microframe", but that goes in the Max Burst
 * endpoint context field.
 */
static u32 xhci_get_endpoint_mult(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	if (udev->speed < USB_SPEED_SUPER ||
			!usb_endpoint_xfer_isoc(&ep->desc))
		return 0;
	return ep->ss_ep_comp.bmAttributes;
}

static u32 xhci_get_endpoint_max_burst(struct usb_device *udev,
				       struct usb_host_endpoint *ep)
{
	/* Super speed and Plus have max burst in ep companion desc */
	if (udev->speed >= USB_SPEED_SUPER)
		return ep->ss_ep_comp.bMaxBurst;

	if (udev->speed == USB_SPEED_HIGH &&
	    (usb_endpoint_xfer_isoc(&ep->desc) ||
	     usb_endpoint_xfer_int(&ep->desc)))
		return usb_endpoint_maxp_mult(&ep->desc) - 1;

	return 0;
}

static u32 xhci_get_endpoint_type(struct usb_host_endpoint *ep)
{
	int in;

	in = usb_endpoint_dir_in(&ep->desc);

	switch (usb_endpoint_type(&ep->desc)) {
	case USB_ENDPOINT_XFER_CONTROL:
		return CTRL_EP;
	case USB_ENDPOINT_XFER_BULK:
		return in ? BULK_IN_EP : BULK_OUT_EP;
	case USB_ENDPOINT_XFER_ISOC:
		return in ? ISOC_IN_EP : ISOC_OUT_EP;
	case USB_ENDPOINT_XFER_INT:
		return in ? INT_IN_EP : INT_OUT_EP;
	}
	return 0;
}

/* Return the maximum endpoint service interval time (ESIT) payload.
 * Basically, this is the maxpacket size, multiplied by the burst size
 * and mult size.
 */
static u32 xhci_get_max_esit_payload(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	int max_burst;
	int max_packet;

	/* Only applies for interrupt or isochronous endpoints */
	if (usb_endpoint_xfer_control(&ep->desc) ||
			usb_endpoint_xfer_bulk(&ep->desc))
		return 0;

	/* SuperSpeedPlus Isoc ep sending over 48k per esit */
	if ((udev->speed >= USB_SPEED_SUPER_PLUS) &&
	    USB_SS_SSP_ISOC_COMP(ep->ss_ep_comp.bmAttributes))
		return le32_to_cpu(ep->ssp_isoc_ep_comp.dwBytesPerInterval);

	/* SuperSpeed or SuperSpeedPlus Isoc ep with less than 48k per esit */
	if (udev->speed >= USB_SPEED_SUPER)
		return le16_to_cpu(ep->ss_ep_comp.wBytesPerInterval);

	max_packet = usb_endpoint_maxp(&ep->desc);
	max_burst = usb_endpoint_maxp_mult(&ep->desc);
	/* A 0 in max burst means 1 transfer per ESIT */
	return max_packet * max_burst;
}

/* Set up an endpoint with one ring segment.  Do not allocate stream rings.
 * Drivers will have to call usb_alloc_streams() to do that.
 */
int xhci_endpoint_init(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		struct usb_device *udev,
		struct usb_host_endpoint *ep,
		gfp_t mem_flags)
{
	unsigned int ep_index;
	struct xhci_ep_ctx *ep_ctx;
	struct xhci_ring *ep_ring;
	unsigned int max_packet;
	enum xhci_ring_type ring_type;
	u32 max_esit_payload;
	u32 endpoint_type;
	unsigned int max_burst;
	unsigned int interval;
	unsigned int mult;
	unsigned int avg_trb_len;
	unsigned int err_count = 0;

	ep_index = xhci_get_endpoint_index(&ep->desc);
	ep_ctx = xhci_get_ep_ctx(xhci, virt_dev->in_ctx, ep_index);

	endpoint_type = xhci_get_endpoint_type(ep);
	if (!endpoint_type)
		return -EINVAL;

	ring_type = usb_endpoint_type(&ep->desc);

	/*
	 * Get values to fill the endpoint context, mostly from ep descriptor.
	 * The average TRB buffer lengt for bulk endpoints is unclear as we
	 * have no clue on scatter gather list entry size. For Isoc and Int,
	 * set it to max available. See xHCI 1.1 spec 4.14.1.1 for details.
	 */
	max_esit_payload = xhci_get_max_esit_payload(udev, ep);
	interval = xhci_get_endpoint_interval(udev, ep);

	/* Periodic endpoint bInterval limit quirk */
	if (usb_endpoint_xfer_int(&ep->desc) ||
	    usb_endpoint_xfer_isoc(&ep->desc)) {
		if ((xhci->quirks & XHCI_LIMIT_ENDPOINT_INTERVAL_9) &&
		    interval >= 9) {
			interval = 8;
		}
		if ((xhci->quirks & XHCI_LIMIT_ENDPOINT_INTERVAL_7) &&
		    udev->speed >= USB_SPEED_HIGH &&
		    interval >= 7) {
			interval = 6;
		}
	}

	mult = xhci_get_endpoint_mult(udev, ep);
	max_packet = usb_endpoint_maxp(&ep->desc);
	max_burst = xhci_get_endpoint_max_burst(udev, ep);
	avg_trb_len = max_esit_payload;

	/* FIXME dig Mult and streams info out of ep companion desc */

	/* Allow 3 retries for everything but isoc, set CErr = 3 */
	if (!usb_endpoint_xfer_isoc(&ep->desc))
		err_count = 3;
	/* HS bulk max packet should be 512, FS bulk supports 8, 16, 32 or 64 */
	if (usb_endpoint_xfer_bulk(&ep->desc)) {
		if (udev->speed == USB_SPEED_HIGH)
			max_packet = 512;
		if (udev->speed == USB_SPEED_FULL) {
			max_packet = rounddown_pow_of_two(max_packet);
			max_packet = clamp_val(max_packet, 8, 64);
		}
	}
	/* xHCI 1.0 and 1.1 indicates that ctrl ep avg TRB Length should be 8 */
	if (usb_endpoint_xfer_control(&ep->desc) && xhci->hci_version >= 0x100)
		avg_trb_len = 8;
	/* xhci 1.1 with LEC support doesn't use mult field, use RsvdZ */
	if ((xhci->hci_version > 0x100) && HCC2_LEC(xhci->hcc_params2))
		mult = 0;

	/* Set up the endpoint ring */
	virt_dev->eps[ep_index].new_ring =
		xhci_ring_alloc(xhci, 2, ring_type, max_packet, mem_flags);
	if (!virt_dev->eps[ep_index].new_ring)
		return -ENOMEM;

	virt_dev->eps[ep_index].skip = false;
	ep_ring = virt_dev->eps[ep_index].new_ring;

	/* Fill the endpoint context */
	ep_ctx->ep_info = cpu_to_le32(EP_MAX_ESIT_PAYLOAD_HI(max_esit_payload) |
				      EP_INTERVAL(interval) |
				      EP_MULT(mult));
	ep_ctx->ep_info2 = cpu_to_le32(EP_TYPE(endpoint_type) |
				       MAX_PACKET(max_packet) |
				       MAX_BURST(max_burst) |
				       ERROR_COUNT(err_count));
	ep_ctx->deq = cpu_to_le64(ep_ring->first_seg->dma |
				  ep_ring->cycle_state);

	ep_ctx->tx_info = cpu_to_le32(EP_MAX_ESIT_PAYLOAD_LO(max_esit_payload) |
				      EP_AVG_TRB_LENGTH(avg_trb_len));

	return 0;
}

void xhci_endpoint_zero(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		struct usb_host_endpoint *ep)
{
	unsigned int ep_index;
	struct xhci_ep_ctx *ep_ctx;

	ep_index = xhci_get_endpoint_index(&ep->desc);
	ep_ctx = xhci_get_ep_ctx(xhci, virt_dev->in_ctx, ep_index);

	ep_ctx->ep_info = 0;
	ep_ctx->ep_info2 = 0;
	ep_ctx->deq = 0;
	ep_ctx->tx_info = 0;
	/* Don't free the endpoint ring until the set interface or configuration
	 * request succeeds.
	 */
}

void xhci_clear_endpoint_bw_info(struct xhci_bw_info *bw_info)
{
	bw_info->ep_interval = 0;
	bw_info->mult = 0;
	bw_info->num_packets = 0;
	bw_info->max_packet_size = 0;
	bw_info->type = 0;
	bw_info->max_esit_payload = 0;
}

void xhci_update_bw_info(struct xhci_hcd *xhci,
		struct xhci_container_ctx *in_ctx,
		struct xhci_input_control_ctx *ctrl_ctx,
		struct xhci_virt_device *virt_dev)
{
	struct xhci_bw_info *bw_info;
	struct xhci_ep_ctx *ep_ctx;
	unsigned int ep_type;
	int i;

	for (i = 1; i < 31; i++) {
		bw_info = &virt_dev->eps[i].bw_info;

		/* We can't tell what endpoint type is being dropped, but
		 * unconditionally clearing the bandwidth info for non-periodic
		 * endpoints should be harmless because the info will never be
		 * set in the first place.
		 */
		if (!EP_IS_ADDED(ctrl_ctx, i) && EP_IS_DROPPED(ctrl_ctx, i)) {
			/* Dropped endpoint */
			xhci_clear_endpoint_bw_info(bw_info);
			continue;
		}

		if (EP_IS_ADDED(ctrl_ctx, i)) {
			ep_ctx = xhci_get_ep_ctx(xhci, in_ctx, i);
			ep_type = CTX_TO_EP_TYPE(le32_to_cpu(ep_ctx->ep_info2));

			/* Ignore non-periodic endpoints */
			if (ep_type != ISOC_OUT_EP && ep_type != INT_OUT_EP &&
					ep_type != ISOC_IN_EP &&
					ep_type != INT_IN_EP)
				continue;

			/* Added or changed endpoint */
			bw_info->ep_interval = CTX_TO_EP_INTERVAL(
					le32_to_cpu(ep_ctx->ep_info));
			/* Number of packets and mult are zero-based in the
			 * input context, but we want one-based for the
			 * interval table.
			 */
			bw_info->mult = CTX_TO_EP_MULT(
					le32_to_cpu(ep_ctx->ep_info)) + 1;
			bw_info->num_packets = CTX_TO_MAX_BURST(
					le32_to_cpu(ep_ctx->ep_info2)) + 1;
			bw_info->max_packet_size = MAX_PACKET_DECODED(
					le32_to_cpu(ep_ctx->ep_info2));
			bw_info->type = ep_type;
			bw_info->max_esit_payload = CTX_TO_MAX_ESIT_PAYLOAD(
					le32_to_cpu(ep_ctx->tx_info));
		}
	}
}

/* Copy output xhci_ep_ctx to the input xhci_ep_ctx copy.
 * Useful when you want to change one particular aspect of the endpoint and then
 * issue a configure endpoint command.
 */
void xhci_endpoint_copy(struct xhci_hcd *xhci,
		struct xhci_container_ctx *in_ctx,
		struct xhci_container_ctx *out_ctx,
		unsigned int ep_index)
{
	struct xhci_ep_ctx *out_ep_ctx;
	struct xhci_ep_ctx *in_ep_ctx;

	out_ep_ctx = xhci_get_ep_ctx(xhci, out_ctx, ep_index);
	in_ep_ctx = xhci_get_ep_ctx(xhci, in_ctx, ep_index);

	in_ep_ctx->ep_info = out_ep_ctx->ep_info;
	in_ep_ctx->ep_info2 = out_ep_ctx->ep_info2;
	in_ep_ctx->deq = out_ep_ctx->deq;
	in_ep_ctx->tx_info = out_ep_ctx->tx_info;
	if (xhci->quirks & XHCI_MTK_HOST) {
		in_ep_ctx->reserved[0] = out_ep_ctx->reserved[0];
		in_ep_ctx->reserved[1] = out_ep_ctx->reserved[1];
	}
}

/* Copy output xhci_slot_ctx to the input xhci_slot_ctx.
 * Useful when you want to change one particular aspect of the endpoint and then
 * issue a configure endpoint command.  Only the context entries field matters,
 * but we'll copy the whole thing anyway.
 */
void xhci_slot_copy(struct xhci_hcd *xhci,
		struct xhci_container_ctx *in_ctx,
		struct xhci_container_ctx *out_ctx)
{
	struct xhci_slot_ctx *in_slot_ctx;
	struct xhci_slot_ctx *out_slot_ctx;

	in_slot_ctx = xhci_get_slot_ctx(xhci, in_ctx);
	out_slot_ctx = xhci_get_slot_ctx(xhci, out_ctx);

	in_slot_ctx->dev_info = out_slot_ctx->dev_info;
	in_slot_ctx->dev_info2 = out_slot_ctx->dev_info2;
	in_slot_ctx->tt_info = out_slot_ctx->tt_info;
	in_slot_ctx->dev_state = out_slot_ctx->dev_state;
}

/* Set up the scratchpad buffer array and scratchpad buffers, if needed. */
static int scratchpad_alloc(struct xhci_hcd *xhci, gfp_t flags)
{
	int i;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	int num_sp = HCS_MAX_SCRATCHPAD(xhci->hcs_params2);

	xhci_dbg_trace(xhci, trace_xhci_dbg_init,
			"Allocating %d scratchpad buffers", num_sp);

	if (!num_sp)
		return 0;

	xhci->scratchpad = kzalloc_node(sizeof(*xhci->scratchpad), flags,
				dev_to_node(dev));
	if (!xhci->scratchpad)
		goto fail_sp;

	xhci->scratchpad->sp_array = dma_alloc_coherent(dev,
				     array_size(sizeof(u64), num_sp),
				     &xhci->scratchpad->sp_dma, flags);
	if (!xhci->scratchpad->sp_array)
		goto fail_sp2;

	xhci->scratchpad->sp_buffers = kcalloc_node(num_sp, sizeof(void *),
					flags, dev_to_node(dev));
	if (!xhci->scratchpad->sp_buffers)
		goto fail_sp3;

	xhci->dcbaa->dev_context_ptrs[0] = cpu_to_le64(xhci->scratchpad->sp_dma);
	for (i = 0; i < num_sp; i++) {
		dma_addr_t dma;
		void *buf = dma_alloc_coherent(dev, xhci->page_size, &dma,
					       flags);
		if (!buf)
			goto fail_sp4;

		xhci->scratchpad->sp_array[i] = dma;
		xhci->scratchpad->sp_buffers[i] = buf;
	}

	return 0;

 fail_sp4:
	while (i--)
		dma_free_coherent(dev, xhci->page_size,
				    xhci->scratchpad->sp_buffers[i],
				    xhci->scratchpad->sp_array[i]);

	kfree(xhci->scratchpad->sp_buffers);

 fail_sp3:
	dma_free_coherent(dev, array_size(sizeof(u64), num_sp),
			    xhci->scratchpad->sp_array,
			    xhci->scratchpad->sp_dma);

 fail_sp2:
	kfree(xhci->scratchpad);
	xhci->scratchpad = NULL;

 fail_sp:
	return -ENOMEM;
}

static void scratchpad_free(struct xhci_hcd *xhci)
{
	int num_sp;
	int i;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	if (!xhci->scratchpad)
		return;

	num_sp = HCS_MAX_SCRATCHPAD(xhci->hcs_params2);

	for (i = 0; i < num_sp; i++) {
		dma_free_coherent(dev, xhci->page_size,
				    xhci->scratchpad->sp_buffers[i],
				    xhci->scratchpad->sp_array[i]);
	}
	kfree(xhci->scratchpad->sp_buffers);
	dma_free_coherent(dev, array_size(sizeof(u64), num_sp),
			    xhci->scratchpad->sp_array,
			    xhci->scratchpad->sp_dma);
	kfree(xhci->scratchpad);
	xhci->scratchpad = NULL;
}

struct xhci_command *xhci_alloc_command(struct xhci_hcd *xhci,
		bool allocate_completion, gfp_t mem_flags)
{
	struct xhci_command *command;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	command = kzalloc_node(sizeof(*command), mem_flags, dev_to_node(dev));
	if (!command)
		return NULL;

	if (allocate_completion) {
		command->completion =
			kzalloc_node(sizeof(struct completion), mem_flags,
				dev_to_node(dev));
		if (!command->completion) {
			kfree(command);
			return NULL;
		}
		init_completion(command->completion);
	}

	command->status = 0;
	/* set default timeout to 5000 ms */
	command->timeout_ms = XHCI_CMD_DEFAULT_TIMEOUT;
	INIT_LIST_HEAD(&command->cmd_list);
	return command;
}

struct xhci_command *xhci_alloc_command_with_ctx(struct xhci_hcd *xhci,
		bool allocate_completion, gfp_t mem_flags)
{
	struct xhci_command *command;

	command = xhci_alloc_command(xhci, allocate_completion, mem_flags);
	if (!command)
		return NULL;

	command->in_ctx = xhci_alloc_container_ctx(xhci, XHCI_CTX_TYPE_INPUT,
						   mem_flags);
	if (!command->in_ctx) {
		kfree(command->completion);
		kfree(command);
		return NULL;
	}
	return command;
}

void xhci_urb_free_priv(struct urb_priv *urb_priv)
{
	kfree(urb_priv);
}

void xhci_free_command(struct xhci_hcd *xhci,
		struct xhci_command *command)
{
	xhci_free_container_ctx(xhci,
			command->in_ctx);
	kfree(command->completion);
	kfree(command);
}

static int xhci_alloc_erst(struct xhci_hcd *xhci,
		    struct xhci_ring *evt_ring,
		    struct xhci_erst *erst,
		    gfp_t flags)
{
	size_t size;
	unsigned int val;
	struct xhci_segment *seg;
	struct xhci_erst_entry *entry;

	size = array_size(sizeof(struct xhci_erst_entry), evt_ring->num_segs);
	erst->entries = dma_alloc_coherent(xhci_to_hcd(xhci)->self.sysdev,
					   size, &erst->erst_dma_addr, flags);
	if (!erst->entries)
		return -ENOMEM;

	erst->num_entries = evt_ring->num_segs;

	seg = evt_ring->first_seg;
	for (val = 0; val < evt_ring->num_segs; val++) {
		entry = &erst->entries[val];
		entry->seg_addr = cpu_to_le64(seg->dma);
		entry->seg_size = cpu_to_le32(TRBS_PER_SEGMENT);
		entry->rsvd = 0;
		seg = seg->next;
	}

	return 0;
}

static void
xhci_remove_interrupter(struct xhci_hcd *xhci, struct xhci_interrupter *ir)
{
	u32 tmp;

	if (!ir)
		return;

	/*
	 * Clean out interrupter registers except ERSTBA. Clearing either the
	 * low or high 32 bits of ERSTBA immediately causes the controller to
	 * dereference the partially cleared 64 bit address, causing IOMMU error.
	 */
	if (ir->ir_set) {
		tmp = readl(&ir->ir_set->erst_size);
		tmp &= ~ERST_SIZE_MASK;
		writel(tmp, &ir->ir_set->erst_size);

		xhci_update_erst_dequeue(xhci, ir, true);
	}
}

static void
xhci_free_interrupter(struct xhci_hcd *xhci, struct xhci_interrupter *ir)
{
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	size_t erst_size;

	if (!ir)
		return;

	erst_size = array_size(sizeof(struct xhci_erst_entry), ir->erst.num_entries);
	if (ir->erst.entries)
		dma_free_coherent(dev, erst_size,
				  ir->erst.entries,
				  ir->erst.erst_dma_addr);
	ir->erst.entries = NULL;

	/* free interrupter event ring */
	if (ir->event_ring)
		xhci_ring_free(xhci, ir->event_ring);

	ir->event_ring = NULL;

	kfree(ir);
}

void xhci_remove_secondary_interrupter(struct usb_hcd *hcd, struct xhci_interrupter *ir)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	unsigned int intr_num;

	spin_lock_irq(&xhci->lock);

	/* interrupter 0 is primary interrupter, don't touch it */
	if (!ir || !ir->intr_num || ir->intr_num >= xhci->max_interrupters) {
		xhci_dbg(xhci, "Invalid secondary interrupter, can't remove\n");
		spin_unlock_irq(&xhci->lock);
		return;
	}

	/*
	 * Cleanup secondary interrupter to ensure there are no pending events.
	 * This also updates event ring dequeue pointer back to the start.
	 */
	xhci_skip_sec_intr_events(xhci, ir->event_ring, ir);
	intr_num = ir->intr_num;

	xhci_remove_interrupter(xhci, ir);
	xhci->interrupters[intr_num] = NULL;

	spin_unlock_irq(&xhci->lock);

	xhci_free_interrupter(xhci, ir);
}
EXPORT_SYMBOL_GPL(xhci_remove_secondary_interrupter);

void xhci_mem_cleanup(struct xhci_hcd *xhci)
{
	struct device	*dev = xhci_to_hcd(xhci)->self.sysdev;
	int i, j, num_ports;

	cancel_delayed_work_sync(&xhci->cmd_timer);

	for (i = 0; xhci->interrupters && i < xhci->max_interrupters; i++) {
		if (xhci->interrupters[i]) {
			xhci_remove_interrupter(xhci, xhci->interrupters[i]);
			xhci_free_interrupter(xhci, xhci->interrupters[i]);
			xhci->interrupters[i] = NULL;
		}
	}
	xhci_dbg_trace(xhci, trace_xhci_dbg_init, "Freed interrupters");

	if (xhci->cmd_ring)
		xhci_ring_free(xhci, xhci->cmd_ring);
	xhci->cmd_ring = NULL;
	xhci_dbg_trace(xhci, trace_xhci_dbg_init, "Freed command ring");
	xhci_cleanup_command_queue(xhci);

	num_ports = HCS_MAX_PORTS(xhci->hcs_params1);
	for (i = 0; i < num_ports && xhci->rh_bw; i++) {
		struct xhci_interval_bw_table *bwt = &xhci->rh_bw[i].bw_table;
		for (j = 0; j < XHCI_MAX_INTERVAL; j++) {
			struct list_head *ep = &bwt->interval_bw[j].endpoints;
			while (!list_empty(ep))
				list_del_init(ep->next);
		}
	}

	for (i = HCS_MAX_SLOTS(xhci->hcs_params1); i > 0; i--)
		xhci_free_virt_devices_depth_first(xhci, i);

	dma_pool_destroy(xhci->segment_pool);
	xhci->segment_pool = NULL;
	xhci_dbg_trace(xhci, trace_xhci_dbg_init, "Freed segment pool");

	dma_pool_destroy(xhci->device_pool);
	xhci->device_pool = NULL;
	xhci_dbg_trace(xhci, trace_xhci_dbg_init, "Freed device context pool");

	dma_pool_destroy(xhci->small_streams_pool);
	xhci->small_streams_pool = NULL;
	xhci_dbg_trace(xhci, trace_xhci_dbg_init,
			"Freed small stream array pool");

	dma_pool_destroy(xhci->port_bw_pool);
	xhci->port_bw_pool = NULL;
	xhci_dbg_trace(xhci, trace_xhci_dbg_init,
			"Freed xhci port bw array pool");

	dma_pool_destroy(xhci->medium_streams_pool);
	xhci->medium_streams_pool = NULL;
	xhci_dbg_trace(xhci, trace_xhci_dbg_init,
			"Freed medium stream array pool");

	if (xhci->dcbaa)
		dma_free_coherent(dev, sizeof(*xhci->dcbaa),
				xhci->dcbaa, xhci->dcbaa->dma);
	xhci->dcbaa = NULL;

	scratchpad_free(xhci);

	if (!xhci->rh_bw)
		goto no_bw;

	for (i = 0; i < num_ports; i++) {
		struct xhci_tt_bw_info *tt, *n;
		list_for_each_entry_safe(tt, n, &xhci->rh_bw[i].tts, tt_list) {
			list_del(&tt->tt_list);
			kfree(tt);
		}
	}

no_bw:
	xhci->cmd_ring_reserved_trbs = 0;
	xhci->usb2_rhub.num_ports = 0;
	xhci->usb3_rhub.num_ports = 0;
	xhci->num_active_eps = 0;
	kfree(xhci->usb2_rhub.ports);
	kfree(xhci->usb3_rhub.ports);
	kfree(xhci->hw_ports);
	kfree(xhci->rh_bw);
	for (i = 0; i < xhci->num_port_caps; i++)
		kfree(xhci->port_caps[i].psi);
	kfree(xhci->port_caps);
	kfree(xhci->interrupters);
	xhci->num_port_caps = 0;

	xhci->usb2_rhub.ports = NULL;
	xhci->usb3_rhub.ports = NULL;
	xhci->hw_ports = NULL;
	xhci->rh_bw = NULL;
	xhci->port_caps = NULL;
	xhci->interrupters = NULL;

	xhci->page_size = 0;
	xhci->usb2_rhub.bus_state.bus_suspended = 0;
	xhci->usb3_rhub.bus_state.bus_suspended = 0;
}

static void xhci_set_hc_event_deq(struct xhci_hcd *xhci, struct xhci_interrupter *ir)
{
	dma_addr_t deq;

	deq = xhci_trb_virt_to_dma(ir->event_ring->deq_seg,
			ir->event_ring->dequeue);
	if (!deq)
		xhci_warn(xhci, "WARN something wrong with SW event ring dequeue ptr.\n");
	/* Update HC event ring dequeue pointer */
	/* Don't clear the EHB bit (which is RW1C) because
	 * there might be more events to service.
	 */
	xhci_dbg_trace(xhci, trace_xhci_dbg_init,
		       "// Write event ring dequeue pointer, preserving EHB bit");
	xhci_write_64(xhci, deq & ERST_PTR_MASK, &ir->ir_set->erst_dequeue);
}

static void xhci_add_in_port(struct xhci_hcd *xhci, unsigned int num_ports,
		__le32 __iomem *addr, int max_caps)
{
	u32 temp, port_offset, port_count;
	int i;
	u8 major_revision, minor_revision, tmp_minor_revision;
	struct xhci_hub *rhub;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	struct xhci_port_cap *port_cap;

	temp = readl(addr);
	major_revision = XHCI_EXT_PORT_MAJOR(temp);
	minor_revision = XHCI_EXT_PORT_MINOR(temp);

	if (major_revision == 0x03) {
		rhub = &xhci->usb3_rhub;
		/*
		 * Some hosts incorrectly use sub-minor version for minor
		 * version (i.e. 0x02 instead of 0x20 for bcdUSB 0x320 and 0x01
		 * for bcdUSB 0x310). Since there is no USB release with sub
		 * minor version 0x301 to 0x309, we can assume that they are
		 * incorrect and fix it here.
		 */
		if (minor_revision > 0x00 && minor_revision < 0x10)
			minor_revision <<= 4;
		/*
		 * Some zhaoxin's xHCI controller that follow usb3.1 spec
		 * but only support Gen1.
		 */
		if (xhci->quirks & XHCI_ZHAOXIN_HOST) {
			tmp_minor_revision = minor_revision;
			minor_revision = 0;
		}

	} else if (major_revision <= 0x02) {
		rhub = &xhci->usb2_rhub;
	} else {
		xhci_warn(xhci, "Ignoring unknown port speed, Ext Cap %p, revision = 0x%x\n",
				addr, major_revision);
		/* Ignoring port protocol we can't understand. FIXME */
		return;
	}

	/* Port offset and count in the third dword, see section 7.2 */
	temp = readl(addr + 2);
	port_offset = XHCI_EXT_PORT_OFF(temp);
	port_count = XHCI_EXT_PORT_COUNT(temp);
	xhci_dbg_trace(xhci, trace_xhci_dbg_init,
		       "Ext Cap %p, port offset = %u, count = %u, revision = 0x%x",
		       addr, port_offset, port_count, major_revision);
	/* Port count includes the current port offset */
	if (port_offset == 0 || (port_offset + port_count - 1) > num_ports)
		/* WTF? "Valid values are ‘1’ to MaxPorts" */
		return;

	port_cap = &xhci->port_caps[xhci->num_port_caps++];
	if (xhci->num_port_caps > max_caps)
		return;

	port_cap->psi_count = XHCI_EXT_PORT_PSIC(temp);

	if (port_cap->psi_count) {
		port_cap->psi = kcalloc_node(port_cap->psi_count,
					     sizeof(*port_cap->psi),
					     GFP_KERNEL, dev_to_node(dev));
		if (!port_cap->psi)
			port_cap->psi_count = 0;

		port_cap->psi_uid_count++;
		for (i = 0; i < port_cap->psi_count; i++) {
			port_cap->psi[i] = readl(addr + 4 + i);

			/* count unique ID values, two consecutive entries can
			 * have the same ID if link is assymetric
			 */
			if (i && (XHCI_EXT_PORT_PSIV(port_cap->psi[i]) !=
				  XHCI_EXT_PORT_PSIV(port_cap->psi[i - 1])))
				port_cap->psi_uid_count++;

			if (xhci->quirks & XHCI_ZHAOXIN_HOST &&
			    major_revision == 0x03 &&
			    XHCI_EXT_PORT_PSIV(port_cap->psi[i]) >= 5)
				minor_revision = tmp_minor_revision;

			xhci_dbg(xhci, "PSIV:%d PSIE:%d PLT:%d PFD:%d LP:%d PSIM:%d\n",
				  XHCI_EXT_PORT_PSIV(port_cap->psi[i]),
				  XHCI_EXT_PORT_PSIE(port_cap->psi[i]),
				  XHCI_EXT_PORT_PLT(port_cap->psi[i]),
				  XHCI_EXT_PORT_PFD(port_cap->psi[i]),
				  XHCI_EXT_PORT_LP(port_cap->psi[i]),
				  XHCI_EXT_PORT_PSIM(port_cap->psi[i]));
		}
	}

	rhub->maj_rev = major_revision;

	if (rhub->min_rev < minor_revision)
		rhub->min_rev = minor_revision;

	port_cap->maj_rev = major_revision;
	port_cap->min_rev = minor_revision;
	port_cap->protocol_caps = temp;

	if ((xhci->hci_version >= 0x100) && (major_revision != 0x03) &&
		 (temp & XHCI_HLC)) {
		xhci_dbg_trace(xhci, trace_xhci_dbg_init,
			       "xHCI 1.0: support USB2 hardware lpm");
		xhci->hw_lpm_support = 1;
	}

	port_offset--;
	for (i = port_offset; i < (port_offset + port_count); i++) {
		struct xhci_port *hw_port = &xhci->hw_ports[i];
		/* Duplicate entry.  Ignore the port if the revisions differ. */
		if (hw_port->rhub) {
			xhci_warn(xhci, "Duplicate port entry, Ext Cap %p, port %u\n", addr, i);
			xhci_warn(xhci, "Port was marked as USB %u, duplicated as USB %u\n",
					hw_port->rhub->maj_rev, major_revision);
			/* Only adjust the roothub port counts if we haven't
			 * found a similar duplicate.
			 */
			if (hw_port->rhub != rhub &&
				 hw_port->hcd_portnum != DUPLICATE_ENTRY) {
				hw_port->rhub->num_ports--;
				hw_port->hcd_portnum = DUPLICATE_ENTRY;
			}
			continue;
		}
		hw_port->rhub = rhub;
		hw_port->port_cap = port_cap;
		rhub->num_ports++;
	}
	/* FIXME: Should we disable ports not in the Extended Capabilities? */
}

static void xhci_create_rhub_port_array(struct xhci_hcd *xhci,
					struct xhci_hub *rhub, gfp_t flags)
{
	int port_index = 0;
	int i;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	if (!rhub->num_ports)
		return;
	rhub->ports = kcalloc_node(rhub->num_ports, sizeof(*rhub->ports),
			flags, dev_to_node(dev));
	if (!rhub->ports)
		return;

	for (i = 0; i < HCS_MAX_PORTS(xhci->hcs_params1); i++) {
		if (xhci->hw_ports[i].rhub != rhub ||
		    xhci->hw_ports[i].hcd_portnum == DUPLICATE_ENTRY)
			continue;
		xhci->hw_ports[i].hcd_portnum = port_index;
		rhub->ports[port_index] = &xhci->hw_ports[i];
		port_index++;
		if (port_index == rhub->num_ports)
			break;
	}
}

/*
 * Scan the Extended Capabilities for the "Supported Protocol Capabilities" that
 * specify what speeds each port is supposed to be.  We can't count on the port
 * speed bits in the PORTSC register being correct until a device is connected,
 * but we need to set up the two fake roothubs with the correct number of USB
 * 3.0 and USB 2.0 ports at host controller initialization time.
 */
static int xhci_setup_port_arrays(struct xhci_hcd *xhci, gfp_t flags)
{
	void __iomem *base;
	u32 offset;
	unsigned int num_ports;
	int i, j;
	int cap_count = 0;
	u32 cap_start;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	num_ports = HCS_MAX_PORTS(xhci->hcs_params1);
	xhci->hw_ports = kcalloc_node(num_ports, sizeof(*xhci->hw_ports),
				flags, dev_to_node(dev));
	if (!xhci->hw_ports)
		return -ENOMEM;

	for (i = 0; i < num_ports; i++) {
		xhci->hw_ports[i].addr = &xhci->op_regs->port_status_base +
			NUM_PORT_REGS * i;
		xhci->hw_ports[i].hw_portnum = i;

		init_completion(&xhci->hw_ports[i].rexit_done);
		init_completion(&xhci->hw_ports[i].u3exit_done);
	}

	xhci->rh_bw = kcalloc_node(num_ports, sizeof(*xhci->rh_bw), flags,
				   dev_to_node(dev));
	if (!xhci->rh_bw)
		return -ENOMEM;
	for (i = 0; i < num_ports; i++) {
		struct xhci_interval_bw_table *bw_table;

		INIT_LIST_HEAD(&xhci->rh_bw[i].tts);
		bw_table = &xhci->rh_bw[i].bw_table;
		for (j = 0; j < XHCI_MAX_INTERVAL; j++)
			INIT_LIST_HEAD(&bw_table->interval_bw[j].endpoints);
	}
	base = &xhci->cap_regs->hc_capbase;

	cap_start = xhci_find_next_ext_cap(base, 0, XHCI_EXT_CAPS_PROTOCOL);
	if (!cap_start) {
		xhci_err(xhci, "No Extended Capability registers, unable to set up roothub\n");
		return -ENODEV;
	}

	offset = cap_start;
	/* count extended protocol capability entries for later caching */
	while (offset) {
		cap_count++;
		offset = xhci_find_next_ext_cap(base, offset,
						      XHCI_EXT_CAPS_PROTOCOL);
	}

	xhci->port_caps = kcalloc_node(cap_count, sizeof(*xhci->port_caps),
				flags, dev_to_node(dev));
	if (!xhci->port_caps)
		return -ENOMEM;

	offset = cap_start;

	while (offset) {
		xhci_add_in_port(xhci, num_ports, base + offset, cap_count);
		if (xhci->usb2_rhub.num_ports + xhci->usb3_rhub.num_ports ==
		    num_ports)
			break;
		offset = xhci_find_next_ext_cap(base, offset,
						XHCI_EXT_CAPS_PROTOCOL);
	}
	if (xhci->usb2_rhub.num_ports == 0 && xhci->usb3_rhub.num_ports == 0) {
		xhci_warn(xhci, "No ports on the roothubs?\n");
		return -ENODEV;
	}
	xhci_dbg_trace(xhci, trace_xhci_dbg_init,
		       "Found %u USB 2.0 ports and %u USB 3.0 ports.",
		       xhci->usb2_rhub.num_ports, xhci->usb3_rhub.num_ports);

	/* Place limits on the number of roothub ports so that the hub
	 * descriptors aren't longer than the USB core will allocate.
	 */
	if (xhci->usb3_rhub.num_ports > USB_SS_MAXPORTS) {
		xhci_dbg_trace(xhci, trace_xhci_dbg_init,
				"Limiting USB 3.0 roothub ports to %u.",
				USB_SS_MAXPORTS);
		xhci->usb3_rhub.num_ports = USB_SS_MAXPORTS;
	}
	if (xhci->usb2_rhub.num_ports > USB_MAXCHILDREN) {
		xhci_dbg_trace(xhci, trace_xhci_dbg_init,
				"Limiting USB 2.0 roothub ports to %u.",
				USB_MAXCHILDREN);
		xhci->usb2_rhub.num_ports = USB_MAXCHILDREN;
	}

	if (!xhci->usb2_rhub.num_ports)
		xhci_info(xhci, "USB2 root hub has no ports\n");

	if (!xhci->usb3_rhub.num_ports)
		xhci_info(xhci, "USB3 root hub has no ports\n");

	xhci_create_rhub_port_array(xhci, &xhci->usb2_rhub, flags);
	xhci_create_rhub_port_array(xhci, &xhci->usb3_rhub, flags);

	return 0;
}

static struct xhci_interrupter *
xhci_alloc_interrupter(struct xhci_hcd *xhci, unsigned int segs, gfp_t flags)
{
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	struct xhci_interrupter *ir;
	unsigned int max_segs;
	int ret;

	if (!segs)
		segs = ERST_DEFAULT_SEGS;

	max_segs = BIT(HCS_ERST_MAX(xhci->hcs_params2));
	segs = min(segs, max_segs);

	ir = kzalloc_node(sizeof(*ir), flags, dev_to_node(dev));
	if (!ir)
		return NULL;

	ir->event_ring = xhci_ring_alloc(xhci, segs, TYPE_EVENT, 0, flags);
	if (!ir->event_ring) {
		xhci_warn(xhci, "Failed to allocate interrupter event ring\n");
		kfree(ir);
		return NULL;
	}

	ret = xhci_alloc_erst(xhci, ir->event_ring, &ir->erst, flags);
	if (ret) {
		xhci_warn(xhci, "Failed to allocate interrupter erst\n");
		xhci_ring_free(xhci, ir->event_ring);
		kfree(ir);
		return NULL;
	}

	return ir;
}

void xhci_add_interrupter(struct xhci_hcd *xhci, unsigned int intr_num)
{
	struct xhci_interrupter *ir;
	u64 erst_base;
	u32 erst_size;

	ir = xhci->interrupters[intr_num];
	ir->intr_num = intr_num;
	ir->ir_set = &xhci->run_regs->ir_set[intr_num];

	/* set ERST count with the number of entries in the segment table */
	erst_size = readl(&ir->ir_set->erst_size);
	erst_size &= ~ERST_SIZE_MASK;
	erst_size |= ir->event_ring->num_segs;
	writel(erst_size, &ir->ir_set->erst_size);

	erst_base = xhci_read_64(xhci, &ir->ir_set->erst_base);
	erst_base &= ~ERST_BASE_ADDRESS_MASK;
	erst_base |= ir->erst.erst_dma_addr & ERST_BASE_ADDRESS_MASK;
	if (xhci->quirks & XHCI_WRITE_64_HI_LO)
		hi_lo_writeq(erst_base, &ir->ir_set->erst_base);
	else
		xhci_write_64(xhci, erst_base, &ir->ir_set->erst_base);

	/* Set the event ring dequeue address of this interrupter */
	xhci_set_hc_event_deq(xhci, ir);
}

struct xhci_interrupter *
xhci_create_secondary_interrupter(struct usb_hcd *hcd, unsigned int segs,
				  u32 imod_interval, unsigned int intr_num)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	struct xhci_interrupter *ir;
	unsigned int i;
	int err = -ENOSPC;

	if (!xhci->interrupters || xhci->max_interrupters <= 1 ||
	    intr_num >= xhci->max_interrupters)
		return NULL;

	ir = xhci_alloc_interrupter(xhci, segs, GFP_KERNEL);
	if (!ir)
		return NULL;

	spin_lock_irq(&xhci->lock);
	if (!intr_num) {
		/* Find available secondary interrupter, interrupter 0 is reserved for primary */
		for (i = 1; i < xhci->max_interrupters; i++) {
			if (!xhci->interrupters[i]) {
				xhci->interrupters[i] = ir;
				xhci_add_interrupter(xhci, i);
				err = 0;
				break;
			}
		}
	} else {
		if (!xhci->interrupters[intr_num]) {
			xhci->interrupters[intr_num] = ir;
			xhci_add_interrupter(xhci, intr_num);
			err = 0;
		}
	}
	spin_unlock_irq(&xhci->lock);

	if (err) {
		xhci_warn(xhci, "Failed to add secondary interrupter, max interrupters %d\n",
			  xhci->max_interrupters);
		xhci_free_interrupter(xhci, ir);
		return NULL;
	}

	xhci_set_interrupter_moderation(ir, imod_interval);

	xhci_dbg(xhci, "Add secondary interrupter %d, max interrupters %d\n",
		 ir->intr_num, xhci->max_interrupters);

	return ir;
}
EXPORT_SYMBOL_GPL(xhci_create_secondary_interrupter);

int xhci_mem_init(struct xhci_hcd *xhci, gfp_t flags)
{
	struct device	*dev = xhci_to_hcd(xhci)->self.sysdev;
	dma_addr_t	dma;

	/*
	 * xHCI section 5.4.6 - Device Context array must be
	 * "physically contiguous and 64-byte (cache line) aligned".
	 */
	xhci->dcbaa = dma_alloc_coherent(dev, sizeof(*xhci->dcbaa), &dma, flags);
	if (!xhci->dcbaa)
		goto fail;

	xhci->dcbaa->dma = dma;
	xhci_dbg_trace(xhci, trace_xhci_dbg_init,
		       "Device context base array address = 0x%pad (DMA), %p (virt)",
		       &xhci->dcbaa->dma, xhci->dcbaa);

	/*
	 * Initialize the ring segment pool.  The ring must be a contiguous
	 * structure comprised of TRBs.  The TRBs must be 16 byte aligned,
	 * however, the command ring segment needs 64-byte aligned segments
	 * and our use of dma addresses in the trb_address_map radix tree needs
	 * TRB_SEGMENT_SIZE alignment, so we pick the greater alignment need.
	 */
	if (xhci->quirks & XHCI_TRB_OVERFETCH)
		/* Buggy HC prefetches beyond segment bounds - allocate dummy space at the end */
		xhci->segment_pool = dma_pool_create("xHCI ring segments", dev,
				TRB_SEGMENT_SIZE * 2, TRB_SEGMENT_SIZE * 2, xhci->page_size * 2);
	else
		xhci->segment_pool = dma_pool_create("xHCI ring segments", dev,
				TRB_SEGMENT_SIZE, TRB_SEGMENT_SIZE, xhci->page_size);
	if (!xhci->segment_pool)
		goto fail;

	/* See Table 46 and Note on Figure 55 */
	xhci->device_pool = dma_pool_create("xHCI input/output contexts", dev, 2112, 64,
					    xhci->page_size);
	if (!xhci->device_pool)
		goto fail;

	/*
	 * Linear stream context arrays don't have any boundary restrictions,
	 * and only need to be 16-byte aligned.
	 */
	xhci->small_streams_pool = dma_pool_create("xHCI 256 byte stream ctx arrays",
						   dev, SMALL_STREAM_ARRAY_SIZE, 16, 0);
	if (!xhci->small_streams_pool)
		goto fail;

	/*
	 * Any stream context array bigger than MEDIUM_STREAM_ARRAY_SIZE will be
	 * allocated with dma_alloc_coherent().
	 */

	xhci->medium_streams_pool = dma_pool_create("xHCI 1KB stream ctx arrays",
						    dev, MEDIUM_STREAM_ARRAY_SIZE, 16, 0);
	if (!xhci->medium_streams_pool)
		goto fail;

	/*
	 * refer to xhci rev1_2 protocol 5.3.3 max ports is 255.
	 * refer to xhci rev1_2 protocol 6.4.3.14 port bandwidth buffer need
	 * to be 16-byte aligned.
	 */
	xhci->port_bw_pool = dma_pool_create("xHCI 256 port bw ctx arrays",
					     dev, GET_PORT_BW_ARRAY_SIZE, 16, 0);
	if (!xhci->port_bw_pool)
		goto fail;

	/* Set up the command ring to have one segments for now. */
	xhci->cmd_ring = xhci_ring_alloc(xhci, 1, TYPE_COMMAND, 0, flags);
	if (!xhci->cmd_ring)
		goto fail;

	xhci_dbg_trace(xhci, trace_xhci_dbg_init, "Allocated command ring at %p", xhci->cmd_ring);
	xhci_dbg_trace(xhci, trace_xhci_dbg_init, "First segment DMA is 0x%pad",
		       &xhci->cmd_ring->first_seg->dma);

	/*
	 * Reserve one command ring TRB for disabling LPM.
	 * Since the USB core grabs the shared usb_bus bandwidth mutex before
	 * disabling LPM, we only need to reserve one TRB for all devices.
	 */
	xhci->cmd_ring_reserved_trbs++;

	/* Allocate and set up primary interrupter 0 with an event ring. */
	xhci_dbg_trace(xhci, trace_xhci_dbg_init, "Allocating primary event ring");
	xhci->interrupters = kcalloc_node(xhci->max_interrupters, sizeof(*xhci->interrupters),
					  flags, dev_to_node(dev));
	if (!xhci->interrupters)
		goto fail;

	xhci->interrupters[0] = xhci_alloc_interrupter(xhci, 0, flags);
	if (!xhci->interrupters[0])
		goto fail;

	if (scratchpad_alloc(xhci, flags))
		goto fail;

	if (xhci_setup_port_arrays(xhci, flags))
		goto fail;

	return 0;

fail:
	xhci_halt(xhci);
	xhci_reset(xhci, XHCI_RESET_SHORT_USEC);
	xhci_mem_cleanup(xhci);
	return -ENOMEM;
}
