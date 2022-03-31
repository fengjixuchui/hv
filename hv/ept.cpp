#include "ept.h"
#include "mm.h"
#include "arch.h"
#include "vcpu.h"

namespace hv {

// identity-map the EPT paging structures
void prepare_ept(vcpu_ept_data& ept) {
  memset(&ept, 0, sizeof(ept));

  // setup the first PML4E so that it points to our PDPT
  auto& pml4e             = ept.pml4[0];
  pml4e.flags             = 0;
  pml4e.read_access       = 1;
  pml4e.write_access      = 1;
  pml4e.execute_access    = 1;
  pml4e.accessed          = 0;
  pml4e.user_mode_execute = 1;
  pml4e.page_frame_number = MmGetPhysicalAddress(&ept.pdpt).QuadPart >> 12;

  // MTRR data for setting memory types
  auto const mtrrs = read_mtrr_data();

  // TODO: allocate a PT for the fixed MTRRs region so that we can get
  // more accurate memory typing in that area (as opposed to just
  // mapping the whole PDE as UC).

  for (size_t i = 0; i < ept_pd_count; ++i) {
    // point each PDPTE to the corresponding PD
    auto& pdpte             = ept.pdpt[i];
    pdpte.flags             = 0;
    pdpte.read_access       = 1;
    pdpte.write_access      = 1;
    pdpte.execute_access    = 1;
    pdpte.accessed          = 0;
    pdpte.user_mode_execute = 1;
    pdpte.page_frame_number = MmGetPhysicalAddress(&ept.pds[i]).QuadPart >> 12;

    for (size_t j = 0; j < 512; ++j) {
      // identity-map every GPA to the corresponding HPA
      auto& pde             = ept.pds_2mb[i][j];
      pde.flags             = 0;
      pde.read_access       = 1;
      pde.write_access      = 1;
      pde.execute_access    = 1;
      pde.ignore_pat        = 0;
      pde.large_page        = 1;
      pde.accessed          = 0;
      pde.dirty             = 0;
      pde.user_mode_execute = 1;
      pde.suppress_ve       = 0;
      pde.page_frame_number = (i << 9) + j;
      pde.memory_type       = calc_mtrr_mem_type(mtrrs,
        pde.page_frame_number << 21, 0x1000 << 9);
    }
  }
}

// update the memory types in the EPT paging structures based on the MTRRs.
// this function should only be called from root-mode during vmx-operation.
void update_ept_memory_type(vcpu_ept_data& ept) {
  // TODO: completely virtualize the guest MTRRs
  auto const mtrrs = read_mtrr_data();

  for (size_t i = 0; i < ept_pd_count; ++i) {
    for (size_t j = 0; j < 512; ++j) {
      auto& pde = ept.pds_2mb[i][j];

      // 2MB large page
      if (pde.large_page) {
        // update the memory type for this PDE
        pde.memory_type = calc_mtrr_mem_type(mtrrs,
          pde.page_frame_number << 21, 0x1000 << 9);
      }
      // PDE points to a PT
      else {
        auto pt = reinterpret_cast<ept_pte*>(host_physical_memory_base
          + (ept.pds[i][j].page_frame_number << 12));

        // update the memory type for every PTE
        for (size_t k = 0; k < 512; ++k) {
          pt[k].memory_type = calc_mtrr_mem_type(mtrrs,
            pt[k].page_frame_number << 12, 0x1000);
        }
      }
    }
  }
}

// get the corresponding EPT PDPTE for a given physical address
ept_pdpte* get_ept_pdpte(vcpu_ept_data& ept, uint64_t const physical_address) {
  pml4_virtual_address const addr = { reinterpret_cast<void*>(physical_address) };

  if (addr.pml4_idx != 0)
    return nullptr;

  if (addr.pdpt_idx >= ept_pd_count)
    return nullptr;

  return &ept.pdpt[addr.pdpt_idx];
}

// get the corresponding EPT PDE for a given physical address
ept_pde* get_ept_pde(vcpu_ept_data& ept, uint64_t const physical_address) {
  pml4_virtual_address const addr = { reinterpret_cast<void*>(physical_address) };

  if (addr.pml4_idx != 0)
    return nullptr;

  if (addr.pdpt_idx >= ept_pd_count)
    return nullptr;

  return &ept.pds[addr.pdpt_idx][addr.pd_idx];
}

// get the corresponding EPT PTE for a given physical address
ept_pte* get_ept_pte(vcpu_ept_data& ept, uint64_t const physical_address) {
  pml4_virtual_address const addr = { reinterpret_cast<void*>(physical_address) };

  if (addr.pml4_idx != 0)
    return nullptr;

  if (addr.pdpt_idx >= ept_pd_count)
    return nullptr;

  auto const& pde_2mb = ept.pds_2mb[addr.pdpt_idx][addr.pd_idx];

  if (pde_2mb.large_page)
    return nullptr;

  auto const pt = reinterpret_cast<ept_pte*>(host_physical_memory_base
    + (ept.pds[addr.pdpt_idx][addr.pd_idx].page_frame_number << 12));

  return &pt[addr.pt_idx];
}

} // namespace hv

