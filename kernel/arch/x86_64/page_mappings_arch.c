/**
 * \file
 * \brief
 */

/*
 * Copyright (c) 2010-2013, 2016 ETH Zurich.
 * Copyright (c) 2014, HP Labs.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstrasse 6, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>
#include <dispatch.h>
#include <target/x86_64/paging_kernel_target.h>
#include <target/x86_64/offsets_target.h>
#include <paging_kernel_arch.h>
#include <mdb/mdb_tree.h>
#include <string.h>
#include <barrelfish_kpi/init.h>
#include <cap_predicates.h>
#include <paging_generic.h>
#include <useraccess.h>

#ifdef __k1om__
#include <target/k1om/offsets_target.h>
#define MEMORY_OFFSET K1OM_MEMORY_OFFSET
#else
#include <target/x86_64/offsets_target.h>
#define MEMORY_OFFSET X86_64_MEMORY_OFFSET
#endif

/// Map within a x86_64 non leaf ptable
static errval_t x86_64_non_ptable(struct capability *dest, cslot_t slot,
                                  struct capability *src, uintptr_t flags,
                                  uintptr_t offset, size_t pte_count,
                                  struct cte *mapping_cte)
{
    //printf("page_mappings_arch:x86_64_non_ptable\n");
    if (slot >= X86_64_PTABLE_SIZE) { // Within pagetable
        return SYS_ERR_VNODE_SLOT_INVALID;
    }

    if (type_is_vnode(src->type) && pte_count != 1) { // only allow single ptable mappings
        debug(SUBSYS_PAGING, "src type and count mismatch\n");
        return SYS_ERR_VM_MAP_SIZE;
    }

    if (slot + pte_count > X86_64_PTABLE_SIZE) { // mapping size ok
        debug(SUBSYS_PAGING, "mapping size invalid (%zd)\n", pte_count);
        return SYS_ERR_VM_MAP_SIZE;
    }

    size_t page_size = 0;
    paging_x86_64_flags_t flags_large = 0;
    bool is_ept = false;
    switch (dest->type) {
        case ObjType_VNode_x86_64_pml4:
            if (src->type != ObjType_VNode_x86_64_pdpt) { // Right mapping
                debug(SUBSYS_PAGING, "src type invalid: %d\n", src->type);
                return SYS_ERR_WRONG_MAPPING;
            }
            if(slot >= X86_64_PML4_BASE(MEMORY_OFFSET)) { // Kernel mapped here
                return SYS_ERR_VNODE_SLOT_RESERVED;
            }
            break;
        case ObjType_VNode_x86_64_ept_pml4:
            is_ept = true;
            if (src->type != ObjType_VNode_x86_64_ept_pdpt) { // Right mapping
                printf("src type invalid\n");
                return SYS_ERR_WRONG_MAPPING;
            }
            if(slot >= X86_64_PML4_BASE(MEMORY_OFFSET)) { // Kernel mapped here
                return SYS_ERR_VNODE_SLOT_RESERVED;
            }
            break;
        case ObjType_VNode_x86_64_pdpt:
            // huge page support
            if (src->type != ObjType_VNode_x86_64_pdir) { // Right mapping
                if (src->type != ObjType_Frame &&
                    src->type != ObjType_DevFrame &&
                    src->type != ObjType_EndPointUMP) { // Right mapping
                    debug(SUBSYS_PAGING, "src type invalid: %d\n", src->type);
                    return SYS_ERR_WRONG_MAPPING;
                }

                if (get_size(src) < HUGE_PAGE_SIZE) {
                    return SYS_ERR_VM_FRAME_TOO_SMALL;
                }

                if ((get_address(src)+offset) & HUGE_PAGE_MASK) {
                    return SYS_ERR_VM_FRAME_UNALIGNED;
                }

                // TODO: check if the system allows 1GB mappings
                page_size = X86_64_HUGE_PAGE_SIZE;
                // check offset within frame
                genpaddr_t off = offset;

                if (off + pte_count * X86_64_HUGE_PAGE_SIZE > get_size(src)) {
                    printk(LOG_NOTE, "frame offset invalid: %zx > 0x%"PRIxGENSIZE"\n",
                            off + pte_count * X86_64_BASE_PAGE_SIZE, get_size(src));
                    return SYS_ERR_FRAME_OFFSET_INVALID;
                }
                // Calculate page access protection flags /
                // Get frame cap rights
                flags_large = paging_x86_64_cap_to_page_flags(src->rights);
                // Mask with provided access rights mask
                flags_large = paging_x86_64_mask_attrs(flags, X86_64_PTABLE_ACCESS(flags));
                // Add additional arch-specific flags
                flags_large |= X86_64_PTABLE_FLAGS(flags);
                // Unconditionally mark the page present
                flags_large |= X86_64_PTABLE_PRESENT;
            }
            break;
        case ObjType_VNode_x86_64_ept_pdpt:
            is_ept = true;
            // huge page support
            if (src->type != ObjType_VNode_x86_64_ept_pdir) { // Right mapping
                // TODO: check if the system allows 1GB mappings
                page_size = X86_64_HUGE_PAGE_SIZE;
                // check offset within frame
                genpaddr_t off = offset;

                if (off + pte_count * X86_64_HUGE_PAGE_SIZE > get_size(src)) {
                    return SYS_ERR_FRAME_OFFSET_INVALID;
                }
                // Calculate page access protection flags /
                // Get frame cap rights
                flags_large = paging_x86_64_cap_to_page_flags(src->rights);
                // Mask with provided access rights mask
                flags_large = paging_x86_64_mask_attrs(flags, X86_64_PTABLE_ACCESS(flags));
                // Add additional arch-specific flags
                flags_large |= X86_64_PTABLE_FLAGS(flags);
                // Unconditionally mark the page present
                flags_large |= X86_64_PTABLE_PRESENT;
            }
            break;
        case ObjType_VNode_x86_64_pdir:
            // superpage support
            if (src->type != ObjType_VNode_x86_64_ptable) { // Right mapping
                if (src->type != ObjType_Frame &&
                    src->type != ObjType_DevFrame &&
                    src->type != ObjType_EndPointUMP) { // Right mapping
                    debug(SUBSYS_PAGING, "src type invalid: %d\n", src->type);
                    return SYS_ERR_WRONG_MAPPING;
                }

                if (get_size(src) < LARGE_PAGE_SIZE) {
                    return SYS_ERR_VM_FRAME_TOO_SMALL;
                }

                if ((get_address(src)+offset) & LARGE_PAGE_MASK) {
                    return SYS_ERR_VM_FRAME_UNALIGNED;
                }

                page_size = X86_64_LARGE_PAGE_SIZE;

                // check offset within frame
                genpaddr_t off = offset;

                if (off + pte_count * X86_64_LARGE_PAGE_SIZE > get_size(src)) {
                    printk(LOG_NOTE, "frame offset invalid: %zx > 0x%"PRIxGENSIZE"\n",
                            off + pte_count * X86_64_BASE_PAGE_SIZE, get_size(src));
                    return SYS_ERR_FRAME_OFFSET_INVALID;
                }
                // Calculate page access protection flags /
                // Get frame cap rights
                flags_large = paging_x86_64_cap_to_page_flags(src->rights);
                // Mask with provided access rights mask
                flags_large = paging_x86_64_mask_attrs(flags, X86_64_PTABLE_ACCESS(flags));
                // Add additional arch-specific flags
                flags_large |= X86_64_PTABLE_FLAGS(flags);
                // Unconditionally mark the page present
                flags_large |= X86_64_PTABLE_PRESENT;

            }
            break;
        case ObjType_VNode_x86_64_ept_pdir:
            is_ept = true;
            // superpage support
            if (src->type != ObjType_VNode_x86_64_ept_ptable) { // Right mapping
                page_size = X86_64_LARGE_PAGE_SIZE;

                // check offset within frame
                genpaddr_t off = offset;

                if (off + pte_count * X86_64_LARGE_PAGE_SIZE > get_size(src)) {
                    return SYS_ERR_FRAME_OFFSET_INVALID;
                }
                // Calculate page access protection flags /
                // Get frame cap rights
                // Mask with provided access rights mask
                flags_large = paging_x86_64_mask_attrs(flags, X86_64_PTABLE_ACCESS(flags));
                // Add additional arch-specific flags
                flags_large |= X86_64_PTABLE_FLAGS(flags);
                // Unconditionally mark the page present
                flags_large |= X86_64_PTABLE_PRESENT;
            }
            break;
        default:
            debug(SUBSYS_PAGING, "dest type invalid\n");
            return SYS_ERR_DEST_TYPE_INVALID;
    }

    // Convert destination base address
    genpaddr_t dest_gp   = get_address(dest);
    lpaddr_t dest_lp     = gen_phys_to_local_phys(dest_gp);
    lvaddr_t dest_lv     = local_phys_to_mem(dest_lp);
    // Convert source base address
    genpaddr_t src_gp   = get_address(src);
    lpaddr_t src_lp     = gen_phys_to_local_phys(src_gp);

    // set metadata
    create_mapping_cap(mapping_cte, src, cte_for_cap(dest),
                       slot, pte_count);

    // use standard paging structs even if we're mapping EPT entries
    is_ept = false;

    bool need_tlb_flush = false;
    cslot_t last_slot = slot + pte_count;
    for (; slot < last_slot; slot++, offset += page_size) {
        // Destination
        union x86_64_pdir_entry *entry = (union x86_64_pdir_entry *)dest_lv + slot;

        if (X86_64_IS_PRESENT(entry)) {
            // cleanup mapping info
            // TODO: cleanup already mapped pages
            //memset(&src_cte->mapping_info, 0, sizeof(struct mapping_info));
            //printf("slot in use\n");
            //return SYS_ERR_VNODE_SLOT_INUSE;
            need_tlb_flush = true;
        }

        // determine if we map a large/huge page or a normal entry
        if (page_size == X86_64_LARGE_PAGE_SIZE)
        {
            //a large page is mapped
//            if (is_ept) {
//                paging_x86_64_ept_map_large((union x86_64_ept_ptable_entry *)entry,
//                        src_lp + offset, flags_large);
//            } else {
                paging_x86_64_map_large((union x86_64_ptable_entry *)entry,
                        src_lp + offset, flags_large);
//            }
        } else if (page_size == X86_64_HUGE_PAGE_SIZE) {
            // a huge page is mapped
 //           if (is_ept) {
 //               paging_x86_64_ept_map_huge((union x86_64_ept_ptable_entry *)entry,
 //                       src_lp + offset, flags_large);
 //           } else {
                paging_x86_64_map_huge((union x86_64_ptable_entry *)entry,
                        src_lp + offset, flags_large);
 //           }
        } else {
            //a normal paging structure entry is mapped
  //          if (is_ept) {
  //              paging_x86_64_ept_map_table(
  //                      (union x86_64_ept_pdir_entry *)entry, src_lp + offset);
  //          } else {
                paging_x86_64_map_table(entry, src_lp + offset);
  //          }
        }
    }
    if (need_tlb_flush) {
        //TODO: do hint-based selective flush for single page mapping
        do_full_tlb_flush();
    }

    return SYS_ERR_OK;
}

/// Map within a x86_64 ptable
static errval_t x86_64_ptable(struct capability *dest, cslot_t slot,
                              struct capability *src, uintptr_t mflags,
                              uintptr_t offset, size_t pte_count,
                              struct cte *mapping_cte)
{
    //printf("page_mappings_arch:x86_64_ptable\n");
    if (slot >= X86_64_PTABLE_SIZE) { // Within pagetable
        debug(SUBSYS_PAGING, "    vnode_invalid\n");
        return SYS_ERR_VNODE_SLOT_INVALID;
    }

    if (slot + pte_count > X86_64_PTABLE_SIZE) { // mapping size ok
        debug(SUBSYS_PAGING, "mapping size invalid (%zd)\n", pte_count);
        return SYS_ERR_VM_MAP_SIZE;
    }

    if (src->type != ObjType_Frame &&
        src->type != ObjType_DevFrame &&
        //(!type_is_ept(dest->type) &&
        !type_is_vnode(src->type) &&
        src->type != ObjType_EndPointUMP) { // Right mapping
        debug(SUBSYS_PAGING, "src type invalid\n");
        return SYS_ERR_WRONG_MAPPING;
    }

    // check offset within frame
    genpaddr_t off = offset;
    if (off + pte_count * X86_64_BASE_PAGE_SIZE > get_size(src)) {
        debug(SUBSYS_PAGING, "frame offset invalid: %zx > 0x%"PRIxGENSIZE"\n",
                off + pte_count * X86_64_BASE_PAGE_SIZE, get_size(src));
        printk(LOG_NOTE, "frame offset invalid: %zx > 0x%"PRIxGENSIZE"\n",
                off + pte_count * X86_64_BASE_PAGE_SIZE, get_size(src));
        char buf[256];
        sprint_cap(buf,256,src);
        printk(LOG_NOTE, "src = %s\n", buf);
        return SYS_ERR_FRAME_OFFSET_INVALID;
    }


    /* Calculate page access protection flags */
    // Get frame cap rights
    paging_x86_64_flags_t flags;
    if (src->type != ObjType_RAM) {
        flags = paging_x86_64_cap_to_page_flags(src->rights);
    } else {
        flags = X86_64_PTABLE_READ_WRITE | X86_64_PTABLE_USER_SUPERVISOR;
    }
    // Mask with provided access rights mask
    flags = paging_x86_64_mask_attrs(flags, X86_64_PTABLE_ACCESS(mflags));
    // Add additional arch-specific flags
    flags |= X86_64_PTABLE_FLAGS(mflags);
    // Unconditionally mark the page present
    flags |= X86_64_PTABLE_PRESENT;
    // 1. Make sure non-guest page-tables are never writeable from user-space
    // 2. ptables mapped in EPT tables need to writeable
    if (type_is_vnode(src->type) &&
        !type_is_ept(dest->type) &&
        !dcb_current->is_vm_guest)
    {
        if (flags & X86_64_PTABLE_READ_WRITE) {
            return SYS_ERR_VM_MAP_RIGHTS;
        }
    }


    // Convert destination base address
    genpaddr_t dest_gp   = get_address(dest);
    lpaddr_t dest_lp     = gen_phys_to_local_phys(dest_gp);
    lvaddr_t dest_lv     = local_phys_to_mem(dest_lp);
    // Convert source base address
    genpaddr_t src_gp   = get_address(src);
    lpaddr_t src_lp     = gen_phys_to_local_phys(src_gp);
    // Set metadata
    create_mapping_cap(mapping_cte, src, cte_for_cap(dest),
                       slot, pte_count);

    bool need_tlb_flush = false;
    cslot_t last_slot = slot + pte_count;
    for (; slot < last_slot; slot++, offset += X86_64_BASE_PAGE_SIZE) {
        union x86_64_ptable_entry *entry =
            (union x86_64_ptable_entry *)dest_lv + slot;

        if (X86_64_IS_PRESENT(entry)) {
            // TODO: cleanup already mapped pages
            //memset(&src_cte->mapping_info, 0, sizeof(struct mapping_info));
            //debug(LOG_WARN, "Trying to remap an already-present page is NYI, but "
            //      "this is most likely a user-space bug!\n");
            //return SYS_ERR_VNODE_SLOT_INUSE;
            need_tlb_flush = true;
        }

        // Carry out the page mapping
#if 0
        if (type_is_ept(dest->type)) {
            paging_x86_64_ept_map((union x86_64_ept_ptable_entry *)entry,
                    src_lp + offset, flags);
        } else {
            paging_x86_64_map(entry, src_lp + offset, flags);
        }
#endif
        paging_x86_64_map(entry, src_lp + offset, flags);
    }
    if (need_tlb_flush) {
        // TODO: do hint-based selective tlb flush if single page modified
        do_full_tlb_flush();
    }

    return SYS_ERR_OK;
}

