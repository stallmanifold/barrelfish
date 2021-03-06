/**
 * \file
 * \brief Driverkit module implementation.
 *
 * Contians helper functions to iterate over driver modules in a domain
 * and create driver instances from driver modules.
 */
/*
 * Copyright (c) 2016, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstrasse 6, CH-8092 Zurich. Attn: Systems Group.
 */
#include <stdlib.h>

#include <barrelfish/barrelfish.h>
#include <driverkit/driverkit.h>
#include <driverkit/iommu.h>
#include <driverkit/hwmodel.h>
#include <collections/hash_table.h>
#include <skb/skb.h>
#include <if/mem_defs.h>
#include "debug.h"
#include "../libc/include/namespace.h"


__attribute__((unused))
static void format_nodelist(int32_t *nodes, char *out){
    *out = '\0';
    sprintf(out + strlen(out), "[");
    int first = 1;
    while(*nodes != 0){
        if(!first) sprintf(out + strlen(out), ",");
        sprintf(out + strlen(out), "%" PRIi32, *nodes);
        nodes++;
        first = 0;
    }
    sprintf(out + strlen(out), "]");
}

void driverkit_parse_namelist(char *in, struct hwmodel_name *names, int *conversions){
    assert(in);
    *conversions = 0;
    struct list_parser_status status;
    skb_read_list_init_offset(&status, in, 0);
    while(skb_read_list(&status, "name(%"SCNu64", %"SCNi32")",
                &names->address, &names->nodeid)) {
        debug_printf("parse_namelist: %lx\n", names->address);
        names++;
        *conversions += 1;
    }
}

#define ALLOC_WRAP_Q "state_get(S)," \
                     "alloc_wrap(S, %zu, %d, %"PRIi32",%s, NewS)," \
                     "state_set(NewS)."

errval_t
driverkit_hwmodel_allocate(size_t bytes, int32_t dstnode, int32_t * nodes,
                           uint8_t alloc_bits, genpaddr_t *retaddr) {
    errval_t err;

    char nodes_str[128];
    format_nodelist(nodes, nodes_str);
    HWMODEL_QUERY_DEBUG(ALLOC_WRAP_Q, bytes, alloc_bits, dstnode, nodes_str);
    err = skb_execute_query(ALLOC_WRAP_Q, bytes, alloc_bits, dstnode, nodes_str);
    if (err_is_fail(err)) {
        DEBUG_SKB_ERR(err, "failed to query\n");
        return err;
    }

    struct hwmodel_name names[1];
    int num_conversions = 0;
    driverkit_parse_namelist(skb_get_output(), names, &num_conversions);
    assert(num_conversions == 1);


    if (retaddr) {
        *retaddr = names[0].address;
    }

    return SYS_ERR_OK;

}

errval_t driverkit_hwmodel_ram_alloc(struct capref *dst,
                                     size_t bytes, int32_t dstnode,
                                     int32_t *nodes)
{

    if (bytes < (LARGE_PAGE_SIZE)) {
        bytes = LARGE_PAGE_SIZE;
    }


    int bits = log2ceil(bytes);
    bytes = 1 << bits;
    assert(bits >= 21);
    // The PT configuration in the SKB is currently using 2M pages.

#ifdef DISABLE_MODEL
    if (dstnode != driverkit_hwmodel_lookup_dram_node_id()) {
        return LIB_ERR_RAM_ALLOC_MS_CONSTRAINTS;
    }
    return ram_alloc(dst, bits);
#else
    errval_t err;
    errval_t msgerr;
    genpaddr_t addr;
    err = driverkit_hwmodel_allocate(bytes, dstnode, nodes, bits, &addr);
    if(err_is_fail(err)) {
        return err;
    }

    // Alloc cap slot
    err = slot_alloc(dst);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SLOT_ALLOC);
    }


    struct mem_binding * b = get_mem_client();
    debug_printf("Determined addr=0x%"PRIx64" as address for (nodeid=%d, size=%zu) request\n",
            addr, dstnode, bytes);

    err = b->rpc_tx_vtbl.allocate(b, bits, addr, addr + bytes,
            &msgerr, dst);
    if(err_is_fail(err)){
        DEBUG_ERR(err, "allocate RPC");
        return err;
    }
    if(err_is_fail(msgerr)){
        DEBUG_ERR(msgerr, "allocate");
        return msgerr;
    }
    return SYS_ERR_OK;

