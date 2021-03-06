/*
 * Copyright (c) 2007-2011, 2017, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstrasse 6, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef E10K_QUEUE_H_
#define E10K_QUEUE_H_

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <net_interfaces/flags.h>

#include <net_interfaces/net_interfaces.h>
#include <devif/queue_interface.h>
#include <devif/queue_interface_backend.h>
#include <devif/backends/net/e10k_devif.h>
#include <dev/e10k_dev.h>
#include <dev/e10k_q_dev.h>
#include <machine/atomic.h>

//#define BENCH_QUEUE 1

#ifdef BENCH_QUEUE
#define BENCH_SIZE 100000
#include <bench/bench.h>
#endif

struct e10k_queue_ops {
    errval_t (*update_txtail)(struct e10k_queue*, size_t);
    errval_t (*update_rxtail)(struct e10k_queue*, size_t);
};

/**
 * Context structure for RX descriptors. This is needed to implement RSC, since
 * we need to be able to chain buffers together. */
struct e10k_queue_rxctx {
    struct devq_buf         buf;
    struct e10k_queue_rxctx *previous;
    bool                    used;
};


struct region_entry {
    uint32_t rid;
    struct dmem mem;
    struct region_entry* next;
};

struct e10k_queue {
    struct devq q;

#ifdef BENCH_QUEUE
    struct bench_ctl en_tx;
    struct bench_ctl en_rx;
    struct bench_ctl deq_rx;
    struct bench_ctl deq_tx;
#endif

    // registers
    void* d;
    struct capref                   regs;
    struct capref                   filter_ep; // EP to add filter
    struct dmem                     reg_mem;

    // queue info
    bool enabled;
    uint16_t id;
    uint32_t rsbufsz;
    bool use_vf; // use VF for this queue
    bool use_rsc; // Receive Side Coalescing
    bool use_vtd; // Virtual addressing (required for VF)
    bool use_rxctx; // 
    bool use_txhwb; //
    bool use_msix;
    size_t rxbufsz;
    uint8_t pci_function; 
    uint64_t mac;

    // registered regions
    struct region_entry* regions;   

    // interrupt
    bool use_irq;
    // callback 
    e10k_event_cb_t cb;
    

    // memory caps
    struct capref                   rx_frame;
    struct capref                   tx_frame;
    struct capref                   txhwb_frame;
    struct dmem                     tx;
    struct dmem                     txhwb;
    struct dmem                     rx;
    size_t rx_ring_size;
    size_t tx_ring_size;
          
    // vf state
    struct vf_state* vf;

    // Communicatio to PF
    struct e10k_vf_binding *binding;
    bool bound;

    // FIXME: Look for appropriate type for the _head/tail/size fields
    e10k_q_tdesc_adv_wb_array_t*    tx_ring;
    struct devq_buf*                tx_bufs;
    bool*                           tx_isctx;
    size_t                          tx_head;
    size_t                          tx_tail, tx_lasttail;
    size_t                          tx_size;
    void*                           tx_hwb;

    e10k_q_rdesc_adv_wb_array_t*    rx_ring;
    struct devq_buf*                rx_bufs;
    struct e10k_queue_rxctx*        rx_context;
    size_t                          rx_head;
    size_t                          rx_tail;
    size_t                          rx_size;

    struct e10k_queue_ops           ops;
    void*                           opaque;
    
};

typedef struct e10k_queue e10k_queue_t;

// Does not initalize the queue struct itself
static inline void e10k_queue_init(struct e10k_queue* q, void* tx, size_t tx_size,
                                   uint32_t* tx_hwb, void* rx, size_t rx_size, 
                                   struct e10k_queue_ops* ops)
{
    q->tx_ring = tx;
    q->tx_bufs = calloc(tx_size, sizeof(struct devq_buf));
    q->tx_isctx = calloc(tx_size, sizeof(bool));
    q->tx_head = 0;
    q->tx_tail = q->tx_lasttail = 0;
    q->tx_size = tx_size;
    q->tx_hwb = tx_hwb;