#include <dev/vtd_dev.h>

/// Map within a x86_64 non leaf ptable
static errval_t x86_64_vtd_table(struct capability *dest, cslot_t slot,
                                  struct capability *src, uintptr_t flags,
                                  uintptr_t offset, size_t pte_count,
                                  struct cte *mapping_cte)
{
    printf("page_mappings_arch:x86_64_vtd_table\n");
    if (slot >= 256) { // Within pagetable
        return SYS_ERR_VNODE_SLOT_INVALID;
    }

    if (pte_count != 1) {
        // only allow single ptable mappings
        debug(SUBSYS_PAGING, "src type and count mismatch\n");
        return SYS_ERR_VM_MAP_SIZE;
    }

    vtd_addr_width_t agaw = 0;
    switch (dest->type) {
        case ObjType_VNode_VTd_root_table :
            if (src->type != ObjType_VNode_VTd_ctxt_table) {
                debug(SUBSYS_PAGING, "src type invalid: %d\n", src->type);
                return SYS_ERR_WRONG_MAPPING;
            }

            break;
        case ObjType_VNode_VTd_ctxt_table :
            switch(src->type) {
                case ObjType_VNode_x86_64_pml4:
                    agaw = vtd_agaw48;
                    break;
                case ObjType_VNode_x86_64_pml5:
                    agaw = vtd_agaw57;
                    break;
                case ObjType_VNode_x86_64_pdpt:
                    agaw = vtd_agaw39;
                    break;
                default:
                    debug(SUBSYS_PAGING, "src type invalid: %d\n", src->type);
                    return SYS_ERR_WRONG_MAPPING;
            }
            break;
        default:
            debug(SUBSYS_PAGING, "dest type invalid\n");
            return SYS_ERR_DEST_TYPE_INVALID;
    }