#endif
}

errval_t driverkit_hwmodel_frame_alloc(struct capref *dst,
                                       size_t bytes, int32_t dstnode,
                                       int32_t *nodes)
{
#ifdef DISABLE_MODEL
    if (dstnode != driverkit_hwmodel_lookup_dram_node_id()) {
        return LIB_ERR_RAM_ALLOC_MS_CONSTRAINTS;
    }
    return frame_alloc(dst, bytes, NULL);
#else
    errval_t err;
    struct capref ram_cap; 

    if(bytes < LARGE_PAGE_SIZE) bytes = LARGE_PAGE_SIZE;

    // Allocate RAM cap
    err = driverkit_hwmodel_ram_alloc(&ram_cap, bytes, dstnode, nodes);
    if(err_is_fail(err)){
        return err;            
    }

    // Alloc cap slot
    err = slot_alloc(dst);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SLOT_ALLOC);
    }

    // Get bits
    assert(bytes > 0);
    uint8_t bits = log2ceil(bytes);
    assert((1UL << bits) >= bytes);

    // This is doing what "create_ram_descendant" in
    // lib/barrelfish/capabilities.c is doing.
    err = cap_retype(*dst, ram_cap, 0, ObjType_Frame, (1UL << bits), 1);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_CAP_RETYPE);
    }

    err = cap_destroy(ram_cap);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_CAP_DESTROY);
    }

    return SYS_ERR_OK;
#endif
}




/**
 * fills in dmem->vbase + maps frame
 */
errval_t driverkit_hwmodel_vspace_map(int32_t nodeid, struct capref frame,
                                      vregion_flags_t flags, struct dmem *dmem)
{

#ifdef DISABLE_MODEL
    return SYS_ERR_OK;
#else
    errval_t err;
    struct frame_identity id;
    err = frame_identify(frame, &id);
    if (err_is_fail(err)) {
        return err;
    }

    char conf_buf[512];

    dmem->mem = frame;
    dmem->size = id.bytes;
    dmem->devaddr = id.base;

    // Alloc space in my vspace
    assert(nodeid == driverkit_hwmodel_get_my_node_id());
    err = driverkit_hwmodel_get_map_conf(frame, nodeid, conf_buf, sizeof(conf_buf),
                                         &dmem->vbase);
    if(err_is_fail(err)) {
        DEBUG_ERR(err, "vspace_map local");
        return err;
    }

    uint64_t inaddr, outaddr;
    int32_t conf_nodeid;
    struct list_parser_status status;
    skb_read_list_init_offset(&status, conf_buf, 0);
    while(skb_read_list(&status, "c(%"SCNi32", %"SCNu64", %"SCNu64")",
                        &conf_nodeid, &inaddr, &outaddr)) {
        debug_printf("%s:%u %i, %i, inaddr=%lx, vbase=%lx\n", __FUNCTION__, __LINE__,
                      nodeid, conf_nodeid, inaddr, dmem->vbase);

        err = driverkit_hwmodel_vspace_map_fixed(nodeid, dmem->vbase, frame,
                                                  flags, dmem);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "TODO CLEANUP!");
            return err;
        }
    }

    return SYS_ERR_OK;
#endif

}

errval_t driverkit_hwmodel_vspace_map_fixed(int32_t nodeid,
                                            genvaddr_t addr,
                                            struct capref frame,
                                            vregion_flags_t flags,
                                            struct dmem *dmem)
{
    errval_t err;

    if(nodeid != driverkit_hwmodel_get_my_node_id()){
        return LIB_ERR_NOT_IMPLEMENTED;
    }

    struct frame_identity id;
    err = frame_identify(frame, &id);
    if (err_is_fail(err)) {
        return err;
    }

    dmem->vbase = addr;

    return vspace_map_one_frame_fixed_attr(addr, id.bytes, frame, flags, NULL, NULL);
}


#define MAP_WRAP_Q  "state_get(S)," \
                    "map_wrap(S, %zu, 21, %"PRIi32", %"PRIu64", %s, NewS)," \
                    "state_set(NewS)."

