/*
 * Copyright (c) 2008, ETH Zurich. All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstrasse 6, CH-8092 Zurich. Attn: Systems Group.
 */
/*
 * e1000.c
 *
 *  Created on: Feb 12, 2013
 *      Author: mao
 *
 * NOTES:
 *      General:
 *          The driver uses kaluga to probe for supported PCI/e devices. At boot, It might happen
 *          that kaluga has not yet finished probing PCI devices and your card doesn't get detected.
 *          If you want the driver automaticaly at boot, try passing device id as a parameter in grub.
 *          Edit menu.lst:
 *          module /x86_64/sbin/e1000 deviceid=xxxx
 *
 *          If you don't know your device id, use lshw -pci to list available PCI devices.
 *          Try looking up all devices_id with vendor 0x8086 in the PCI database:
 *              http://www.pcidatabase.com/vendor_details.php?id=1302
 *          Your network card should be called some thing with e1000, Intel Pro/1000 or e1000.
 *
 *          If you use Simics, also read the Simics note.
 *
 *      Simics:
 *          Currently Simics doesn't provide an EEPROM for the tested network cards and the mac_address
 *          argument doesn't seem to set the MAC address properly. You will have to specify it manually:
 *
 *          e1000 mac=54:10:10:53:00:30
 *
 *
 * This part of the code builds on the original e1000 Barrelfish driver.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/nameservice_client.h>
#include <driverkit/driverkit.h>

#include <if/octopus_defs.h>
#include <if/e1000_devif_defs.h>

#include <octopus/octopus.h>
#include <octopus/trigger.h>
#include <net_queue_manager/net_queue_manager.h>
#include <trace/trace.h>
#include <int_route/msix_ctrl.h>
#include <int_route/int_route_client.h>

#include <pci/pci.h>

#include "e1000n.h"
#include "test_instr.h"

#if CONFIG_TRACE && NETWORK_STACK_TRACE
#define TRACE_ETHERSRV_MODE 1
#endif // CONFIG_TRACE && NETWORK_STACK_TRACE

#define MAX_ALLOWED_PKT_PER_ITERATION   (0xff)  // working value

/* MTU is 1500 bytes, plus Ethernet header plus CRC. */
#define RX_PACKET_MAX_LEN       (1500 + 14 + 4)

#define PACKET_SIZE_LIMIT       1073741824      /* 1 Gigabyte */

/*****************************************************************
 * External declarations for net_queue_manager
 *
 ****************************************************************/
uint64_t interrupt_counter;

static void e1000_print_link_status(struct e1000_driver_state *eds)
{
    const char *media_type = NULL;
    e1000_status_t status;

    status = e1000_status_rd(eds->device);
    e1000_ledctl_rd(eds->device);

    switch (eds->media_type) {
    case e1000_media_type_copper:
        media_type = "copper";
        break;
    case e1000_media_type_fiber:
        media_type = "fiber";
        break;
    case e1000_media_type_serdes:
        media_type = "SerDes";
        break;
    default:
        media_type = "Unknown";
        break;
    }

    if (e1000_check_link_up(eds->device)) {
        const char *duplex;

        if (e1000_status_fd_extract(status)) {
            duplex = "Full";
        } else {
            duplex = "Half";
        }

        switch (e1000_status_speed_extract(status)) {
        case 0x0:
            E1000_PRINT("Media type: %s, Link speed: 10 Mb in %s duplex.\n",
                        media_type, duplex);
            break;
        case 0x1:
            E1000_PRINT("Media type: %s, Link speed: 100 Mb in %s duplex.\n",
                        media_type, duplex);
            break;
        default:
            E1000_PRINT("Media type: %s, Link speed: 1 Gb in %s duplex.\n",
                        media_type, duplex);
            break;
        }
    } else {
        E1000_PRINT("Media type: %s, Link down.\n", media_type);
    }
}




/*****************************************************************
 * Parse MAC address to see if it has a valid format.
 *
 ****************************************************************/
static bool parse_mac(uint8_t *mac, const char *str)
{
    for (int i = 0; i < 6; i++) {
        char *next = NULL;
        unsigned long val = strtoul(str, &next, 16);
        if (val > UINT8_MAX || next == NULL || (i == 5 && *next != '\0')
                || (i < 5 && (*next != ':' && *next != '-'))) {
            return false; /* parse error */
        }
        mac[i] = val;
        str = next + 1;
    }

    return true;
}

static void setup_internal_memory(struct e1000_driver_state * eds)
{
    eds->receive_opaque = calloc(sizeof(void *), DRIVER_RECEIVE_BUFFERS);
    assert(eds->receive_opaque != NULL );
}


