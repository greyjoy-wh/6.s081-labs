#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

//sys_trace()
uint64
sys_trace(void)
{
  if(argint(0, &(myproc()->target_syscall)) < 0)
    return -1;
  return 0;
}

extern int count_free_memry(void);
extern int count_unused_proc(void);

uint64
sys_sysinfo(void)
{
  //在内核中计算出空闲的内存空间以及进程的数量
  //返回给用户态。
  struct proc *p = myproc();
  int free_page_count = count_free_memry();
  int unused_proc_count = count_unused_proc();
  uint64 des;
  if(argaddr(0, &des) < 0)
    return -1;
  struct sysinfo info;
  info.freemem = (uint64)free_page_count * 4096;
  info.nproc = unused_proc_count;
  if(copyout(p->pagetable, des, (char*)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}