errval_t driverkit_hwmodel_vspace_alloc(struct capref frame,
                                        int32_t nodeid, genvaddr_t *addr)
{
    errval_t err;

    struct frame_identity id;
    err = frame_identify(frame, &id);
    if (err_is_fail(err)) {
        return err;
    }

    int32_t src_nodeid[2];
    char src_nodeid_str[128];
    src_nodeid[0] = nodeid;
    src_nodeid[1] = 0;
    format_nodelist(src_nodeid, src_nodeid_str);

    //int32_t mem_nodeid = id.pasid;
    int32_t mem_nodeid = driverkit_hwmodel_lookup_dram_node_id();
    uint64_t mem_addr = id.base;
    HWMODEL_QUERY_DEBUG(MAP_WRAP_Q,
            id.bytes, mem_nodeid, mem_addr, src_nodeid_str);
    err = skb_execute_query(MAP_WRAP_Q,
            id.bytes, mem_nodeid, mem_addr, src_nodeid_str);
    if(err_is_fail(err)){
        DEBUG_SKB_ERR(err, "map_wrap");
        return err;
    }
    
    struct hwmodel_name names[2];
    int num_conversions = 0;
    driverkit_parse_namelist(skb_get_output(), names, &num_conversions);
    assert(num_conversions == 2);
    //ignore, names[0] it is the resolved name as stored in frame
    *addr = names[1].address;
    debug_printf("Determined addr=0x%"PRIx64" as vbase for (nodeid=%d, size=%zu) request\n",
            *addr, nodeid, id.bytes);
    return SYS_ERR_OK;
}

/*
 *  Returns this process nodeid. It lazily adds the process' model node
 *  and returns it's identifier.
 */
int32_t driverkit_hwmodel_get_my_node_id(void)
{
    errval_t err;

    err = skb_client_connect();
    if (err_is_fail(err)) {
        return -1;
    }
    /*
     * XXX: this assumes the domain only runs on a single core!
     */
    static int32_t nodeid = -1;

    if(nodeid == -1){
        HWMODEL_QUERY_DEBUG(
            "state_get(S), "
            "add_process(S, E, NewS), writeln(E), "
            "state_set(NewS)");
        err = skb_execute_query(
            "state_get(S), "
            "add_process(S, E, NewS), writeln(E), "
            "state_set(NewS)");
        if (err_is_fail(err)) {
            DEBUG_SKB_ERR(err, "add_process");
            return -1;
        }
        err = skb_read_output("%d", &nodeid);
        assert(err_is_ok(err));
        DRIVERKIT_DEBUG("Instantiated new process model node, nodeid=%"PRIi32"\n",
                        nodeid);
    }
    return nodeid;
}

int32_t driverkit_hwmodel_lookup_dram_node_id(void)
{
#ifdef DISABLE_MODEL
    return 1;
#else
    return driverkit_hwmodel_lookup_node_id("[\"DRAM\"]");
#endif
}

int32_t driverkit_hwmodel_lookup_pcibus_node_id(void)
{
    return driverkit_hwmodel_lookup_node_id("[\"PCIBUS\"]");
}


int32_t driverkit_hwmodel_lookup_node_id(const char *path)
{

    debug_printf("%s:%u with path='%s'\n", __FUNCTION__, __LINE__, path);

    errval_t err;
    HWMODEL_QUERY_DEBUG(
        "node_enum(%s, E), writeln(E)",
        path);
    err = skb_execute_query(
        "node_enum(%s, E), writeln(E)",
        path);
    if (err_is_fail(err)) {
        DEBUG_SKB_ERR(err, "query node_enum");
    }
    int32_t nodeid;
    err = skb_read_output("%d", &nodeid);
    assert(err_is_ok(err));
    return nodeid;
}

#define REVERSE_RESOLVE_Q "state_get(S)," \
                    "reverse_resolve_wrap(S, %"PRIi32", %"PRIu64", %zu, %"PRIi32")."

#define FORMAT  "[\"KNC_SOCKET\", \"PCI0\", %d]"