/*
 * Start a MSIx controller client, if possible and requested.
 */
static errval_t e1000_init_msix_client(struct e1000_driver_state * eds) {

    errval_t err;
    struct capref bar2;
    lvaddr_t vaddr;

    err = driverkit_get_bar_cap(eds->bfi, 2, &bar2);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "pcid_get_bar_cap");
        E1000_PRINT_ERROR("Error: pcid_get_bar_cap. Will not initialize"
                " MSIx controller.\n");
        return err;
    }

    err = map_device_cap(bar2, &vaddr);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "pcid_map_bar");
        E1000_PRINT_ERROR("Error: map_device failed. Will not initialize"
                " MSIx controller.\n");
        return err;
    }

    err = msix_client_init_by_args(eds->args_len, eds->args, (void*) vaddr);
    if(err == SYS_ERR_IRQ_INVALID){
        E1000_DEBUG("Card supports MSI-x but legacy interrupt requested by Kaluga");
        return err;
    }
    if(err_is_fail(err)){
        DEBUG_ERR(err, "msix_client_init");
        return err;
    }
    return SYS_ERR_OK;
}

/*****************************************************************
 * e1000 interrupt handler
 *
 ****************************************************************/
static void e1000_interrupt_handler_fn(void *arg)
{
    struct e1000_driver_state * eds = arg;
    /* Read interrupt cause, this also acknowledges the interrupt */
    e1000_intreg_t icr = e1000_icr_rd(eds->device);
    test_instr_interrupt(eds, icr);

#if TRACE_ETHERSRV_MODE
    trace_event(TRACE_SUBSYS_NNET, TRACE_EVENT_NNET_NI_I, interrupt_counter);
#endif

//    printf("#### interrupt handler called: %"PRIu64"\n", interrupt_counter);
    ++interrupt_counter;

    if (e1000_intreg_lsc_extract(icr) != 0) {
        if (e1000_check_link_up(eds->device)) {
            e1000_auto_negotiate_link(eds->device, eds->mac_type);
        } else {
            E1000_DEBUG("Link status change to down.\n");
        }
    }

    if (e1000_intreg_rxt0_extract(icr) != 0) {
        E1000_DEBUG("Packet received!.\n");
    }
}

/* Configure the card to trigger MSI-x interrupts */
static void e1000_enable_msix(struct e1000_driver_state * eds){
    // enable all the msix interrupts and map those to MSI vector 0
    e1000_ivar_82574_int_alloc0_wrf(eds->device, 0);
    e1000_ivar_82574_int_alloc_val0_wrf(eds->device, 1);

    e1000_ivar_82574_int_alloc1_wrf(eds->device, 0);
    e1000_ivar_82574_int_alloc_val1_wrf(eds->device, 1);

    e1000_ivar_82574_int_alloc2_wrf(eds->device, 0);
    e1000_ivar_82574_int_alloc_val2_wrf(eds->device, 1);

    e1000_ivar_82574_int_alloc3_wrf(eds->device, 0);
    e1000_ivar_82574_int_alloc_val3_wrf(eds->device, 1);

    e1000_ivar_82574_int_alloc4_wrf(eds->device, 0);
    e1000_ivar_82574_int_alloc_val4_wrf(eds->device, 1);

    e1000_ivar_82574_int_on_all_wb_wrf(eds->device, 1);
    int count = 4;
    pcid_enable_msix(&count);
}

/*****************************************************************
 * PCI init callback.
 *
 * Setup device, create receive ring and connect to Ethernet server.
 *
 ****************************************************************/
static void e1000_init_fn(struct e1000_driver_state * device)
{
    // set features
    switch (device->mac_type) {
        case e1000_82571:
        case e1000_82572:
        case e1000_82574: {
            device->extended_interrupts = 0;
            device->advanced_descriptors = 1;
        } break;
        case e1000_82576:
        case e1000_I210:
        case e1000_I219:
        case e1000_I350: {
            device->extended_interrupts = 1;
            device->advanced_descriptors = 3;
        } break;
    default:
        device->extended_interrupts = 0;
        device->advanced_descriptors = 0;
    }

    E1000_DEBUG("Starting hardware initialization.\n");
    e1000_hwinit(device);
    E1000_DEBUG("Hardware initialization complete.\n");


    errval_t err;

    struct capref intcap = NULL_CAP;
    err = driverkit_get_interrupt_cap(device->bfi, &intcap);
    assert(err_is_ok(err));

    if (device->msix) {
        E1000_DEBUG("MSI-X interrupt support!\n");
        err = e1000_init_msix_client(device);
        if(err_is_ok(err)){
            // Can use MSIX
            E1000_DEBUG("Successfully instantiated MSIx, setup int routing\n");
            e1000_enable_msix(device);
        }

        err = int_route_client_route_and_connect(intcap, 0,
                                                 get_default_waitset(), 
                                                 e1000_interrupt_handler_fn, device);
        if (err_is_fail(err)) {
            USER_PANIC("Interrupt setup failed!\n");
        }
    } else {
        E1000_DEBUG("Legacy interrupt support!\n");
#ifdef UNDER_TEST
        err = int_route_client_route_and_connect(intcap, 0,
                                                 get_default_waitset(), 
                                                 e1000_interrupt_handler_fn, device);
        if(err_is_fail(err)){
            USER_PANIC("Setting up interrupt failed \n");
        }
#endif
    }

    test_instr_init(device);

    setup_internal_memory(device);
}