    q->rx_ring = rx;
    q->rx_bufs = calloc(rx_size, sizeof(struct devq_buf));
    q->rx_context = calloc(rx_size, sizeof(*q->rx_context));
    q->rx_head = 0;
    q->rx_tail = 0;
    q->rx_size = rx_size;

    q->ops = *ops;

    // Initialize ring memory with zero
    memset(tx, 0, tx_size * e10k_q_tdesc_adv_wb_size);
    memset(rx, 0, rx_size * e10k_q_rdesc_adv_wb_size);
    memset(q->tx_isctx, 0, tx_size*sizeof(bool));
    memset(q->rx_context, 0, tx_size*sizeof(*q->rx_context));

#ifdef BENCH_QUEUE
    q->en_tx.mode = BENCH_MODE_FIXEDRUNS;
    q->en_tx.result_dimensions = 1;
    q->en_tx.min_runs = BENCH_SIZE;
    q->en_tx.data = calloc(q->en_tx.min_runs * q->en_tx.result_dimensions,
                       sizeof(*q->en_tx.data));
    assert(q->en_tx.data != NULL);

    q->en_rx.mode = BENCH_MODE_FIXEDRUNS;
    q->en_rx.result_dimensions = 1;
    q->en_rx.min_runs = BENCH_SIZE;
    q->en_rx.data = calloc(q->en_rx.min_runs * q->en_rx.result_dimensions,
                       sizeof(*q->en_rx.data));
    assert(q->en_rx.data != NULL);

    q->deq_rx.mode = BENCH_MODE_FIXEDRUNS;
    q->deq_rx.result_dimensions = 1;
    q->deq_rx.min_runs = BENCH_SIZE;
    q->deq_rx.data = calloc(q->deq_rx.min_runs * q->deq_rx.result_dimensions,
                       sizeof(*q->deq_rx.data));
    assert(q->deq_rx.data != NULL);

    q->deq_tx.mode = BENCH_MODE_FIXEDRUNS;
    q->deq_tx.result_dimensions = 1;
    q->deq_tx.min_runs = BENCH_SIZE;
    q->deq_tx.data = calloc(q->deq_tx.min_runs * q->deq_tx.result_dimensions,
                       sizeof(*q->deq_tx.data));

    assert(q->deq_tx.data != NULL);
#endif

}

static inline int e10k_queue_add_txcontext(e10k_queue_t* q, uint8_t idx,
                                           uint8_t maclen, uint16_t iplen, 
                                           uint8_t l4len, e10k_q_l4_type_t l4t)
{
    e10k_q_tdesc_adv_ctx_t d;
    size_t tail = q->tx_tail;

    memset(q->tx_ring[tail], 0, e10k_q_tdesc_adv_wb_size);

    // TODO: Check if there is room in the queue
    q->tx_isctx[tail] = true;
    d = q->tx_ring[tail];

    e10k_q_tdesc_adv_rd_dtyp_insert(d, e10k_q_adv_ctx);
    e10k_q_tdesc_adv_rd_dext_insert(d, 1);

    /* e10k_q_tdesc_adv_ctx_bcntlen_insert(d, 0x3f); */
    e10k_q_tdesc_adv_ctx_idx_insert(d, idx);
    e10k_q_tdesc_adv_ctx_maclen_insert(d, maclen);
    e10k_q_tdesc_adv_ctx_iplen_insert(d, iplen);
    e10k_q_tdesc_adv_ctx_ipv4_insert(d, 1);
    e10k_q_tdesc_adv_ctx_l4len_insert(d, l4len);
    e10k_q_tdesc_adv_ctx_l4t_insert(d, l4t);

    q->tx_lasttail = q->tx_tail;
    q->tx_tail = (tail + 1) % q->tx_size;
    return 0;
}