    // Convert destination base address
    genpaddr_t dest_gp   = get_address(dest);
    lpaddr_t dest_lp     = gen_phys_to_local_phys(dest_gp);
    lvaddr_t dest_lv     = local_phys_to_mem(dest_lp);
    // Convert source base address
    genpaddr_t src_gp   = get_address(src);
    lpaddr_t src_lp     = gen_phys_to_local_phys(src_gp);

    switch (dest->type) {
        case ObjType_VNode_VTd_root_table : {
            vtd_root_entry_t rt = (vtd_root_entry_t) (dest_lv + slot *
                                                                vtd_root_entry_size);

            if (vtd_root_entry_p_extract(rt)) {
                return SYS_ERR_VNODE_SLOT_INUSE;
            }
            vtd_root_entry_ctp_insert(rt, src_lp >> BASE_PAGE_BITS);
            vtd_root_entry_p_insert(rt, 1);

            break;
        }
        case ObjType_VNode_VTd_ctxt_table : {

            vtd_ctxt_entry_t ct = (vtd_ctxt_entry_t)(
                    dest_lv + slot * vtd_ctxt_entry_size);
            if (vtd_ctxt_entry_p_extract(ct)) {
                return SYS_ERR_VNODE_SLOT_INUSE;
            }

            uint16_t domid = (flags >> 8) & 0xffff;

            /* Don't support device TLB's */
            vtd_ctxt_entry_t_insert(ct, vtd_hmd);
            vtd_ctxt_entry_aw_insert(ct, agaw);
            vtd_ctxt_entry_did_insert(ct, domid);

            #if 0
            if (device_tlbs_supported) {
             00b: Untranslated requests are translated using second-level
                  paging structures referenced through SLPTPTR field. Translated
                  requests and Translation Requests are blocked.
             01b: Untranslated, Translated and Translation Requests are
                  supported. This encoding is treated as reserved by hardware
                  implementations not supporting Device-TLBs (DT=0 in Extended
                  Capability Register).
             10b: Untranslated requests are processed as pass-through.
                  SLPTPTR field is ignored by hardware. Translated and Translation
                  Requests are blocked. This encoding is treated by hardware as
                  reserved for hardware implementations not supporting Pass Through
            #define vtd_hmd ((vtd_translation_type_t)0x0)
            #define vtd_hme ((vtd_translation_type_t)0x1)
            #define vtd_ptm ((vtd_translation_type_t)0x2)

            }
            #endif
            vtd_ctxt_entry_t_insert(ct, vtd_hmd);

            /* flush the cache */
            wbinvd();

            vtd_ctxt_entry_slptptr_insert(ct, (src_lp >> BASE_PAGE_BITS));
            vtd_ctxt_entry_p_insert(ct, 1);

            break;
        }
        default:
            debug(SUBSYS_PAGING, "dest type invalid\n");
            return SYS_ERR_DEST_TYPE_INVALID;
    }