/*****************************************************************
 * Print help and exit.
 *
 *
 ****************************************************************/
static void exit_help(void)
{
    fprintf(stderr, "Args for e1000n driver:\n");
    fprintf(stderr, "\taffinitymin=  Set RAM min affinity. When using this option it's mandatory to also set affinitymax.\n");
    fprintf(stderr, "\taffinitymax=  Set RAM max affinity. When using this option it's mandatory to also set affinitymin.\n");
    fprintf(stderr, "\tserivcename=  Set device driver service name.\n");
    fprintf(stderr, "\tbus=          Connect driver to device using this PCI bus.\n");
    fprintf(stderr, "\tfunction=     Connect driver to PCI device with function id.\n");
    fprintf(stderr, "\tdevice=       Connect driver to PCI device with type id.\n");
    fprintf(stderr, "\tdeviceid=     Connect driver to PCI device with device id.\n");
    fprintf(stderr, "\tmac=          Set device MAC address using MAC address format.\n");
    fprintf(stderr, "\tnoirq         Do not enable PCI bus IRQ, use device polling instead.\n");
    fprintf(stderr, "\t-h, --help    Print this help.\n");

    exit(2);
}


/*
static void e1000_reregister_handler(void *arg)
{
    errval_t err;
    printf("%s:%s:%d:\n", __FILE__, __FUNCTION__, __LINE__);
    err = pci_reregister_irq_for_device(
            class, subclass, program_interface,
            vendor, deviceid, bus, device, function,
            e1000_interrupt_handler_fn, NULL,
            e1000_reregister_handler, NULL);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "pci_reregister_irq_for_device");
    }

    return;
} */

void e1000_driver_state_init(struct e1000_driver_state * eds){
    memset(eds, 0, sizeof(struct e1000_driver_state));
    eds->mac_type = e1000_undefined;
    eds->user_mac_address = false;
    //eds->use_interrupt = false;
    eds->use_interrupt = true;
    eds->use_force = false;
    eds->queue_init_done = false;

    eds->rx_bsize = bsize_16384;
    //eds->rx_bsize = bsize_2048;

    eds->minbase = -1;
    eds->maxbase = -1;
}


/******************************************************************************/
/* Management interface implemetation */

static errval_t cd_create_queue_rpc(struct e1000_devif_binding *b, 
                                bool interrupt, 
                                uint64_t *mac, int *media_type, struct capref *regs,
                                struct capref *irq, struct capref *iommu, errval_t* err)
{
    struct e1000_driver_state* driver = (struct e1000_driver_state*) b->st;

    assert(driver != NULL);

    if (driver->queue_init_done) {
        debug_printf("e1000: queue already initalized. \n");
        driver->queue_init_done = true;
        return DEVQ_ERR_INIT_QUEUE;
    }
    memcpy(mac, driver->mac_address, sizeof(driver->mac_address));
    *regs = driver->regs;
    *media_type = driver->media_type;

    errval_t err_out = driverkit_get_interrupt_cap(driver->bfi, irq);
    if (err_is_fail(err_out)) {
        *err = err_out; 
        return DEVQ_ERR_INIT_QUEUE;
    }

    err_out = driverkit_get_iommu_cap(driver->bfi, iommu);
    if (err_is_fail(err_out)) {
        *err = err_out;
        return DEVQ_ERR_INIT_QUEUE;
    }

    driver->queue_init_done = true;
    return SYS_ERR_OK;
}