// len is only length of this descriptor where length is the total length
static inline int e10k_queue_add_txbuf_ctx(e10k_queue_t* q, lpaddr_t phys,
                                           regionid_t rid,
                                           genoffset_t offset,
                                           genoffset_t length,
                                           genoffset_t valid_data, 
                                           genoffset_t valid_length,
                                           uint64_t flags,
                                           size_t len, uint8_t ctx,
                                           bool ixsm, bool txsm)
{
    e10k_q_tdesc_adv_rd_t d;
    size_t tail = q->tx_tail;

    memset(q->tx_ring[tail], 0, e10k_q_tdesc_adv_wb_size);

    // TODO: Check if there is room in the queue
    q->tx_isctx[tail] = false;
    struct devq_buf* buf = &q->tx_bufs[tail];
    buf->rid = rid;
    buf->offset = offset;
    buf->length = length;
    buf->valid_data = valid_data;
    buf->valid_length = valid_length;
    buf->flags = flags;  
    d = q->tx_ring[tail];

    e10k_q_tdesc_adv_rd_buffer_insert(d, phys);
    e10k_q_tdesc_adv_rd_dtalen_insert(d, valid_length);
    e10k_q_tdesc_adv_rd_paylen_insert(d, valid_length);

    // TODO use flags of devq interface to set eop and rs
    e10k_q_tdesc_adv_rd_dtyp_insert(d, e10k_q_adv_data);
    e10k_q_tdesc_adv_rd_dext_insert(d, 1);
    e10k_q_tdesc_adv_rd_rs_insert(d, 1);
    e10k_q_tdesc_adv_rd_ifcs_insert(d, 1);
    e10k_q_tdesc_adv_rd_eop_insert(d, 1);

    if (ctx != (uint8_t)-1) {
        e10k_q_tdesc_adv_rd_idx_insert(d, ctx);
        e10k_q_tdesc_adv_rd_cc_insert(d, 1);
        e10k_q_tdesc_adv_rd_ixsm_insert(d, ixsm);
        e10k_q_tdesc_adv_rd_txsm_insert(d, txsm);
    }

    q->tx_lasttail = q->tx_tail;
    q->tx_tail = (tail + 1) % q->tx_size;
    return 0;

}


static inline int e10k_queue_add_txbuf_legacy(e10k_queue_t* q, lpaddr_t phys,
                                       regionid_t rid,
                                       genoffset_t offset,
                                       genoffset_t length,      
                                       genoffset_t valid_data,
                                       genoffset_t valid_length,
                                       uint64_t flags,
                                       size_t len)
{
    size_t tail = q->tx_tail;


    struct devq_buf* buf = &q->tx_bufs[tail];
    buf->rid = rid;
    buf->offset = offset;
    buf->length = length;
    buf->valid_data = valid_data;
    buf->valid_length = valid_length;
    buf->flags = flags;  

#ifdef BENCH_QUEUE
    uint64_t start, end;
    start = rdtscp();
#endif

    e10k_q_tdesc_legacy_t d;
    d = q->tx_ring[tail];

    e10k_q_tdesc_legacy_buffer_insert(d, phys);
    e10k_q_tdesc_legacy_length_insert(d, len);
    // OPTIMIZATION: Maybe only set rs on last packet?
    bool last = flags & NETIF_TXFLAG_LAST;
    e10k_q_tdesc_legacy_rs_insert(d, last);
    e10k_q_tdesc_legacy_ifcs_insert(d,  1);
    e10k_q_tdesc_legacy_eop_insert(d, last);

#ifdef BENCH_QUEUE
    end = rdtscp();
    uint64_t res = end - start;
    bench_ctl_add_run(&q->en_tx, &res);
#endif
    __sync_synchronize();

    q->tx_tail = (tail + 1) % q->tx_size;
    return 0;
}