    // set metadata
    create_mapping_cap(mapping_cte, src, cte_for_cap(dest),
                       slot, pte_count);


    return SYS_ERR_OK;
}


typedef errval_t (*mapping_handler_t)(struct capability *dest_cap,
                                      cslot_t dest_slot,
                                      struct capability *src_cap,
                                      uintptr_t flags, uintptr_t offset,
                                      size_t pte_count,
                                      struct cte *mapping_cte);

/// Dispatcher table for the type of mapping to create
static mapping_handler_t handler[ObjType_Num] = {
    [ObjType_VNode_VTd_root_table]  = x86_64_vtd_table,
    [ObjType_VNode_VTd_ctxt_table]  = x86_64_vtd_table,
    [ObjType_VNode_x86_64_pml5]     = x86_64_non_ptable,
    [ObjType_VNode_x86_64_pml4]     = x86_64_non_ptable,
    [ObjType_VNode_x86_64_pdpt]     = x86_64_non_ptable,
    [ObjType_VNode_x86_64_pdir]     = x86_64_non_ptable,
    [ObjType_VNode_x86_64_ptable]   = x86_64_ptable,
    [ObjType_VNode_x86_64_ept_pml4]   = x86_64_non_ptable,
    [ObjType_VNode_x86_64_ept_pdpt]   = x86_64_non_ptable,
    [ObjType_VNode_x86_64_ept_pdir]   = x86_64_non_ptable,
    [ObjType_VNode_x86_64_ept_ptable] = x86_64_ptable,
};


/// Create page mappings
errval_t caps_copy_to_vnode(struct cte *dest_vnode_cte, cslot_t dest_slot,
                            struct cte *src_cte, uintptr_t flags,
                            uintptr_t offset, uintptr_t pte_count,
                            struct cte *mapping_cte)
{
    assert(type_is_vnode(dest_vnode_cte->cap.type));
    assert(mapping_cte->cap.type == ObjType_Null);

    struct capability *src_cap  = &src_cte->cap;
    struct capability *dest_cap = &dest_vnode_cte->cap;
    mapping_handler_t handler_func = handler[dest_cap->type];

    assert(handler_func != NULL);

#if 0
    genpaddr_t paddr = get_address(&src_cte->cap) + offset;
    genvaddr_t vaddr;
    compile_vaddr(dest_vnode_cte, dest_slot, &vaddr);
    printk(LOG_NOTE, "mapping 0x%"PRIxGENPADDR" to 0x%"PRIxGENVADDR"\n", paddr, vaddr);
#endif

    cslot_t last_slot = dest_slot + pte_count;

    if (last_slot > X86_64_PTABLE_SIZE) {
        // requested map overlaps leaf page table
        debug(SUBSYS_CAPS,
                "caps_copy_to_vnode: requested mapping spans multiple leaf page tables\n");
        debug(SUBSYS_PAGING,
                "first_slot = %"PRIuCSLOT", last_slot = %"PRIuCSLOT", count = %zu\n",
                dest_slot, last_slot, pte_count);
        return SYS_ERR_VM_RETRY_SINGLE;
    }

