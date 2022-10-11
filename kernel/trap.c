#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];
extern struct proc *queue[NPR][NPROC];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

// handle COW interrupt
int handleCOW(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;
  uint flags;
  char *mem;
  
  if (va >= MAXVA)
    return 1;

  va = PGROUNDDOWN(va);
  pte = walk(pagetable, va, 0);
  pa = PTE2PA(*pte);
  flags = PTE_FLAGS(*pte);

  if (va == 0 || (flags & PTE_COW) == 0 || pte == 0 || (flags & PTE_V) == 0 || (flags & PTE_U) == 0) {
    return 1;
  }

  memref_lock();
  memref_unlock_kalloc();
  flags = (flags & (~PTE_COW)) | PTE_W;
  int fq = memref_get((void*)pa);
  // printf("Ref: %d\n", fq);
  if (fq == 1) {
    *pte = (*pte & (~PTE_COW)) | PTE_W;
  } else {
    if ((mem = kalloc()) == 0) {
      memref_unlock();
      return 1;
    }

    memmove((void*)mem, (void*)pa, PGSIZE);
    uvmunmap(pagetable, va, 1, 0);
    if (mappages(pagetable, va, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      memref_unlock();
      return 1;
    }

    memref_set((void*)pa, fq - 1);
  }
  memref_lock_kalloc();
  memref_unlock();

  return 0;
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void usertrap(void)
{
  int i, j;
  int which_dev = 0;


  (void)i;
  (void)j;


  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();

  // save user program counter.
  p->trapframe->epc = r_sepc();

  if (r_scause() == 8)
  {
    // system call

    if (killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  }
  else if (r_scause() == 15) // 0xf is a pagefault trap (like writing to read only PTEs)
  {
    // Specification 3(COW)
    // we now allocate this page to the child process
    // and we then set the PTE_W bit
    uint64 va = r_stval();
    if (handleCOW(p->pagetable, va)) {
      setkilled(p);
    }
  }
  else if ((which_dev = devintr()) != 0)
  {
    // ok

    if (which_dev == 2) {
      struct proc *fp;
      for(fp = proc; fp < &proc[NPROC]; fp++) {
        acquire(&fp->lock);

        if (fp->state == RUNNING) fp->rtime +=1;

        release(&fp->lock);
      }
    }
    
    // Specification 1
    if (which_dev == 2 && p->alarmOn == 0)
    {
      // this is to check whether the interrupt was from the timer(look at the devintr() function on line 181)
      p->nticks += 1;

      if (p->nticks == p->interval)
      {
        struct trapframe *context = kalloc();
        memmove(context, p->trapframe, PGSIZE);
        p->alarmContext = context;
        p->alarmOn = 1; // done to prevent reentrance (test 2)
        p->trapframe->epc = p->handler;
      }
    }

    // Specification 2 (MLFQ)
    if (which_dev == 2) {
      if (p->state == RUNNING)
        p->rticks++;
      
      for(i = 0; i < NPR; i++) {
        for(j = 0; j < NPROC; j++) {
          struct proc *fp = queue[i][j];
          if (fp && fp->state == RUNNABLE) {
            // printf("------------![USER]!------------");
            fp->wticks++;
          }
        }
      }
    }
  }
  else
  {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if (killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  #ifdef RR
  if (which_dev == 2)
    yield();
  #endif
  #ifdef MLFQ
  if(which_dev == 2) {
    if (p->rticks == (1 << p->pr)) {
      // printf("int %d\n", p->rticks);
      p->rticks = 0;
      if (p->pr != P4)
        p->pr++;
      yield();
    }

    // check if higher priority process exists
    for(i = 0; i < p->pr; i++) {
      for(j = 0; j < NPROC; j++)
        if (!queue[i][j])
          break;
        
      if (j) {
        // higher priority process available (don't reset rticks)
        yield();
      }
    }
  }
  #endif
  #ifdef LOTTERY
  if (which_dev == 2)
    yield();
  #endif

  usertrapret();
}

//
// return to user space
//
void usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp(); // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.

  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap()
{
  int i, j;
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();


  (void)i;
  (void)j;


  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0)
  {
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  #ifdef RR
  if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();
  #endif
  #ifdef MLFQ
  if (which_dev == 2) {
    for(i = 0; i < NPR; i++) {
      for(j = 0; j < NPROC; j++) {
        struct proc *fp = queue[i][j];
        if (fp && fp->state == RUNNABLE) {
          // printf("------------![KERNEL]!------------");
          fp->wticks++;
        }
      }
    }
  }
  if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();
  #endif
  #ifdef LOTTERY
  if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();
  #endif

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int devintr()
{
  uint64 scause = r_scause();

  if ((scause & 0x8000000000000000L) &&
      (scause & 0xff) == 9)
  {
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if (irq == UART0_IRQ)
    {
      uartintr();
    }
    else if (irq == VIRTIO0_IRQ)
    {
      virtio_disk_intr();
    }
    else if (irq)
    {
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if (irq)
      plic_complete(irq);

    return 1;
  }
  else if (scause == 0x8000000000000001L)
  {
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if (cpuid() == 0)
    {
      clockintr();
    }

    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  }
  else
  {
    return 0;
  }
}
