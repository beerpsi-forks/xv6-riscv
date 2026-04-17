#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch(n){
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);
extern uint64 sys_getprocs(void);
extern uint64 sys_trace(void);
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);

// An array mapping syscall numbers from syscall.h
// to the function that handles the system call.
static uint64 (*syscalls[])(void) = {
  [SYS_fork]    sys_fork,
  [SYS_exit]    sys_exit,
  [SYS_wait]    sys_wait,
  [SYS_pipe]    sys_pipe,
  [SYS_read]    sys_read,
  [SYS_kill]    sys_kill,
  [SYS_exec]    sys_exec,
  [SYS_fstat]   sys_fstat,
  [SYS_chdir]   sys_chdir,
  [SYS_dup]     sys_dup,
  [SYS_getpid]  sys_getpid,
  [SYS_sbrk]    sys_sbrk,
  [SYS_pause]   sys_pause,
  [SYS_uptime]  sys_uptime,
  [SYS_open]    sys_open,
  [SYS_write]   sys_write,
  [SYS_mknod]   sys_mknod,
  [SYS_unlink]  sys_unlink,
  [SYS_link]    sys_link,
  [SYS_mkdir]   sys_mkdir,
  [SYS_close]   sys_close,
  [SYS_getprocs] sys_getprocs,
  [SYS_trace]   sys_trace,
  [SYS_mmap]    sys_mmap,
  [SYS_munmap]  sys_munmap,
};

// Array of syscall names indexed by syscall number.
static char *syscall_names[] = {
[SYS_fork]    "fork",
[SYS_exit]    "exit",
[SYS_wait]    "wait",
[SYS_pipe]    "pipe",
[SYS_read]    "read",
[SYS_kill]    "kill",
[SYS_exec]    "exec",
[SYS_fstat]   "fstat",
[SYS_chdir]   "chdir",
[SYS_dup]     "dup",
[SYS_getpid]  "getpid",
[SYS_sbrk]    "sbrk",
[SYS_pause]   "pause",
[SYS_uptime]  "uptime",
[SYS_open]    "open",
[SYS_write]   "write",
[SYS_mknod]   "mknod",
[SYS_unlink]  "unlink",
[SYS_link]    "link",
[SYS_mkdir]   "mkdir",
[SYS_close]   "close",
[SYS_getprocs] "getprocs",
[SYS_trace]   "trace",
[SYS_mmap]    "mmap",
[SYS_munmap]  "munmap",
};

// Argument type tags.
#define ARG_NONE 0   // no argument (end of list)
#define ARG_INT  1   // integer / fd
#define ARG_PTR  2   // raw pointer (printed as hex)
#define ARG_STR  3   // pointer to null-terminated string

// Per-syscall argument descriptors.
// Each entry is a 0-terminated array of ARG_* tags, at most 6 args.
static char syscall_args[][6] = {
  [SYS_fork]    = {ARG_NONE},
  [SYS_exit]    = {ARG_INT,  ARG_NONE},
  [SYS_wait]    = {ARG_PTR,  ARG_NONE},
  [SYS_pipe]    = {ARG_PTR,  ARG_NONE},
  [SYS_read]    = {ARG_INT,  ARG_PTR,  ARG_INT,  ARG_NONE},
  [SYS_kill]    = {ARG_INT,  ARG_NONE},
  [SYS_exec]    = {ARG_STR,  ARG_PTR,  ARG_NONE},
  [SYS_fstat]   = {ARG_INT,  ARG_PTR,  ARG_NONE},
  [SYS_chdir]   = {ARG_STR,  ARG_NONE},
  [SYS_dup]     = {ARG_INT,  ARG_NONE},
  [SYS_getpid]  = {ARG_NONE},
  [SYS_sbrk]    = {ARG_INT,  ARG_INT,  ARG_NONE},
  [SYS_pause]   = {ARG_INT,  ARG_NONE},
  [SYS_uptime]  = {ARG_NONE},
  [SYS_open]    = {ARG_STR,  ARG_INT,  ARG_NONE},
  [SYS_write]   = {ARG_INT,  ARG_PTR,  ARG_INT,  ARG_NONE},
  [SYS_mknod]   = {ARG_STR,  ARG_INT,  ARG_INT,  ARG_NONE},
  [SYS_unlink]  = {ARG_STR,  ARG_NONE},
  [SYS_link]    = {ARG_STR,  ARG_STR,  ARG_NONE},
  [SYS_mkdir]   = {ARG_STR,  ARG_NONE},
  [SYS_close]   = {ARG_INT,  ARG_NONE},
  [SYS_getprocs]= {ARG_PTR,  ARG_NONE},
  [SYS_trace]   = {ARG_INT,  ARG_NONE},
  [SYS_mmap]    = {ARG_PTR,  ARG_INT,  ARG_INT,  ARG_INT,  ARG_INT,  ARG_INT},
  [SYS_munmap]  = {ARG_PTR,  ARG_INT,  ARG_NONE},
};

// Print arguments for syscall number `num`.
// Must be called BEFORE the syscall executes (raw registers still intact).
static void
print_syscall_args(int num)
{
  char strbuf[64];
  char *args = syscall_args[num];
  int first = 1;

  printf("(");
  for(int i = 0; i < 6 && args[i] != ARG_NONE; i++){
    uint64 val = argraw(i);
    if(!first)
      printf(", ");
    first = 0;

    switch(args[i]){
    case ARG_INT:
      printf("%d", (int)val);
      break;
    case ARG_PTR:
      printf("%p", (void*)val);
      break;
    case ARG_STR:
      if(fetchstr(val, strbuf, sizeof(strbuf)) < 0)
        printf("%p", (void*)val);   // fallback if fail copying
      else
        printf("\"%s\"", strbuf);
      break;
    }
  }
  printf(")");
}

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // If this syscall is being traced, print name + args BEFORE executing
    // (registers a0-a5 still hold raw arguments at this point).
    if((1 << num) & p->trace_mask) {
      printf("%d: syscall %s", p->pid, syscall_names[num]);
      print_syscall_args(num);
    }

    // Use num to lookup the system call function for num, call it,
    // and store its return value in p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();

    // Print return value after execution.
    if((1 << num) & p->trace_mask) {
      printf(" -> %ld\n", p->trapframe->a0);
    }
  } else {
    printf("%d %s: unknown sys call %d\n", p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
