#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc(); //创建一个页表空间
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

pagetable_t
kvminit_for_everyproc()
{
  pagetable_t kpagetable = (pagetable_t) kalloc(); //创建一个页表空间
  memset(kpagetable, 0, PGSIZE);

  // uart registers
  ukvmmap(kpagetable ,UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  ukvmmap(kpagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  ukvmmap(kpagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  ukvmmap(kpagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  ukvmmap(kpagetable,KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  ukvmmap(kpagetable, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  ukvmmap(kpagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  return kpagetable;
}



// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.  一个PET大小是8个字节包括PPN以及一下标志位和一些
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc) //alloc的作用？
{
  //这个函数里面操作的地址都是虚拟内存，由于内核虚拟空间中除了最上面的内核栈
  //以外其他的虚拟地址全部和物理地址保持一致。
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)]; //根据第几级的level中的9个数直接对应到pte
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {//如果无效的话
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0) //创建4096个字节的地址 刚好是8 * 512
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)]; //返回的是最后一级页表PTE地址
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

  pte = walk(pagetable, va, 0); //不能够构建
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa; //返回的是PPN 还没有加上偏移？
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.

void
ukvmmap(pagetable_t kpagetable, uint64 va, uint64 pa, uint64 sz, int perm){
  if(mappages(kpagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}


void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(myproc()->kenerl_page_table, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
//根据传入的虚拟地址和物理地址来构建映射。
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
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
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0; //该pte置位无效
  }
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

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
//为什么要重新创建一个页然后把代码拷贝进来，而不是直接映射代码存在的物理页？难道代码不在物理内存中吗

void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc(); //kalloc返回的是物理地址
  memset(mem, 0, PGSIZE); //代码怎么能知道传入的是物理地址还是虚拟地址？内核中地址一致
  //构建页表映射其实就是构建不同的虚拟地址空间，给用户页表中添加映射就是在构建虚拟地址空间
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz); //跳到下一页的虚拟地址空间的初始位置
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
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

void
only_freepage(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if(pte & PTE_V)
    {
      pagetable[i] = 0;
      if((pte & (PTE_R | PTE_X | PTE_W)) == 0)
      {
        uint64 child = PTE2PA(pte);
        only_freepage((pagetable_t)child);
      }

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
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
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


// void
// uvmpagecopy(pagetable_t upagetable, pagetable_t kpagetable, uint64 oldsz, uint64 newsz)
// {
//   oldsz = PGROUNDUP(oldsz);
//   for(uint64 i = oldsz; i < newsz; i += PGSIZE){
//     pte_t* opte = walk(upagetable, i, 0);
//     pte_t* npte = walk(kpagetable, i, 1);
//     if(opte == 0) panic("u2kvmcopy: src pte do not exist");
//     if(npte == 0) panic("u2kvmcopy: dest pte walk fail");
//     uint64 pa = PTE2PA(*opte);
//     uint flag = (PTE_FLAGS(*opte)) & (~PTE_U);
//     *npte = PA2PTE(pa) | flag;
//   }
//   return;
// }

void
uvmpagecopy(pagetable_t pagetable, pagetable_t kpagetable, uint64 oldsz, uint64 newsz)
{
  pte_t *pte_from, *pte_to;
  uint64 a, pa;
  uint flags;

  if (newsz < oldsz)
    return;
  
  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    if ((pte_from = walk(pagetable, a, 0)) == 0)
      panic("u2kvmcopy: pte should exist");
    if ((pte_to = walk(kpagetable, a, 1)) == 0)
      panic("u2kvmcopy: walk fails");
    pa = PTE2PA(*pte_from);
    // 清除PTE_U的标记位
    flags = (PTE_FLAGS(*pte_from) & (~PTE_U));
    *pte_to = PA2PTE(pa) | flags;
  }
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

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
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
extern int copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
extern int copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);


// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
// int
// copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
// {
//   uint64 n, va0, pa0;

//   while(len > 0){
//     va0 = PGROUNDDOWN(srcva);
//     pa0 = walkaddr(pagetable, va0);
//     if(pa0 == 0)
//       return -1;
//     n = PGSIZE - (srcva - va0); //剩下多少？
//     if(n > len)
//       n = len;
//     memmove(dst, (void *)(pa0 + (srcva - va0)), n);

//     len -= n;
//     dst += n;
//     srcva = va0 + PGSIZE;
//   }
//   return 0;
// }

int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len){
  return copyin_new(pagetable,dst, srcva,len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
// int
// copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
// {
//   uint64 n, va0, pa0;
//   int got_null = 0;

//   while(got_null == 0 && max > 0){
//     va0 = PGROUNDDOWN(srcva);
//     pa0 = walkaddr(pagetable, va0);
//     if(pa0 == 0)
//       return -1;
//     n = PGSIZE - (srcva - va0);
//     if(n > max)
//       n = max;

//     char *p = (char *) (pa0 + (srcva - va0));
//     while(n > 0){
//       if(*p == '\0'){
//         *dst = '\0';
//         got_null = 1;
//         break;
//       } else {
//         *dst = *p;
//       }
//       --n;
//       --max;
//       p++;
//       dst++;
//     }

//     srcva = va0 + PGSIZE;
//   }
//   if(got_null){
//     return 0;
//   } else {
//     return -1;
//   }
// }

int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max){
  return copyinstr_new(pagetable, dst, srcva, max);
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", (uint64)pagetable);
  for(int i = 0; i < 512; i++){
    pte_t pte_1 = pagetable[i];
    if((pte_1 & PTE_V )== 0){
      continue;
    }
    printf(".. %d: pte %p pa %p\n", i, (uint64)pte_1, PTE2PA(pte_1));
    for(int j = 0; j < 512; j++){
      pte_t pte_2 = ((pagetable_t)PTE2PA(pte_1))[j];
      if((pte_2 & PTE_V) == 0){
        continue;
      }
      printf(".. .. %d: pte %p pa %p\n", j, (uint64)pte_2, PTE2PA(pte_2));
      for(int k = 0; k < 512; k++){
        pte_t pte_3 = ((pagetable_t)PTE2PA(pte_2))[k];
        if((pte_3 & PTE_V) == 0){
          continue;
        }
        printf(".. .. ..%d: pte %p pa %p\n", k, (uint64)pte_3, PTE2PA(pte_3));
      }
    }

  }
}