    errval_t r = handler_func(dest_cap, dest_slot, src_cap, flags, offset,
                              pte_count, mapping_cte);
    if (err_is_fail(r)) {
        assert(mapping_cte->cap.type == ObjType_Null);
        debug(SUBSYS_PAGING, "caps_copy_to_vnode: handler func returned %ld\n", r);
        return r;
    }

    /* insert mapping cap into mdb */
    assert(type_is_mapping(mapping_cte->cap.type));
    errval_t err = mdb_insert(mapping_cte);
    if (err_is_fail(err)) {
        printk(LOG_ERR, "%s: mdb_insert: %"PRIuERRV"\n", __FUNCTION__, err);
    }

    TRACE_CAP_MSG("created", mapping_cte);

    return err;
}

static inline void read_pt_entry(struct capability *pgtable, size_t slot,
                                 genpaddr_t *mapped_addr, lpaddr_t *pte,
                                 void **entry)
{
    assert(type_is_vnode(pgtable->type));

    genpaddr_t paddr = 0;
    lpaddr_t pte_;
    void *entry_;

    genpaddr_t gp = get_address(pgtable);
    lpaddr_t lp = gen_phys_to_local_phys(gp);
    lvaddr_t lv = local_phys_to_mem(lp);

    // get paddr
    union x86_64_ptable_entry *e = NULL;
    bool huge = false;
    switch (pgtable->type) {
    case ObjType_VNode_x86_64_pdpt:
        huge = true;
    case ObjType_VNode_x86_64_pml4:
    case ObjType_VNode_x86_64_pdir: {
        e = (union x86_64_ptable_entry *)lv + slot;
        if (e->large.always1) {
            if (huge) {
                paddr = (lpaddr_t)e->huge.base_addr << HUGE_PAGE_BITS;
            } else {
                paddr = (lpaddr_t)e->large.base_addr << LARGE_PAGE_BITS;
            }
        } else {
            union x86_64_pdir_entry *de =
                (union x86_64_pdir_entry *)lv + slot;
            paddr = (lpaddr_t)de->d.base_addr << BASE_PAGE_BITS;
            entry_ = de;
            pte_ = lp + slot * sizeof(union x86_64_pdir_entry);
        }
        break;
    }
    case ObjType_VNode_x86_64_ptable: {
        e = (union x86_64_ptable_entry *)lv + slot;
        paddr = (lpaddr_t)e->base.base_addr << BASE_PAGE_BITS;
        entry_ = e;
        pte_ = lp + slot * sizeof(union x86_64_ptable_entry);
        break;
    }
    default:
        assert(!"Should not get here");
    }

    if (mapped_addr) {
        *mapped_addr = paddr;
    }
    if (pte) {
        *pte = pte_;
    }
    if (entry) {
        *entry = entry_;
    }

    if (mapped_addr) {
        *mapped_addr = paddr;
    }

    if (pte) {
        *pte = pte_;
    }

    if (entry) {
        *entry = entry_;
    }
}

errval_t paging_copy_remap(struct cte *dest_vnode_cte, cslot_t dest_slot,
                           struct cte *src_cte, uintptr_t flags,
                           uintptr_t offset, uintptr_t pte_count,
                           struct cte *mapping_cte)
{
    assert(type_is_vnode(dest_vnode_cte->cap.type));
    assert(mapping_cte->cap.type == ObjType_Null);

    struct capability *src_cap  = &src_cte->cap;
    struct capability *dest_cap = &dest_vnode_cte->cap;
    mapping_handler_t handler_func = handler[dest_cap->type];

    assert(handler_func != NULL);

    cslot_t last_slot = dest_slot + pte_count;

    if (last_slot > X86_64_PTABLE_SIZE) {
        // requested map overlaps leaf page table
        printf("caps_copy_to_vnode: requested mapping spans multiple leaf page tables\n");
        return SYS_ERR_VM_RETRY_SINGLE;
    }

    size_t page_size = BASE_PAGE_SIZE;
    switch(dest_cap->type) {
        case ObjType_VNode_x86_64_ptable:
            page_size = BASE_PAGE_SIZE;
            break;
        case ObjType_VNode_x86_64_pdir:
            page_size = LARGE_PAGE_SIZE;
            break;
        case ObjType_VNode_x86_64_pdpt:
            page_size = HUGE_PAGE_SIZE;
            break;
        default:
            printf("%s: unknown dest VNode: %d\n", __FUNCTION__, dest_cap->type);
            break;
    }

    errval_t err;
    // clone existing pages
    lvaddr_t toaddr;
    toaddr = local_phys_to_mem(gen_phys_to_local_phys(get_address(src_cap))) + offset;
    genpaddr_t gpfromaddr = 0;
    lvaddr_t fromaddr = 0;

    read_pt_entry(dest_cap, dest_slot, &gpfromaddr, NULL, NULL);
    if (!gpfromaddr) {
        printf("%s: dest slot empty\n", __FUNCTION__);
        return SYS_ERR_VNODE_NOT_INSTALLED;
    }
    fromaddr = local_phys_to_mem(gen_phys_to_local_phys(gpfromaddr));
    memcpy((void*)toaddr, (void*)fromaddr, pte_count*page_size);

    err = handler_func(dest_cap, dest_slot, src_cap, flags, offset, pte_count,
                       mapping_cte);
    if (err_is_fail(err)) {
        printf("%s: handler func returned %ld\n", __FUNCTION__, err);
        if (err_no(err) == SYS_ERR_WRONG_MAPPING) {
            printk(LOG_NOTE, "dest->type = %d, src->type = %d\n",
                    dest_cap->type, src_cap->type);
        }
        memset(mapping_cte, 0, sizeof(*mapping_cte));
        return err;
    }

    /* insert mapping cap into mdb */
    assert(type_is_mapping(mapping_cte->cap.type));
    err = mdb_insert(mapping_cte);
    if (err_is_fail(err)) {
        printk(LOG_ERR, "%s: mdb_insert: %"PRIuERRV"\n", __FUNCTION__, err);
        return err;
    }

    return err;
}