static inline int e10k_queue_add_txbuf(e10k_queue_t* q, lpaddr_t phys,
                                       regionid_t rid,
                                       genoffset_t offset,
                                       genoffset_t length,      
                                       genoffset_t valid_data,
                                       genoffset_t valid_length,
                                       uint64_t flags,
                                       size_t len)
{
    if(!q->use_vf) {
    /*
        return e10k_queue_add_txbuf_legacy(q, phys, rid, offset, length,
                                    valid_data, valid_length, 
                                    flags, len);
    */
        return e10k_queue_add_txbuf_ctx(q, phys, rid, offset, length,
                                    valid_data, valid_length, 
                                    flags, len, -1, false, false);
    } else {
        // TODO try generate checksums
        return e10k_queue_add_txbuf_ctx(q, phys, rid, offset, length,
                                    valid_data, valid_length, 
                                    flags, len, -1, false, false);
    }
}

/*
 * Reclaim 1 packet from the TX queue once it's handled by the
 * card. Call multiple times to reclaim more packets.
 *
 * \param       q       Queue to check
 * \param       opaque  Contains opaque data of reclaimed packet, if any
 *
 * \return true if packet can be reclaimed otherwise false
 */
static inline bool e10k_queue_get_txbuf_avd(e10k_queue_t* q, regionid_t* rid,   
                                        genoffset_t* offset,
                                        genoffset_t* length,
                                        genoffset_t* valid_data,
                                        genoffset_t* valid_length,
                                        uint64_t* flags)
{
    /* e10k_q_tdesc_adv_wb_t d; */
    size_t head = q->tx_head;
    bool result = false;
    // If HWB is enabled, we can skip reading the descriptor if nothing happened
    
    if (q->tx_hwb && *((uint32_t*)q->tx_hwb) == head) {
        return false;
    }

    if(!q->tx_hwb) {
        size_t idx = head;

        // Skip over context and non-EOP descriptors
        while(idx != q->tx_tail && q->tx_isctx[idx] && 
              !e10k_q_tdesc_adv_wb_dd_extract(q->tx_ring[idx])) {
            idx = (idx + 1) % q->tx_size;
        }

        if(idx == q->tx_tail) {
            return false;
        }
    }

    // That last packet got written out, now go reclaim from the head pointer.
    if (!q->tx_isctx[head]) {
        *rid = q->tx_bufs[head].rid;
        *offset = q->tx_bufs[head].offset;
        *length = q->tx_bufs[head].length;
        *valid_data = q->tx_bufs[head].valid_data;
        *valid_length = q->tx_bufs[head].valid_length;
        *flags = q->tx_bufs[head].flags;

        result = true;
    }

    /* memset(q->tx_ring[head], 0, e10k_q_tdesc_adv_wb_size); */
    q->tx_head = (head + 1) % q->tx_size;
    return result;
}

static inline bool e10k_queue_get_txbuf_legacy(e10k_queue_t* q, regionid_t* rid,   
                                        genoffset_t* offset,
                                        genoffset_t* length,
                                        genoffset_t* valid_data,
                                        genoffset_t* valid_length,
                                        uint64_t* flags)
{
#ifdef BENCH_QUEUE
    uint64_t start, end;
    start = rdtscp();
#endif

    e10k_q_tdesc_legacy_t d;
    size_t head = q->tx_head;

    d = q->tx_ring[head];
    if (e10k_q_tdesc_legacy_dd_extract(d)) {
        *rid = q->tx_bufs[head].rid;
        *offset = q->tx_bufs[head].offset;
        *length = q->tx_bufs[head].length;
        *valid_data = q->tx_bufs[head].valid_data;
        *valid_length = q->tx_bufs[head].valid_length;
        *flags = q->tx_bufs[head].flags;
        memset(d, 0, e10k_q_tdesc_legacy_size);

        q->tx_head = (head + 1) % q->tx_size;

#ifdef BENCH_QUEUE
        end = rdtscp();
        uint64_t res = end - start;
        bench_ctl_add_run(&q->deq_tx, &res);
#endif
        return true;
    } 

    if (q->tx_hwb) {
        head = *((uint32_t*) q->tx_hwb);
        if (q->tx_head == head) {
            return false;
        } else {
            *rid = q->tx_bufs[q->tx_head].rid;
            *offset = q->tx_bufs[q->tx_head].offset;
            *length = q->tx_bufs[q->tx_head].length;
            *valid_data = q->tx_bufs[q->tx_head].valid_data;
            *valid_length = q->tx_bufs[q->tx_head].valid_length;
            *flags = q->tx_bufs[q->tx_head].flags;
            memset(d, 0, e10k_q_tdesc_legacy_size);

            q->tx_head = (q->tx_head + 1) % q->tx_size;

#ifdef BENCH_QUEUE
            end = rdtscp();
            uint64_t res = end - start;
            bench_ctl_add_run(&q->deq_tx, &res);
#endif
            return true;
        }
    }

    return false;
}

