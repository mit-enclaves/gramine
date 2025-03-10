#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "common.h"
#include "host_tyche_driver.h"
#include "log.h"

pt_profile_t g_pal_pt_profile = {
  .nb_levels = x86_64_LEVELS,
  .nb_entries = PT_NB_ENTRIES,
  .masks = {PT_PTE_PAGE_MASK, PT_PMD_PAGE_MASK, PT_PGD_PAGE_MASK, PT_PML4_PAGE_MASK},
  .shifts = {PT_PTE_SHIFT, PT_PMD_SHIFT, PT_PGD_SHIFT, PT_PML4_SHIFT},
  .how = x86_64_how_visit_leaves,
  .next = x86_64_next,
};

typedef struct pt_segment_t {
  addr_t va;
  addr_t size;
  addr_t pa;
  dll_elem(struct pt_segment_t, list);
} pt_segment_t;

typedef dll_list(pt_segment_t, pt_segment_list_t);

pt_mapper_t g_pt_mapper;

// Account for the driver remappings.
pt_segment_list_t driver_remappings;

typedef struct extras_t {
  entry_t root;
  entry_t default_flags;
  entry_t flags;
} extras_t;

addr_t pa_to_va(addr_t addr, pt_profile_t* profile) {
  pt_segment_t* curr = NULL;
  dll_foreach(&driver_remappings, curr, list) {
    if (addr >= curr->pa && addr < (curr->pa + curr->size)) {
      addr_t offset = addr - curr->pa;
      return curr->va + offset;
    }
  }
  log_error("Unable to find va for pa: 0x%llx", addr);
  assert(0);
}

addr_t va_to_pa(addr_t addr, pt_profile_t* profile) {
  pt_segment_t* curr = NULL;
  dll_foreach(&driver_remappings, curr, list) {
    if (addr >= curr->va && addr < (curr->va + curr->size)) {
      addr_t offset = addr - curr->va;
      return curr->pa + offset;
    }
  }
  log_error("Unable to find pa for va: 0x%llx", addr);
  assert(0);
}

/*
addr_t pa_to_va(addr_t addr, pt_profile_t* profile) {
  return addr - ((addr_t)g_pt_mapper.paddr_start) + ((addr_t) g_pt_mapper.vaddr_start);
}

addr_t va_to_pa(addr_t addr, pt_profile_t* profile) {
  assert(addr >= ((addr_t)g_pt_mapper.vaddr_start));
  return addr - ((addr_t)g_pt_mapper.vaddr_start) + ((addr_t) g_pt_mapper.paddr_start);
}
*/

/// Stupid bump allocator.
entry_t* alloc(void* ptr) {
  assert(g_pt_mapper.vaddr_next_free < g_pt_mapper.vaddr_end);
  entry_t* allocation = (entry_t*) g_pt_mapper.vaddr_next_free;
  g_pt_mapper.vaddr_next_free += PT_PAGE_SIZE;
  return (entry_t*) va_to_pa((addr_t)allocation, ptr);
}

static int find_paddr(addr_t vaddr, addr_t* phys) {
  assert(phys != NULL);
  if (backend_td_virt_to_phys(g_pt_mapper.domain, vaddr, phys) != SUCCESS) {
    goto failure;
  }
  return SUCCESS;
failure:
  log_error("Unable to find phys address for %llx", vaddr);
  return FAILURE;
}

callback_action_t pte_page_mapper(entry_t* curr, level_t level, pt_profile_t* profile)
{
  assert(curr != NULL);
  assert(profile != NULL);
  assert((*curr & PT_PP) == 0);
  assert(profile->extras !=NULL);
  extras_t* extra = (extras_t*)(profile->extras);
  entry_t flags = (level == PT_PTE)? extra->flags : extra->default_flags;
  entry_t addr = 0xdeadbeef;
  if (level == PT_PTE) {
    if (find_paddr(profile->curr_va, &addr) != SUCCESS) {
      log_error("Failed to do a mapping");
      assert(0);
      return  WALK;
    }
    if (profile->curr_va == 0x7fde2000) {
      log_error("vaddr: 0x%llx -> paddr: 0x%llx", profile->curr_va, addr);
    }
  } else {
   addr = profile->allocate(NULL);
  }
  *curr = (addr & PT_PHYS_PAGE_MASK) | flags;
  return WALK; 
}