__attribute__((unused))
static inline lvaddr_t get_leaf_ptable_for_vaddr(genvaddr_t vaddr)
{
    lvaddr_t root_pt = local_phys_to_mem(dcb_current->vspace);

    // get pdpt
    union x86_64_pdir_entry *pdpt = (union x86_64_pdir_entry *)root_pt + X86_64_PML4_BASE(vaddr);
    if (!pdpt->raw) { return 0; }
    genpaddr_t pdpt_gp = pdpt->d.base_addr << BASE_PAGE_BITS;
    lvaddr_t pdpt_lv = local_phys_to_mem(gen_phys_to_local_phys(pdpt_gp));
    // get pdir
    union x86_64_pdir_entry *pdir = (union x86_64_pdir_entry *)pdpt_lv + X86_64_PDPT_BASE(vaddr);
    if (!pdir->raw) { return 0; }
    genpaddr_t pdir_gp = pdir->d.base_addr << BASE_PAGE_BITS;
    lvaddr_t pdir_lv = local_phys_to_mem(gen_phys_to_local_phys(pdir_gp));
    // get ptable
    union x86_64_ptable_entry *ptable = (union x86_64_ptable_entry *)pdir_lv + X86_64_PDIR_BASE(vaddr);
    if (!ptable->raw) { return 0; }
    genpaddr_t ptable_gp = ptable->base.base_addr << BASE_PAGE_BITS;
    lvaddr_t ptable_lv = local_phys_to_mem(gen_phys_to_local_phys(ptable_gp));

    return ptable_lv;
}

size_t do_unmap(lvaddr_t pt, cslot_t slot, size_t num_pages)
{
    // iterate over affected leaf ptables
    size_t unmapped_pages = 0;
    union x86_64_ptable_entry *ptentry = (union x86_64_ptable_entry *)pt + slot;
    for (int i = 0; i < num_pages; i++) {
        ptentry++->raw = 0;
        unmapped_pages++;
    }
    return unmapped_pages;
}

static size_t ptable_type_get_page_size(enum objtype type)
{
    switch(type) {
        case ObjType_VNode_x86_64_ptable:
            return BASE_PAGE_SIZE;
        case ObjType_VNode_x86_64_pdir:
            return LARGE_PAGE_SIZE;
        case ObjType_VNode_x86_64_pdpt:
            return HUGE_PAGE_SIZE;
        case ObjType_VNode_x86_64_pml4:
            return 0;

        default:
            assert(!"Type not x86_64 vnode");
    }
    return 0;
}

/**
 * \brief modify flags of entries in `leaf_pt`.
 *
 * \arg leaf_pt the frame whose mapping should be modified
 * \arg offset the offset from the first page table entry in entries
 * \arg pages the number of pages to modify
 * \arg flags the new flags
 * \arg va_hint a user-supplied virtual address for hinting selective TLB
 *              flushing
 */
static errval_t generic_modify_flags(struct cte *leaf_pt, size_t offset,
                                     size_t pages,
                                     paging_x86_64_flags_t flags)
{
    lvaddr_t base = local_phys_to_mem(get_address(&leaf_pt->cap)) +
        offset * sizeof(union x86_64_ptable_entry);

    switch(leaf_pt->cap.type) {
        case ObjType_VNode_x86_64_ptable :
            for (int i = 0; i < pages; i++) {
                union x86_64_ptable_entry *entry =
                    (union x86_64_ptable_entry *)base + i;
                if (entry->base.present) {
                    paging_x86_64_modify_flags(entry, flags);
                }
            }
            break;
        case ObjType_VNode_x86_64_pdir :
            for (int i = 0; i < pages; i++) {
                union x86_64_ptable_entry *entry =
                    (union x86_64_ptable_entry *)base + i;
                if (entry->large.present && entry->large.always1) {
                    paging_x86_64_modify_flags_large(entry, flags);
                } else if (((union x86_64_pdir_entry*)entry)->d.present) {
                    paging_x86_64_pdir_modify_flags((union x86_64_pdir_entry*)entry, flags);
                }
            }
            break;
        case ObjType_VNode_x86_64_pdpt :
            for (int i = 0; i < pages; i++) {
                union x86_64_ptable_entry *entry =
                    (union x86_64_ptable_entry *)base + i;
                if (entry->large.present && entry->large.always1) {
                    paging_x86_64_modify_flags_huge(entry, flags);
                } else if (((union x86_64_pdir_entry*)entry)->d.present) {
                    paging_x86_64_pdir_modify_flags((union x86_64_pdir_entry*)entry, flags);
                }
            }
            break;
        case ObjType_VNode_x86_64_pml4 :
            for (int i = 0; i < pages; i++) {
                union x86_64_pdir_entry *entry =
                    (union x86_64_pdir_entry *)base + i;
                if (entry->d.present) {
                    paging_x86_64_pdir_modify_flags(entry, flags);
                }
            }
            break;
        default:
            return SYS_ERR_VNODE_TYPE;
    }

    return SYS_ERR_OK;
}

/**
 * \brief modify flags of mapping `mapping`.
 *
 * \arg mapping the mapping to modify
 * \arg offset the offset from the first page table entry in entries
 * \arg pages the number of pages to modify
 * \arg flags the new flags
 * \arg va_hint a user-supplied virtual address for hinting selective TLB
 *              flushing
 */