static inline bool e10k_queue_get_txbuf(e10k_queue_t* q, regionid_t* rid,   
                                        genoffset_t* offset,
                                        genoffset_t* length,
                                        genoffset_t* valid_data,
                                        genoffset_t* valid_length,
                                        uint64_t* flags)
{
    if(!q->use_vf) {
        /*
        return e10k_queue_get_txbuf_legacy(q, rid, offset, length, valid_data, 
                                           valid_length, flags);
        */
        return e10k_queue_get_txbuf_avd(q, rid, offset, length, valid_data, 
                                        valid_length, flags);
    } else {
        return e10k_queue_get_txbuf_avd(q, rid, offset, length, valid_data, 
                                        valid_length, flags);
    }
}

static inline errval_t e10k_queue_bump_txtail(e10k_queue_t* q)
{
    return q->ops.update_txtail(q, q->tx_tail);
}

static inline size_t e10k_queue_free_txslots(e10k_queue_t* q)
{
    size_t head = q->tx_head;
    size_t tail = q->tx_tail;
    size_t size = q->tx_size;

    if (tail >= head) {
        return size - (tail - head) - 1; // TODO: could this be off by 1?
    } else {
        return size - (tail + size - head) - 1; // TODO: off by 1?
    }

}

static inline int e10k_queue_add_rxbuf_adv(e10k_queue_t* q,
                                       lpaddr_t phys,
                                       regionid_t rid,
                                       genoffset_t offset,
                                       genoffset_t length,
                                       genoffset_t valid_data,
                                       genoffset_t valid_length,
                                       uint64_t flags)
{
    size_t tail = q->rx_tail;
    struct e10k_queue_rxctx *ctx;

    ctx = q->rx_context + tail;
    if (ctx->used) {
        printf("e10k: Already used!\n");
        return 1;
    }

    // TODO: Check if there is room in the queue
    ctx->buf.rid = rid;
    ctx->buf.offset = offset;
    ctx->buf.length = length;
    ctx->buf.valid_data = valid_data;
    ctx->buf.valid_length = valid_length;
    ctx->buf.flags = flags;
    ctx->used = true;

#ifdef BENCH_QUEUE
    uint64_t start, end;
    start = rdtscp();
#endif
    e10k_q_rdesc_adv_rd_t d;
    d = (e10k_q_rdesc_adv_rd_t) q->rx_ring[tail];

    e10k_q_rdesc_adv_rd_buffer_insert(d, phys);
    // TODO: Does this make sense for RSC?
    e10k_q_rdesc_adv_rd_hdr_buffer_insert(d, 0);

    __sync_synchronize();

    q->rx_tail = (tail + 1) % q->rx_size;

#ifdef BENCH_QUEUE
    end = rdtscp();
    uint64_t res = end - start;
    bench_ctl_add_run(&q->en_rx, &res);
#endif

    return 0;
}

