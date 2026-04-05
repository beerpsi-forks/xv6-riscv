#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "mmap.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Shared memory region address
#define SHMEM_REGION 0x4000000  // 64MB mark
#define NSHMEM 64 // maximum number of MAP_SHARED regions

struct shmem_region {
  struct spinlock lock;
  int refcount;
  int used;
};
static struct shmem_region shmem_regions[NSHMEM];

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
    if(do_free)
      kfree((void*)PTE2PA(*pte));
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
    pa    = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

// Initialize the shared memory system
void
init_shmem(void)
{
  for (int i = 0; i < NSHMEM; i++) {
    initlock(&shmem_regions[i].lock, "shmem");
    shmem_regions[i].refcount = 0;
    shmem_regions[i].used = 0;
  }
}

// Grab a free shmem_region from the pool (refcount = 1).
// Returns NULL if the pool is exhausted.
static struct shmem_region *
shmem_alloc(void)
{
  for(int i = 0; i < NSHMEM; i++){
    acquire(&shmem_regions[i].lock);
    if(!shmem_regions[i].used){
      shmem_regions[i].used     = 1;
      shmem_regions[i].refcount = 1;
      release(&shmem_regions[i].lock);
      return &shmem_regions[i];
    }
    release(&shmem_regions[i].lock);
  }
  return 0;
}

// mmap regions live in [MMAP_BASE, MMAP_TOP), well above the heap.
// MMAP_TOP is just below the trapframe page (MAXVA - 2*PGSIZE).
#define MMAP_BASE ((uint64)(MAXVA / 2))
#define MMAP_TOP  ((uint64)(MAXVA - 2 * PGSIZE))

// Convert mmap protection flags to RISC-V PTE permission bits.
static int
prot_to_pte(int prot)
{
  int p = PTE_U;
  if(prot & PROT_READ)  p |= PTE_R;
  if(prot & PROT_WRITE) p |= PTE_W;
  if(prot & PROT_EXEC)  p |= PTE_X;
  return p;
}

// Return 1 if [addr, addr+length) overlaps any live VMA in p.
static int
vma_overlaps(struct proc *p, uint64 addr, uint64 length)
{
  for(int i = 0; i < NVMAS; i++){
    if(!p->vmas[i].used) continue;
    uint64 vs = p->vmas[i].addr;
    uint64 ve = vs + p->vmas[i].length;
    if(addr < ve && addr + length > vs)
      return 1;
  }
  return 0;
}

// Return a pointer to an unused VMA slot, or NULL if all are taken.
static struct vma *
vma_alloc_slot(struct proc *p)
{
  for(int i = 0; i < NVMAS; i++)
    if(!p->vmas[i].used)
      return &p->vmas[i];
  return 0;
}

// Find a free address range between MMAP_BASE and MMAP_TOP.
//
// If hint is non-zero and page-aligned, it is tried first.
// If hint is invalid or conflicts with an existing region,
// scan for a valid region starting from MMAP_TOP.
// 
// Returns an address on success, 0 on failure.
static uint64
vma_find_addr(struct proc *p, uint64 hint, uint64 length)
{
  if(hint != 0 && (hint % PGSIZE) == 0
     && hint >= MMAP_BASE && hint + length <= MMAP_TOP
     && !vma_overlaps(p, hint, length))
    return hint;

  // Scan downward in page-sized steps so we fill holes left by
  // earlier munmap() calls.
  for(uint64 a = MMAP_TOP - length; a >= MMAP_BASE; a -= PGSIZE)
    if(!vma_overlaps(p, a, length))
      return a;

  return 0;
}

// do_munmap - release one VMA from process p
//
// For MAP_SHARED: physical pages are freed only when the last
// reference drops; otherwise the PTEs are cleared with do_free=0
// so other processes keep their mappings intact.
// For MAP_PRIVATE: the physical pages are always freed.
void
do_munmap(struct proc *p, struct vma *v)
{
  uint64 npages = v->length / PGSIZE;

  if(v->flags & MAP_SHARED){
    struct shmem_region *sr = v->shared;
    acquire(&sr->lock);
    sr->refcount--;
    int do_free = (sr->refcount == 0);
    if(do_free) sr->used = 0;
    release(&sr->lock);
    // Free physical pages only on the last unmap; otherwise just
    // remove the PTEs so other processes' mappings are unaffected.
    uvmunmap(p->pagetable, v->addr, npages, do_free);
  } else {
    // Private mapping - this process owns the physical pages.
    uvmunmap(p->pagetable, v->addr, npages, 1);
  }

  v->used   = 0;
  v->shared = 0;
}