static void add_segment(addr_t va, addr_t size, addr_t pa)
{
  pt_segment_t *segment = malloc(sizeof(pt_segment_t));
  if (segment == NULL) {
    log_error("failed allocation.");
    assert(0);
  }
  memset(segment, 0, sizeof(pt_segment_t));
  dll_init_elem(segment, list);
  segment->va = va;
  segment->size = size;
  segment->pa = pa;
  dll_add(&driver_remappings, segment, list);
}

void init_pt_mapper(tyche_domain_t* domain)
{
  addr_t curr_va = 0, start_va = 0, start_pa = 0, size = 0;
  /* sanity checks*/
  assert(domain != NULL);
  assert(domain->mmaps.tail != NULL);
  assert(domain->config.page_table_root == domain->mmaps.tail->physoffset); 

  /* extend the profile. */
  g_pal_pt_profile.how = x86_64_how_map;
  g_pal_pt_profile.mappers[PT_PTE] = pte_page_mapper;
  g_pal_pt_profile.mappers[PT_PMD] = pte_page_mapper;
  g_pal_pt_profile.mappers[PT_PGD] = pte_page_mapper;
  g_pal_pt_profile.mappers[PT_PML4] = pte_page_mapper;
  g_pal_pt_profile.allocate = alloc; 
  g_pal_pt_profile.pa_to_va = pa_to_va;
  g_pal_pt_profile.va_to_pa = va_to_pa;

  /* init the mapper.*/
  memset(&g_pt_mapper, 0, sizeof(pt_mapper_t));
  g_pt_mapper.vaddr_start = domain->mmaps.tail->virtoffset;
  /* The first page is for the root. */
  g_pt_mapper.vaddr_next_free = g_pt_mapper.vaddr_start + PT_PAGE_SIZE;
  g_pt_mapper.vaddr_end = g_pt_mapper.vaddr_start + domain->mmaps.tail->size;
  g_pt_mapper.paddr_start = domain->mmaps.tail->physoffset;
  g_pt_mapper.domain = domain;

  // Capture the remappings.
  dll_init_list(&driver_remappings);

  start_va = g_pt_mapper.vaddr_start;
  if (find_paddr(start_va, &start_pa) != SUCCESS) {
    log_error("Unable to find the start address for the pt mapper");
    assert(0);
  }

  curr_va = g_pt_mapper.vaddr_start + PT_PAGE_SIZE;
  size = PT_PAGE_SIZE;
  for (; curr_va < g_pt_mapper.vaddr_end; curr_va += PT_PAGE_SIZE) {
    addr_t phys_curr = 0;
    if (find_paddr(curr_va, &phys_curr) != SUCCESS) {
      log_error("Unable to find phys addr for 0x%llx", curr_va);
      assert(0);
    }
    // Split
    if (phys_curr != (start_pa + size)) {
      assert(start_va + size == curr_va);
      add_segment(start_va, size, start_pa);
      // Update the start_va and reset the size and pa.
      start_va = curr_va;
      size = PT_PAGE_SIZE;
      start_pa = phys_curr;
      continue;
    }
    // Contiguous, let's keep going.
    size += PT_PAGE_SIZE;
  }
  // Add the last segment.
  add_segment(start_va, size, start_pa);
}

int pt_map_region(uint64_t vaddr, uint64_t size, uint64_t flags)
{
  extras_t extras = {
    .root = g_pt_mapper.domain->config.page_table_root,
    .default_flags = PT_PP | PT_USR | PT_RW,
    .flags = flags,
  };
  g_pal_pt_profile.extras = (void*) &extras;
  if (pt_walk_page_range(extras.root, PT_PML4, vaddr, vaddr+size, &g_pal_pt_profile) != 0) {
    log_error("Error mapping address range..");
    assert(0);
  }
  g_pal_pt_profile.extras = NULL;
  return SUCCESS;
}