static inline int e10k_queue_add_rxbuf_legacy(e10k_queue_t* q,
                                       lpaddr_t phys,
                                       regionid_t rid,
                                       genoffset_t offset,
                                       genoffset_t length,
                                       genoffset_t valid_data,
                                       genoffset_t valid_length,
                                       uint64_t flags)
{
    size_t tail = q->rx_tail;


    struct devq_buf* buf = &q->rx_bufs[tail];
    buf->rid = rid;
    buf->offset = offset;
    buf->length = length;
    buf->valid_data = valid_data;
    buf->valid_length = valid_length;
    buf->flags = flags;  
    
#ifdef BENCH_QUEUE
    uint64_t start, end;
    start = rdtscp();
#endif

    e10k_q_rdesc_legacy_t d;

    d = q->rx_ring[tail];
    e10k_q_rdesc_legacy_buffer_insert(d, phys);

    __sync_synchronize();

    q->rx_tail = (tail + 1) % q->rx_size;

#ifdef BENCH_QUEUE
    end = rdtscp();
    uint64_t res = end - start;
    bench_ctl_add_run(&q->en_rx, &res);
#endif
    return 0;
}


static inline int e10k_queue_add_rxbuf(e10k_queue_t* q,
                                       lpaddr_t phys,
                                       regionid_t rid,
                                       genoffset_t offset,
                                       genoffset_t length,
                                       genoffset_t valid_data,
                                       genoffset_t valid_length,
                                       uint64_t flags)
{
    if(!q->use_vf) {
    /*
        return e10k_queue_add_rxbuf_legacy(q, phys, rid, offset, length, valid_data,
                                           valid_length, flags);
    */
        return e10k_queue_add_rxbuf_adv(q, phys, rid, offset, length, valid_data,
                                        valid_length, flags);
    } else {
        return e10k_queue_add_rxbuf_adv(q, phys, rid, offset, length, valid_data,
                                        valid_length, flags);
    }
}
static inline uint64_t e10k_queue_convert_rxflags(e10k_q_rdesc_adv_wb_t d)
{
    uint64_t flags = 0;

    // IP checksum
    if (e10k_q_rdesc_adv_wb_ipcs_extract(d)) {
        flags |= NETIF_RXFLAG_IPCHECKSUM;
        if (!e10k_q_rdesc_adv_wb_ipe_extract(d)) {
            flags |= NETIF_RXFLAG_IPCHECKSUM_GOOD;
        }
    }

    // L4 checksum
    if (e10k_q_rdesc_adv_wb_l4i_extract(d)) {
        flags |= NETIF_RXFLAG_L4CHECKSUM;
        if (!e10k_q_rdesc_adv_wb_l4e_extract(d)) {
            flags |= NETIF_RXFLAG_L4CHECKSUM_GOOD;
        }
    }

    // Packet type
    if (e10k_q_rdesc_adv_wb_pt_ipv4_extract(d)) {
        flags |= NETIF_RXFLAG_TYPE_IPV4;
    }
    if (e10k_q_rdesc_adv_wb_pt_tcp_extract(d)) {
        flags |= NETIF_RXFLAG_TYPE_TCP;
    }
    if (e10k_q_rdesc_adv_wb_pt_udp_extract(d)) {
        flags |= NETIF_RXFLAG_TYPE_UDP;
    }

    return flags;
}

static inline bool e10k_queue_get_rxbuf_avd(e10k_queue_t* q, regionid_t* rid,
                                        genoffset_t* offset,
                                        genoffset_t* length,
                                        genoffset_t* valid_data,
                                        genoffset_t* valid_length,
                                        uint64_t* flags,
                                        int* last)
{
    e10k_q_rdesc_adv_wb_t d;
    size_t head = q->rx_head;
    struct e10k_queue_rxctx *ctx;

    d = q->rx_ring[head];
    ctx = q->rx_context + head;

    if (!e10k_q_rdesc_adv_wb_dd_extract(d)) {
        return false;
    }

    // Barrier needed according to linux driver to make sure nothing else is
    // read before the dd bit TODO: make sure
    rmb();

    // TODO add code for RSC

    *flags = ctx->buf.flags;
    // Set flags if it this is a descriptor with EOP
    // TODO: with multi-part packets, we want these flags on the first packet
    if (e10k_q_rdesc_adv_wb_eop_extract(d)) {
        *flags = *flags | e10k_queue_convert_rxflags(d);
    }

    // TODO: Extract status (okay/error)
    *last = e10k_q_rdesc_adv_wb_eop_extract(d);
    *valid_length = e10k_q_rdesc_adv_wb_pkt_len_extract(d);
    *rid = ctx->buf.rid;
    *offset = ctx->buf.offset;
    *length = ctx->buf.length;
    *valid_data = ctx->buf.valid_data;

    ctx->used = false;
    memset(d, 0, e10k_q_rdesc_adv_wb_size);

    q->rx_head = (head + 1) % q->rx_size;
    return true;
}