static void cd_create_queue(struct e1000_devif_binding *b, bool interrupt)
{
    debug_printf("in cd_create_queue\n");
    uint64_t mac = 0;
    errval_t err = SYS_ERR_OK;

    struct capref regs;
    struct capref irq;
    struct capref iommu;
    int media_type;

    cd_create_queue_rpc(b, interrupt, &mac, &media_type, &regs, &irq, &iommu, &err);
    
    // check endpoint
    struct endpoint_identity epid;
    err = invoke_endpoint_identify(iommu, &epid);
    if (err_is_fail(err)){
        // IOMMU not started, continue
        iommu = NULL_CAP;
        err = SYS_ERR_OK;
    }

    err = b->tx_vtbl.create_queue_response(b, NOP_CONT, mac, media_type, 
                                           regs, irq, iommu, err);
    assert(err_is_ok(err));
}

static errval_t cd_destroy_queue_rpc(struct e1000_devif_binding *b, errval_t* err)
{
    USER_PANIC("NIY \n");
    return SYS_ERR_OK;
}


static void cd_destroy_queue(struct e1000_devif_binding *b)
{
    errval_t err = SYS_ERR_OK;

    cd_destroy_queue_rpc(b, &err);

    err = b->tx_vtbl.destroy_queue_response(b, NOP_CONT, SYS_ERR_OK);
    assert(err_is_ok(err));
}


static errval_t connect_devif_cb(void *st, struct e1000_devif_binding *b)
{

    b->rx_vtbl.create_queue_call = cd_create_queue;
    b->rx_vtbl.destroy_queue_call = cd_destroy_queue;

    b->rpc_rx_vtbl.create_queue_call = cd_create_queue_rpc;
    b->rpc_rx_vtbl.destroy_queue_call = cd_destroy_queue_rpc;
    b->st = st;

    return SYS_ERR_OK;
}

static void export_devif_cb(void *st, errval_t err, iref_t iref)
{
    struct e1000_driver_state* s = (struct e1000_driver_state*) st;
    const char *suffix = "devif";
    char name[256];

    assert(err_is_ok(err));

    // Build label for interal management service
    sprintf(name, "%s_%x_%x_%x_%s", s->service_name, s->addr.bus, s->addr.device, 
            s->addr.function, suffix);

    err = nameservice_register(name, iref);
    assert(err_is_ok(err));
    s->initialized = true;
}

/**
 * Initialize management interface for queue drivers.
 * This has to be done _after_ the hardware is initialized.
 */
static void initialize_mngif(struct e1000_driver_state* st)
{
    errval_t r;

    r = e1000_devif_export(st, export_devif_cb, connect_devif_cb,
                           get_default_waitset(), 1);
    assert(err_is_ok(r));
}