errval_t page_mappings_modify_flags(struct capability *mapping, size_t offset,
                                    size_t pages, size_t mflags, genvaddr_t va_hint)
{
    assert(type_is_mapping(mapping->type));
    struct Frame_Mapping *info = &mapping->u.frame_mapping;

    /* Calculate page access protection flags */
    // Get frame cap rights
    paging_x86_64_flags_t flags =
        paging_x86_64_cap_to_page_flags(info->cap->rights);
    // Mask with provided access rights mask
    flags = paging_x86_64_mask_attrs(flags, X86_64_PTABLE_ACCESS(mflags));
    // Add additional arch-specific flags
    flags |= X86_64_PTABLE_FLAGS(mflags);
    // Unconditionally mark the page present
    flags |= X86_64_PTABLE_PRESENT;

    // check arguments
    if (offset >= X86_64_PTABLE_SIZE) { // Within pagetable
        return SYS_ERR_VNODE_SLOT_INVALID;
    }
    if (offset + pages > X86_64_PTABLE_SIZE) { // mapping size ok
        return SYS_ERR_VM_MAP_SIZE;
    }

    // get pt cap to figure out page size
    struct cte *leaf_pt = info->ptable;
    if (!type_is_vnode(leaf_pt->cap.type)) {
        return SYS_ERR_VNODE_TYPE;
    }
    assert(type_is_vnode(leaf_pt->cap.type));
    // add first pte location from mapping cap to user supplied offset
    offset += info->entry;

    errval_t err;
    err = generic_modify_flags(leaf_pt, offset, pages, flags);
    if (err_is_fail(err)) {
        return err;
    }

    size_t pagesize = ptable_type_get_page_size(leaf_pt->cap.type);
    if (va_hint != 0 && va_hint > BASE_PAGE_SIZE) {
        debug(SUBSYS_PAGING,
                "selective flush: 0x%"PRIxGENVADDR"--0x%"PRIxGENVADDR"\n",
                va_hint, va_hint + pages * pagesize);
        // use as direct hint
        // invlpg should work for large/huge pages
        for (int i = 0; i < pages; i++) {
            do_one_tlb_flush(va_hint + i * pagesize);
        }
    } else if (va_hint == 1) {
        // XXX: remove this or cleanup interface, -SG, 2015-03-11
        // do computed selective flush
        debug(SUBSYS_PAGING, "computed selective flush\n");
        return paging_tlb_flush_range(cte_for_cap(mapping), offset, pages);
    } else {
        debug(SUBSYS_PAGING, "full flush\n");
        /* do full TLB flush */
        do_full_tlb_flush();
    }

    return SYS_ERR_OK;
}

errval_t ptable_modify_flags(struct capability *leaf_pt, size_t offset,
                                    size_t pages, size_t mflags)
{
    /* Calculate page access protection flags */
    // Mask with provided access rights mask
    paging_x86_64_flags_t flags = X86_64_PTABLE_USER_SUPERVISOR;
    flags = paging_x86_64_mask_attrs(flags, X86_64_PTABLE_ACCESS(mflags));
    // Add additional arch-specific flags
    flags |= X86_64_PTABLE_FLAGS(mflags);
    // Unconditionally mark the page present
    flags |= X86_64_PTABLE_PRESENT;

    // check arguments
    if (offset >= X86_64_PTABLE_SIZE) { // Within pagetable
        return SYS_ERR_VNODE_SLOT_INVALID;
    }
    if (offset + pages > X86_64_PTABLE_SIZE) { // mapping size ok
        return SYS_ERR_VM_MAP_SIZE;
    }

    errval_t err = generic_modify_flags(cte_for_cap(leaf_pt), offset, pages, flags);

    do_full_tlb_flush();

    return err;
}

bool paging_is_region_valid(lvaddr_t buffer, size_t size, uint8_t type)
{
    lvaddr_t root_pt = local_phys_to_mem(dcb_current->vspace);

    lvaddr_t end = buffer + size;

    uint16_t first_pml4e = X86_64_PML4_BASE(buffer);
    uint16_t last_pml4e = X86_64_PML4_BASE(end);

    uint16_t first_pdpte, first_pdire, first_pte;
        uint16_t last_pdpte, last_pdire, last_pte;


    union x86_64_ptable_entry *pte;
    union x86_64_pdir_entry *pde;

    for (uint16_t pml4e = first_pml4e; pml4e <= last_pml4e; pml4e++) {
        pde = (union x86_64_pdir_entry *)root_pt + pml4e;
        if (!pde->d.present) { return false; }
        if (type == ACCESS_WRITE && !pde->d.read_write) { return false; }
        // calculate which part of pdpt to check
        first_pdpte = pml4e == first_pml4e ? X86_64_PDPT_BASE(buffer) : 0;
        last_pdpte  = pml4e == last_pml4e  ? X86_64_PDPT_BASE(end)  : PTABLE_ENTRIES;
        // read pdpt base
        lvaddr_t pdpt = local_phys_to_mem((genpaddr_t)pde->d.base_addr << BASE_PAGE_BITS);
        for (uint16_t pdptidx = first_pdpte; pdptidx <= last_pdpte; pdptidx++) {
            pde = (union x86_64_pdir_entry *)pdpt + pdptidx;
            if (!pde->d.present) { return false; }
            if (type == ACCESS_WRITE && !pde->d.read_write) { return false; }
            pte = (union x86_64_ptable_entry *)pde;
            if (!pte->huge.always1) {
                // calculate which part of pdpt to check
                first_pdire = pdptidx == first_pdpte ? X86_64_PDIR_BASE(buffer) : 0;
                last_pdire  = pdptidx == last_pdpte  ? X86_64_PDIR_BASE(end)  : PTABLE_ENTRIES;
                // read pdpt base
                lvaddr_t pdir = local_phys_to_mem((genpaddr_t)pde->d.base_addr << BASE_PAGE_BITS);
                for (uint16_t pdiridx = first_pdire; pdiridx <= last_pdire; pdiridx++) {
                    pde = (union x86_64_pdir_entry *)pdir + pdiridx;
                    if (!pde->d.present) { return false; }
                    if (type == ACCESS_WRITE && !pde->d.read_write) { return false; }
                    pte = (union x86_64_ptable_entry *)pde;
                    if (!pte->large.always1) {
                        // calculate which part of pdpt to check
                        first_pte = pdiridx == first_pdire ? X86_64_PTABLE_BASE(buffer) : 0;
                        last_pte  = pdiridx == last_pdire  ? X86_64_PTABLE_BASE(end)  : PTABLE_ENTRIES;
                        // read pdpt base
                        lvaddr_t pt = local_phys_to_mem((genpaddr_t)pde->d.base_addr << BASE_PAGE_BITS);
                        for (uint16_t ptidx = first_pte; ptidx < last_pte; ptidx++) {
                            pte = (union x86_64_ptable_entry *)pt + ptidx;
                            if (!pte->base.present) { return false; }
                            if (type == ACCESS_WRITE && !pte->base.read_write) { return false; }
                        }
                    }
                }
            }
        }
    }
    // if we never bailed early, the access is fine.
    return true;
}