static inline bool e10k_queue_get_rxbuf_legacy(e10k_queue_t* q, regionid_t* rid,
                                        genoffset_t* offset,
                                        genoffset_t* length,
                                        genoffset_t* valid_data,
                                        genoffset_t* valid_length,
                                        uint64_t* flags,
                                        int* last)
{
#ifdef BENCH_QUEUE
    uint64_t start, end;
    start = rdtscp();
#endif

    e10k_q_rdesc_legacy_t d;
    size_t head = q->rx_head;
    struct devq_buf* buf = &q->rx_bufs[head];

    d = q->rx_ring[head];
    if (e10k_q_rdesc_legacy_dd_extract(d)) {
        *last = e10k_q_rdesc_legacy_eop_extract(d);
        *valid_length = e10k_q_rdesc_legacy_length_extract(d);

        *rid = buf->rid;
        *offset = buf->offset;
        *length = buf->length;
        *valid_data = buf->valid_data;
        *flags = buf->flags;

        memset(d, 0, e10k_q_rdesc_legacy_size);

        q->rx_head = (head + 1) % q->rx_size;
#ifdef BENCH_QUEUE
        end = rdtscp();
        uint64_t res = end - start;
        bench_ctl_add_run(&q->deq_rx, &res);
#endif
        return true;
    } else {
        return false;
    }
}


static inline bool e10k_queue_get_rxbuf(e10k_queue_t* q, regionid_t* rid,
                                        genoffset_t* offset,
                                        genoffset_t* length,
                                        genoffset_t* valid_data,
                                        genoffset_t* valid_length,
                                        uint64_t* flags,
                                        int* last)
{
    if(!q->use_vf) {
    /*
       return e10k_queue_get_rxbuf_legacy(q, rid, offset, length, valid_data, valid_length,
                                    flags, last);
    */
       return e10k_queue_get_rxbuf_avd(q, rid, offset, length, valid_data, valid_length,
                                       flags, last);
    } else {
       return e10k_queue_get_rxbuf_avd(q, rid, offset, length, valid_data, valid_length,
                                       flags, last);
    }
}

static inline errval_t e10k_queue_bump_rxtail(e10k_queue_t* q)
{
    return q->ops.update_rxtail(q, q->rx_tail);
}

static inline size_t e10k_queue_free_rxslots(e10k_queue_t* q)
{
    size_t head = q->rx_head;
    size_t tail = q->rx_tail;
    size_t size = q->rx_size;

    if (tail >= head) {
        return size - (tail - head) - 1; // TODO: could this be off by 1?
    } else {
        return size - (tail + size - head) - 1; // TODO: off by 1?
    }
}

static inline struct bench_ctl* e10k_queue_get_benchmark_data(e10k_queue_t* q, uint8_t type)
{
#ifdef BENCH_QUEUE
    switch (type) {
        case 0:
            return &q->en_rx;
        case 1:
            return &q->en_tx;
        case 2:
            return &q->deq_rx;
        case 3:
            return &q->deq_tx;
        default:
            return NULL;
    }
#endif
    return NULL;
}

#endif // ndef E10K_QUEUE_H_
