/*
 * Copyright (c) 2018, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstr. 6, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/barrelfish.h>
#include <numa.h>

#include "intel_vtd.h"

static struct vtd_device  *vtd_devices[VTD_NUM_ROOT_ENTRIES];

errval_t vtd_devices_init(void)
{
    INTEL_VTD_DEBUG_DEVICES("Initialize. \n");

    memset(vtd_devices, 0, sizeof(vtd_devices));

    return SYS_ERR_OK;
}


static struct vtd_device *get_device(uint16_t pciseg, uint8_t bus, uint8_t dev,
                                     uint8_t fun)
{
    assert(pciseg == 0);

    if (vtd_devices[bus]) {
        return &vtd_devices[bus][vtd_dev_fun_to_ctxt_id(dev, fun)];
    }

    vtd_devices[bus] = calloc(VTD_NUM_ROOT_ENTRIES, sizeof(struct vtd_device));
    if (vtd_devices[bus] == NULL) {
        return NULL;
    }

    return &vtd_devices[bus][vtd_dev_fun_to_ctxt_id(dev, fun)];
}


errval_t vtd_devices_create(uint16_t pciseg, uint8_t bus, uint8_t dev,
                            uint8_t fun, struct iommu_binding *binding,
                            struct vtd_device **rdev)
{
    errval_t err ;

    struct vtd *vtd;
    err = vtd_lookup_by_device(bus, dev, fun, pciseg, &vtd);
    if (err_is_fail(err)) {
        return err;
    }

    struct vtd_device *vdev = get_device(pciseg, bus, dev, fun);
    if (vdev == NULL) {
        return LIB_ERR_MALLOC_FAIL;
    }

    vdev->bus = bus;
    vdev->function = fun;
    vdev->device = dev;
    vdev->pciseg = pciseg;
    vdev->binding = binding;

    vdev->domain = NULL;
    vdev->mappingcap = NULL_CAP;

    err = vtd_get_ctxt_table_by_id(vtd, bus, &vdev->ctxt_table);
    if (err_is_fail(err)) {
        return err;
    }

    *rdev = vdev;

    return SYS_ERR_OK;
}

errval_t vtd_devices_destroy(struct vtd_device *dev)
{
    errval_t err;
    err = vtd_devices_remove_from_domain(dev);
    if (err_is_fail(err)) {
        return err;
    }

    memset(dev, 0, sizeof(*dev));

    return SYS_ERR_OK;
}

errval_t vtd_devices_remove_from_domain(struct vtd_device *dev)
{
    errval_t err = SYS_ERR_OK;
    if (dev->domain != NULL) {
        err = vtd_domains_remove_device(dev->domain, dev);
    }

    return err;
}

struct vtd_device *vtd_devices_get(uint8_t bus, uint8_t dev, uint8_t fun)
{
    USER_PANIC("NYI");
    return NULL;
}

struct vtd_device *vtd_devices_get_by_cap(struct capref cap)
{
    USER_PANIC("NYI");
    return NULL;
}