static errval_t init(struct bfdriver_instance *bfi, uint64_t flags, iref_t *dev)
{

    errval_t err;

    /** Parse command line arguments. */
    E1000_DEBUG("e1000 standalone driver started.\n");
    E1000_DEBUG("args_len=%d, caps_len=%d\n", bfi->capc, bfi->argc);

    struct e1000_driver_state * eds = malloc(sizeof(struct e1000_driver_state));
    e1000_driver_state_init(eds);
    bfi->dstate = eds;

    eds->args = bfi->argv;
    eds->args_len = bfi->argc;
    eds->service_name = "e1000"; // default name
    eds->bfi = bfi;

    struct capref cap;
    // When started by Kaluga it handend off an endpoint cap to PCI
    err = driverkit_get_pci_cap(bfi, &cap);
    if (err_is_fail(err)) {
        goto err_out;
    }

    assert(!capref_is_null(cap));

    debug_printf("Connect to PCI\n");
    err = pci_client_connect_ep(cap);
    if (err_is_fail(err)) {
        goto err_out;
    }
    
    err = pci_get_device_info(&eds->addr, &eds->id);
    if (err_is_fail(err)) {
        goto err_out;
    }

    for (int i = 1; i < bfi->argc; i++) {
        E1000_DEBUG("arg %d = %s\n", i, bfi->argv[i]);
        if (strcmp(bfi->argv[i], "auto") == 0) {
            continue;
        }
        if (strncmp(bfi->argv[i], "affinitymin=", strlen("affinitymin=")) == 0) {
            eds->minbase = atol(bfi->argv[i] + strlen("affinitymin="));
            E1000_DEBUG("minbase = %lu\n", eds->minbase);
        } else if (strncmp(bfi->argv[i], "affinitymax=", strlen("affinitymax="))
                 == 0) {
            eds->maxbase = atol(bfi->argv[i] + strlen("affinitymax="));
            E1000_DEBUG("maxbase = %lu\n", eds->maxbase);
        } else if (strncmp(bfi->argv[i], "mac=", strlen("mac=")) == 0) {
            uint8_t* mac = eds->mac_address;
            if (parse_mac(mac, bfi->argv[i] + strlen("mac="))) {
                eds->user_mac_address = true;
                E1000_DEBUG("MAC= %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
                            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            } else {
                E1000_PRINT_ERROR("Error: Failed parsing MAC address '%s'.\n",
                                  bfi->argv[i]);
                exit(1);
            }
        } else if(strncmp(bfi->argv[i], "servicename=", strlen("servicename=")) == 0) {
            eds->service_name = bfi->argv[i]  + strlen("servicename=");
            E1000_DEBUG("Service name=%s.\n", eds->service_name);
        } else if(strcmp(bfi->argv[i],"noirq")==0) {
            E1000_DEBUG("Driver working in polling mode.\n");
            eds->use_interrupt = false;
        } else if (strcmp(bfi->argv[i], "-h") == 0 ||
                 strcmp(bfi->argv[i], "--help") == 0) {
            exit_help();
        } else {
            E1000_DEBUG("Parsed Kaluga device address %s.\n", bfi->argv[i]);
        }
    } // end for :

    if ((eds->minbase != -1) && (eds->maxbase != -1)) {
        E1000_DEBUG("set memory affinity [%lx, %lx]\n", eds->minbase, eds->maxbase);
        ram_set_affinity(eds->minbase, eds->maxbase);
    }

    E1000_DEBUG("Starting e1000 driver.\n");

    eds->mac_type = e1000_get_mac_type(eds->id.vendor, eds->id.device);
    E1000_DEBUG("mac_type is: %s\n", e1000_mac_type_to_str(eds->mac_type));

    /* Setup known device info */
    if (eds->mac_type == e1000_82575
        || eds->mac_type == e1000_82576
        || eds->mac_type == e1000_I210
        || eds->mac_type == e1000_I219
        || eds->mac_type == e1000_I350) {
        // These cards do not have a bsex reg entry
        // therefore, we can't use 16384 buffer size.
        // If we use smaller buffers than 2048 bytes the
        // eop bit on received packets might not be set in case the package
        // is biger than the receive buffer size and we don't handle these
        // cases currently.
        eds->rx_bsize = bsize_2048;
    } else {
        eds->rx_bsize = bsize_16384;
    }
    eds->media_type = e1000_media_type_undefined;

    // Setup int routing in init_fn
    e1000_init_fn(eds);

    if(false) e1000_print_link_status(eds);

    initialize_mngif(eds);

    return err;

err_out:
    free(eds);
    return err;
}

static errval_t attach(struct bfdriver_instance* bfi) {
    return SYS_ERR_OK;
}

static errval_t detach(struct bfdriver_instance* bfi) {
    return SYS_ERR_OK;
}

static errval_t set_sleep_level(struct bfdriver_instance* bfi, uint32_t level) {
    struct e1000_driver_state* eds = bfi->dstate;
    eds->level = level;
    return SYS_ERR_OK;
}

static errval_t destroy(struct bfdriver_instance* bfi) {
    struct e1000_driver_state* eds = bfi->dstate;
    free(eds);
    bfi->dstate = NULL;
    // XXX: Tear-down the service
    bfi->device = 0x0;
    return SYS_ERR_OK;
}

// TODO local RPCs
static struct e1000_devif_rx_vtbl vtbl = {
    .create_queue_call = cd_create_queue,
    .destroy_queue_call = cd_destroy_queue
};

static struct e1000_devif_rpc_rx_vtbl rpc_vtbl = {
    .create_queue_call = cd_create_queue_rpc,
    .destroy_queue_call = cd_destroy_queue_rpc
};

static errval_t get_ep(struct bfdriver_instance* bfi, bool lmp, struct capref* ret_cap)
{
    E1000_DEBUG("Endpoint was requested \n");
    errval_t err;
    struct e1000_devif_binding* b;
    err = e1000_devif_create_endpoint(lmp? IDC_ENDPOINT_LMP: IDC_ENDPOINT_UMP, 
                                      &vtbl, bfi->dstate,
                                      get_default_waitset(),
                                      IDC_ENDPOINT_FLAGS_DUMMY,
                                      &b, *ret_cap);
    b->rpc_rx_vtbl = rpc_vtbl;
    return SYS_ERR_OK;
}

#ifdef UNDER_TEST
DEFINE_MODULE(e1000n_irqtest_module, init, attach, detach, set_sleep_level, destroy, get_ep);
#else
DEFINE_MODULE(e1000n_module, init, attach, detach, set_sleep_level, destroy, get_ep);
#endif