// Without reconfiguration, under what ret_addr can you reach dst
// from nodeid?
errval_t driverkit_hwmodel_reverse_resolve(struct capref dst, int32_t nodeid,
                                           genpaddr_t *ret_addr)
{

    errval_t err;
    struct frame_identity id;
    err = frame_identify(dst, &id);
    if (err_is_fail(err)) {
        return err;
    }
    assert(ret_addr);
#ifdef DISABLE_MODEL
    *ret_addr = id.base;
    return SYS_ERR_OK;
#else
    int dst_enum = id.pasid;
    dst_enum = driverkit_hwmodel_lookup_pcibus_node_id();

    assert(nodeid < 100);
    char buf[sizeof(FORMAT)];
    snprintf(buf, sizeof(buf), FORMAT, nodeid);

    nodeid =  driverkit_hwmodel_lookup_node_id(buf);

    HWMODEL_QUERY_DEBUG(REVERSE_RESOLVE_Q, dst_enum, id.base, id.bytes, nodeid);
    err = skb_execute_query(REVERSE_RESOLVE_Q, dst_enum, id.base, id.bytes, nodeid);

    DEBUG_SKB_ERR(err, "reverse_resolve");
    if(err_is_fail(err)){
        DEBUG_SKB_ERR(err, "reverse_resolve");
        return err;
    }

    struct hwmodel_name names[1];
    int num_conversions = 0;
    driverkit_parse_namelist(skb_get_output(), names, &num_conversions);
    assert(num_conversions == 1);
    *ret_addr = names[0].address;

    debug_printf("Determined (0x%"PRIx64", %d) is alias of (0x%"PRIx64", %d)\n",
            names[0].address, nodeid, id.base, dst_enum);

    return SYS_ERR_OK;
#endif
}


#define MAP_WRAP_Q  "state_get(S)," \
                    "map_wrap(S, %zu, 21, %"PRIi32", %"PRIu64", %s, NewS)," \
                    "state_set(NewS)."

errval_t driverkit_hwmodel_get_map_conf_addr(int32_t mem_nodeid, genpaddr_t addr,
                                             gensize_t size, int32_t nodeid,
                                             char *ret_conf, size_t ret_conf_size,
                                             lvaddr_t *ret_addr)
{
    errval_t err;
#ifdef DISABLE_MODEL
    return SYS_ERR_OK;
#endif

    debug_printf("%s:%d: alias_conf request addr=0x%"PRIx64", size=%"PRIuGENSIZE"\n",
                 __FUNCTION__, __LINE__, addr, size);



    int32_t src_nodeid[2];
    char src_nodeid_str[128];
    src_nodeid[0] = nodeid;
    src_nodeid[1] = 0;
    format_nodelist(src_nodeid, src_nodeid_str);

    for(int tries=0; tries<3; tries++){
        HWMODEL_QUERY_DEBUG(MAP_WRAP_Q, size, mem_nodeid, addr, src_nodeid_str);
        err = skb_execute_query(MAP_WRAP_Q, size, mem_nodeid, addr, src_nodeid_str);
        if(err_is_ok(err)) break;
    }
    if (err_is_fail(err)) {
        DEBUG_SKB_ERR(err, "alias_conf \n");
        return err;
    }

    // Determine and copy conf line (second output line)
    char * confline = strstr(skb_get_output(), "\n");
    assert(confline);
    if(ret_conf){
        strncpy(ret_conf, confline + 1, ret_conf_size);
    }

    debug_printf("retbuf=%p, %s\n", ret_conf, confline);

    // Parse names
    *confline = 0;
    struct hwmodel_name names[2];
    int conversions;
    driverkit_parse_namelist(skb_get_output(), names, &conversions);
    debug_printf("Conversions = %d\n", conversions);


    if(ret_addr) *ret_addr = names[1].address;

    return SYS_ERR_OK;
}

/**
 * Makes dst visible to nodeid, assuming the configuration returned 
 * in ret_conf will be installed.
 */
errval_t driverkit_hwmodel_get_map_conf(struct capref dst,
                                       int32_t nodeid,
                                       char *ret_conf, size_t ret_conf_size,
                                       lvaddr_t *ret_addr)
{
#ifdef DISABLE_MODEL
    return SYS_ERR_OK;
#else
    struct frame_identity id;
    errval_t err;
    err = frame_identify(dst, &id);
    if (err_is_fail(err)) {
        return err;
    }

    int32_t mem_nodeid = driverkit_hwmodel_lookup_pcibus_node_id();

    return driverkit_hwmodel_get_map_conf_addr(mem_nodeid, id.base, id.bytes,
                                               nodeid, ret_conf, ret_conf_size, ret_addr);
#endif
}