// vma_fork - called by fork() to clone a parent's VMA table
//
// MAP_SHARED: map the same physical pages into the child and
//             increment the shared region's reference count.
// MAP_PRIVATE: deep-copy every physical page into the child.
//
// Returns 0 on success.  On failure, any partially-built child
// VMAs are cleaned up here before returning -1.
int
vma_fork(struct proc *parent, struct proc *child)
{
  for(int i = 0; i < NVMAS; i++){
    child->vmas[i].used = 0;           // start clean

    if(!parent->vmas[i].used) continue;

    struct vma *pv = &parent->vmas[i];
    struct vma *cv = &child->vmas[i];
    int pte_prot   = prot_to_pte(pv->prot);

    // Copy metadata; mark unused until pages are wired up.
    cv->addr   = pv->addr;
    cv->length = pv->length;
    cv->prot   = pv->prot;
    cv->flags  = pv->flags;
    cv->shared = pv->shared;

    if(pv->flags & MAP_SHARED){
      // Bump refcount before touching the page table so that a
      // concurrent munmap() on the parent never drops count to 0
      // while we still need the pages.
      acquire(&pv->shared->lock);
      pv->shared->refcount++;
      release(&pv->shared->lock);

      // Wire the same physical frames into the child.
      for(uint64 off = 0; off < pv->length; off += PGSIZE){
        pte_t *pte = walk(parent->pagetable, pv->addr + off, 0);
        if(pte == 0 || !(*pte & PTE_V)) continue;
        if(mappages(child->pagetable, pv->addr + off,
                    PGSIZE, PTE2PA(*pte), pte_prot) != 0){
          // Undo child PTEs installed so far (do_free=0: shared).
          uvmunmap(child->pagetable, pv->addr, pv->length/PGSIZE, 0);
          acquire(&pv->shared->lock);
          if(--pv->shared->refcount == 0) pv->shared->used = 0;
          release(&pv->shared->lock);
          goto err;
        }
      }

    } else {
      // MAP_PRIVATE: allocate new physical pages and copy content.
      for(uint64 off = 0; off < pv->length; off += PGSIZE){
        pte_t *pte = walk(parent->pagetable, pv->addr + off, 0);
        if(pte == 0 || !(*pte & PTE_V)) continue;
        char *mem = kalloc();
        if(mem == 0){
          uvmunmap(child->pagetable, pv->addr, pv->length/PGSIZE, 1);
          goto err;
        }
        memmove(mem, (char*)PTE2PA(*pte), PGSIZE);
        if(mappages(child->pagetable, pv->addr + off,
                    PGSIZE, (uint64)mem, pte_prot) != 0){
          kfree(mem);
          uvmunmap(child->pagetable, pv->addr, pv->length/PGSIZE, 1);
          goto err;
        }
      }
    }

    cv->used = 1;   // VMA fully wired - now visible to do_munmap
  }
  return 0;

err:
  // Roll back every child VMA that was successfully completed.
  for(int j = 0; j < NVMAS; j++)
    if(child->vmas[j].used)
      do_munmap(child, &child->vmas[j]);
  return -1;
}

// mmap - map anonymous memory into the calling process
//
// addr   hint VA (0 = don't care); must be page-aligned if given
// length number of bytes (rounded up to PGSIZE)
// prot   PROT_READ | PROT_WRITE | PROT_EXEC (any combination)
// flags  MAP_SHARED or MAP_PRIVATE (exactly one)
//
// Returns the start VA on success, 0 on failure (MAP_FAILED).
//
// Physical pages are allocated eagerly.
uint64
mmap(uint64 addr, uint64 length, int prot, int flags)
{
  struct proc *p = myproc();

  // Exactly one of MAP_SHARED / MAP_PRIVATE must be set.
  if(!(flags & MAP_SHARED) == !(flags & MAP_PRIVATE))
    return 0;
  if(length == 0)
    return 0;
  if(addr != 0 && (addr % PGSIZE) != 0)
    return 0;

  length = PGROUNDUP(length);

  struct vma *v = vma_alloc_slot(p);
  if(v == 0)
    return 0;

  uint64 va = vma_find_addr(p, addr, length);
  if(va == 0)
    return 0;

  // Allocate a shared-region token for MAP_SHARED mappings.
  struct shmem_region *sr = 0;
  
  if(flags & MAP_SHARED){
    sr = shmem_alloc();
    if(sr == 0)
      return 0;
  }

  // Eagerly allocate and map physical pages.
  int pte_prot  = prot_to_pte(prot);
  uint64 npages = length / PGSIZE;
  for(uint64 i = 0; i < npages; i++){
    char *mem = kalloc();
    if(mem == 0)
      goto err;
    memset(mem, 0, PGSIZE);
    if(mappages(p->pagetable, va + i*PGSIZE, PGSIZE,
                (uint64)mem, pte_prot) != 0){
      kfree(mem);
      goto err;
    }
  }

  v->used   = 1;
  v->addr   = va;
  v->length = length;
  v->prot   = prot;
  v->flags  = flags;
  v->shared = sr;
  return va;

err:
  // uvmunmap skips PTEs that were never set, so passing the full
  // npages here is safe even if we failed partway through.
  uvmunmap(p->pagetable, va, npages, 1);
  if(sr != 0){
    acquire(&sr->lock);
    sr->used     = 0;
    sr->refcount = 0;
    release(&sr->lock);
  }
  return 0;
}


// munmap - remove an entire mapping created by mmap()
//
// va must be the exact start address returned by mmap().
// Partial unmapping is not supported.
// Returns 0 on success, -1 if va does not match any VMA.
int
munmap(uint64 va)
{
  struct proc *p = myproc();

  for(int i = 0; i < NVMAS; i++){
    if(p->vmas[i].used && p->vmas[i].addr == va){
      do_munmap(p, &p->vmas[i]);
      return 0;
    }
  }
  return -1;
}