void paging_dump_tables_around(struct dcb *dispatcher, lvaddr_t vaddr)
{
    if (!local_phys_is_valid(dispatcher->vspace)) {
        printk(LOG_ERR, "dispatcher->vspace = 0x%"PRIxLPADDR": too high!\n" ,
               dispatcher->vspace);
        return;
    }
    lvaddr_t root_pt = local_phys_to_mem(dispatcher->vspace);

    uint16_t first_pml4e = 0, last_pml4e = X86_64_PML4_BASE(X86_64_MEMORY_OFFSET);

    if (vaddr) {
        first_pml4e = X86_64_PML4_BASE(vaddr);
        last_pml4e = first_pml4e + 1;
        printk(LOG_NOTE, "printing page tables for PML4e %hu\n", first_pml4e);
    }

    // loop over pdpts
    union x86_64_ptable_entry *pt;
    for (int pdpt_index = first_pml4e; pdpt_index < last_pml4e; pdpt_index++) {
        union x86_64_pdir_entry *pdpt = (union x86_64_pdir_entry *)root_pt + pdpt_index;
        if (!pdpt->d.present) { continue; }
        else {
            genpaddr_t paddr = (genpaddr_t)pdpt->d.base_addr << BASE_PAGE_BITS;
            printf("%d: 0x%"PRIxGENPADDR" (%d %d), raw=0x%"PRIx64"\n",
                    pdpt_index, paddr,
                    pdpt->d.read_write, pdpt->d.user_supervisor,
                    pdpt->raw);
        }
        genpaddr_t pdpt_gp = (genpaddr_t)pdpt->d.base_addr << BASE_PAGE_BITS;
        lvaddr_t pdpt_lv = local_phys_to_mem(gen_phys_to_local_phys(pdpt_gp));

        for (int pdir_index = 0; pdir_index < X86_64_PTABLE_SIZE; pdir_index++) {
            // get pdir
            union x86_64_pdir_entry *pdir = (union x86_64_pdir_entry *)pdpt_lv + pdir_index;
            pt = (union x86_64_ptable_entry*)pdir;
            if (!pdir->d.present) { continue; }
            // check if pdir or huge page
            if (pt->huge.always1) {
                // is huge page mapping
                genpaddr_t paddr = (genpaddr_t)pt->huge.base_addr << HUGE_PAGE_BITS;
                printf("%d.%d: 0x%"PRIxGENPADDR" (%d %d %d), raw=0x%"PRIx64"\n", pdpt_index,
                        pdir_index, paddr, pt->huge.read_write,
                        pt->huge.dirty, pt->huge.accessed, pt->raw);
                // goto next pdpt entry
                continue;
            } else {
                genpaddr_t paddr = (genpaddr_t)pdir->d.base_addr << BASE_PAGE_BITS;
                printf("%d.%d: 0x%"PRIxGENPADDR" (%d %d), raw=0x%"PRIx64"\n",
                        pdpt_index, pdir_index, paddr,
                        pdir->d.read_write, pdir->d.user_supervisor,
                        pdir->raw);
            }
            genpaddr_t pdir_gp = (genpaddr_t)pdir->d.base_addr << BASE_PAGE_BITS;
            lvaddr_t pdir_lv = local_phys_to_mem(gen_phys_to_local_phys(pdir_gp));

            for (int ptable_index = 0; ptable_index < X86_64_PTABLE_SIZE; ptable_index++) {
                // get ptable
                union x86_64_pdir_entry *ptable = (union x86_64_pdir_entry *)pdir_lv + ptable_index;
                pt = (union x86_64_ptable_entry *)ptable;
                if (!ptable->d.present) { continue; }
                // check if ptable or large page
                if (pt->large.always1) {
                    // is large page mapping
                    genpaddr_t paddr = (genpaddr_t)pt->large.base_addr << LARGE_PAGE_BITS;
                    printf("%d.%d.%d: 0x%"PRIxGENPADDR" (%d %d %d), raw=0x%"PRIx64"\n",
                            pdpt_index, pdir_index, ptable_index, paddr,
                            pt->large.read_write, pt->large.dirty,
                            pt->large.accessed, pt->raw);
                    // goto next pdir entry
                    continue;
                } else {
                    genpaddr_t paddr = (genpaddr_t)ptable->d.base_addr << BASE_PAGE_BITS;
                    printf("%d.%d.%d: 0x%"PRIxGENPADDR" (%d %d), raw=0x%"PRIx64"\n",
                            pdpt_index, pdir_index, ptable_index, paddr,
                            ptable->d.read_write, ptable->d.user_supervisor,
                            ptable->raw);
                }
                genpaddr_t ptable_gp = (genpaddr_t)ptable->d.base_addr << BASE_PAGE_BITS;
                lvaddr_t ptable_lv = local_phys_to_mem(gen_phys_to_local_phys(ptable_gp));

                for (int entry = 0; entry < X86_64_PTABLE_SIZE; entry++) {
                    union x86_64_ptable_entry *e =
                        (union x86_64_ptable_entry *)ptable_lv + entry;
                    if (!e->base.present) { continue; }
                    genpaddr_t paddr = (genpaddr_t)e->base.base_addr << BASE_PAGE_BITS;
                    printf("%d.%d.%d.%d: 0x%"PRIxGENPADDR" (%d %d %d), raw=0x%"PRIx64"\n", 
                            pdpt_index, pdir_index, ptable_index, entry,
                            paddr, e->base.read_write, e->base.dirty, e->base.accessed,
                            e->raw);
                }
            }
        }
    }
}

void paging_dump_tables(struct dcb *dispatcher)
{
    return paging_dump_tables_around(dispatcher, 0);
}
