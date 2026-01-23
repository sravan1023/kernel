/* syscall.c - System call implementation */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"
#include "../include/memory.h"

#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define SYS_CREATE      1
#define SYS_KILL        2 
#define SYS_GETPID      3
#define SYS_SUSPEND     4
#define SYS_RESUME      5
#define SYS_YIELD       6
#define SYS_SLEEP       7       
#define SYS_SLEEPMS     8       
#define SYS_EXIT        9       
#define SYS_WAIT        10 
#define SYS_GETPRIO     11 
#define SYS_SETPRIO     12

/* Memory system calls */
#define SYS_GETMEM      20
#define SYS_FREEMEM     21
#define SYS_GETSTK      22
#define SYS_FREESTK     23

/* Semaphore system calls */
#define SYS_SEMCREATE   30
#define SYS_SEMDELETE   31
#define SYS_WAIT_SEM    32
#define SYS_SIGNAL      33
#define SYS_SIGNALN     34
#define SYS_SEMCOUNT    35

/* I/O system calls */
#define SYS_READ        40
#define SYS_WRITE       41
#define SYS_OPEN        42
#define SYS_CLOSE       43
#define SYS_SEEK        44
#define SYS_IOCTL       45
#define SYS_GETC        46
#define SYS_PUTC        47
/* Message passing system calls */
#define SYS_SEND        50
#define SYS_RECEIVE     51      
#define SYS_RECVCLR     52      
#define SYS_RECVTIME    53 

/* Time system calls */
#define SYS_GETTIME     60 
#define SYS_GETTICKS    61
#define SYS_GETUPTIME   62

/* System control */
#define SYS_SHUTDOWN    70 
#define SYS_REBOOT      71

/* Maximum system call number */
#define NSYSCALLS       128

typedef int32_t (*syscall_handler_t)(void *args);

typedef struct {
    syscall_handler_t   handler; 
    const char          *name;
    uint8_t             nargs;
    bool                enabled;
} syscall_entry_t;

static syscall_entry_t syscall_table[NSYSCALLS];

static struct {
    uint64_t total_calls;
    uint64_t calls[NSYSCALLS];
    uint64_t errors;
} syscall_stats;

/*  System Call Argument Access */

typedef struct {
    uint32_t arg[8];    /* Up to 8 arguments */
} syscall_args_t;

/*  System Call Handlers (Process) */

static int32_t sys_create(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    void *funcaddr = (void *)a->arg[0];
    uint32_t ssize = a->arg[1];
    pri16 priority = (pri16)a->arg[2];
    const char *name = (const char *)a->arg[3];
    uint32_t nargs = a->arg[4];
    /* Additional args would be in a->arg[5..] */
    
    return create(funcaddr, ssize, priority, name, nargs);
}

static int32_t sys_kill(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    
    return kill(pid);
}

static int32_t sys_getpid(void *args) {
    (void)args;
    return getpid();
}

static int32_t sys_suspend(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    
    return suspend(pid);
}

static int32_t sys_resume(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    
    return resume(pid);
}

static int32_t sys_yield(void *args) {
    (void)args;
    yield();
    return OK;
}

static int32_t sys_sleep(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    uint32_t delay = a->arg[0];
    
    return sleep(delay);
}

static int32_t sys_sleepms(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    uint32_t msec = a->arg[0];
    
    return sleepms(msec);
}

static int32_t sys_exit(void *args) {
    (void)args;
    /* Kill the current process */
    kill(getpid());
    /* Should not return */
    return OK;
}

static int32_t sys_getprio(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    
    return getprio(pid);
}

static int32_t sys_setprio(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    pri16 newprio = (pri16)a->arg[1];
    
    return chprio(pid, newprio);
}

/*  System Call Handlers (Memory) */

static int32_t sys_getmem(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    uint32_t nbytes = a->arg[0];
    
    return (int32_t)getmem(nbytes);
}

static int32_t sys_freemem(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    void *block = (void *)a->arg[0];
    uint32_t nbytes = a->arg[1];
    
    return freemem(block, nbytes);
}

/*  System Call Handlers (Semaphore) */

static int32_t sys_semcreate(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    int32_t count = (int32_t)a->arg[0];
    
    return semcreate(count);
}

static int32_t sys_semdelete(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    
    return semdelete(sem);
}

static int32_t sys_wait_sem(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    
    return wait(sem);
}

static int32_t sys_signal(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    
    return signal(sem);
}

static int32_t sys_signaln(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    int32_t count = (int32_t)a->arg[1];
    
    return signaln(sem, count);
}

static int32_t sys_semcount(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    
    return semcount(sem);
}

/* System Call Handlers (Message Passing) */

static int32_t sys_send(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    umsg32 msg = (umsg32)a->arg[1];
    
    return send(pid, msg);
}

static int32_t sys_receive(void *args) {
    (void)args;
    return receive();
}

static int32_t sys_recvclr(void *args) {
    (void)args;
    return recvclr();
}

static int32_t sys_recvtime(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    uint32_t timeout = a->arg[0];
    
    return recvtime(timeout);
}

/* System Call Handlers (Time) */

static int32_t sys_gettime(void *args) {
    (void)args;
    return gettime();
}

static int32_t sys_getticks(void *args) {
    (void)args;
    return (int32_t)(getticks() & 0xFFFFFFFF);
}

static int32_t sys_shutdown(void *args) {
    (void)args;
    shutdown();
    return OK;
}

static int32_t sys_reboot(void *args) {
    (void)args;
    reboot();
    return OK;
}

/* System Call Initialization */

void syscall_init(void) {
    int i;
    
    /* Clear syscall table */
    for (i = 0; i < NSYSCALLS; i++) {
        syscall_table[i].handler = NULL;
        syscall_table[i].name = "unused";
        syscall_table[i].nargs = 0;
        syscall_table[i].enabled = false;
    }
    
    /* Clear statistics */
    syscall_stats.total_calls = 0;
    syscall_stats.errors = 0;
    for (i = 0; i < NSYSCALLS; i++) {
        syscall_stats.calls[i] = 0;
    }
    
    /* Register process syscalls */
    syscall_register(SYS_CREATE, sys_create, "create", 5);
    syscall_register(SYS_KILL, sys_kill, "kill", 1);
    syscall_register(SYS_GETPID, sys_getpid, "getpid", 0);
    syscall_register(SYS_SUSPEND, sys_suspend, "suspend", 1);
    syscall_register(SYS_RESUME, sys_resume, "resume", 1);
    syscall_register(SYS_YIELD, sys_yield, "yield", 0);
    syscall_register(SYS_SLEEP, sys_sleep, "sleep", 1);
    syscall_register(SYS_SLEEPMS, sys_sleepms, "sleepms", 1);
    syscall_register(SYS_EXIT, sys_exit, "exit", 0);
    syscall_register(SYS_GETPRIO, sys_getprio, "getprio", 1);
    syscall_register(SYS_SETPRIO, sys_setprio, "chprio", 2);
    
    /* Register memory syscalls */
    syscall_register(SYS_GETMEM, sys_getmem, "getmem", 1);
    syscall_register(SYS_FREEMEM, sys_freemem, "freemem", 2);
    
    /* Register semaphore syscalls */
    syscall_register(SYS_SEMCREATE, sys_semcreate, "semcreate", 1);
    syscall_register(SYS_SEMDELETE, sys_semdelete, "semdelete", 1);
    syscall_register(SYS_WAIT_SEM, sys_wait_sem, "wait", 1);
    syscall_register(SYS_SIGNAL, sys_signal, "signal", 1);
    syscall_register(SYS_SIGNALN, sys_signaln, "signaln", 2);
    syscall_register(SYS_SEMCOUNT, sys_semcount, "semcount", 1);
    
    /* Register message syscalls */
    syscall_register(SYS_SEND, sys_send, "send", 2);
    syscall_register(SYS_RECEIVE, sys_receive, "receive", 0);
    syscall_register(SYS_RECVCLR, sys_recvclr, "recvclr", 0);
    syscall_register(SYS_RECVTIME, sys_recvtime, "recvtime", 1);
    
    /* Register time syscalls */
    syscall_register(SYS_GETTIME, sys_gettime, "gettime", 0);
    syscall_register(SYS_GETTICKS, sys_getticks, "getticks", 0);
    
    /* Register system control syscalls */
    syscall_register(SYS_SHUTDOWN, sys_shutdown, "shutdown", 0);
    syscall_register(SYS_REBOOT, sys_reboot, "reboot", 0);
}

syscall syscall_register(int num, syscall_handler_t handler,
                         const char *name, uint8_t nargs) {
    if (num < 0 || num >= NSYSCALLS) {
        return SYSERR;
    }
    
    if (handler == NULL) {
        return SYSERR;
    }
    
    syscall_table[num].handler = handler;
    syscall_table[num].name = name;
    syscall_table[num].nargs = nargs;
    syscall_table[num].enabled = true;
    
    return OK;
}

syscall syscall_unregister(int num) {
    if (num < 0 || num >= NSYSCALLS) {
        return SYSERR;
    }
    
    syscall_table[num].handler = NULL;
    syscall_table[num].name = "unused";
    syscall_table[num].nargs = 0;
    syscall_table[num].enabled = false;
    
    return OK;
}

/* System Call Dispatch */

int32_t syscall_dispatch(int num, void *args) {
    int32_t result;

    if (num < 0 || num >= NSYSCALLS) {
        syscall_stats.errors++;
        return SYSERR;
    }

    if (!syscall_table[num].enabled || syscall_table[num].handler == NULL) {
        syscall_stats.errors++;
        return SYSERR;
    }

    syscall_stats.total_calls++;
    syscall_stats.calls[num]++;
    
    result = syscall_table[num].handler(args);
    
    return result;
}

void syscall_handler(void) {
    /* Architecture-specific:
     * 
     * x86: syscall number in EAX, args in EBX, ECX, EDX, ESI, EDI
     * ARM: syscall number in R7, args in R0-R5
     * RISC-V: syscall number in A7, args in A0-A5
     * 
     * Implementation would extract these into syscall_args_t
     * and call syscall_dispatch().
     */
}

const char *syscall_name(int num) {
    if (num < 0 || num >= NSYSCALLS) {
        return NULL;
    }
    
    return syscall_table[num].name;
}


int64_t syscall_count(int num) {
    if (num < 0 || num >= NSYSCALLS) {
        return -1;
    }
    
    return syscall_stats.calls[num];
}

void syscall_stats_print(void) {
    int i;
    
    kprintf("\n System Call Statistics \n");
    kprintf("Total calls: %llu\n", syscall_stats.total_calls);
    kprintf("Total errors: %llu\n\n", syscall_stats.errors);
    
    kprintf("Per-syscall counts:\n");
    for (i = 0; i < NSYSCALLS; i++) {
        if (syscall_table[i].enabled && syscall_stats.calls[i] > 0) {
            kprintf("  [%3d] %-12s: %llu\n", 
                    i, syscall_table[i].name, syscall_stats.calls[i]);
        }
    }
}

void syscall_list(void) {
    int i;
    
    kprintf("\n Registered System Calls \n");
    for (i = 0; i < NSYSCALLS; i++) {
        if (syscall_table[i].enabled) {
            kprintf("  [%3d] %-12s (%d args)\n",
                    i, syscall_table[i].name, syscall_table[i].nargs);
        }
    }
}

#ifdef USER_SPACE_SYSCALLS

static inline int32_t _syscall0(int num) {
    /* Architecture-specific inline assembly */
    int32_t result;
    
#if defined(__i386__)
    __asm__ volatile (
        "int $0x80"
        : "=a" (result)
        : "a" (num)
    );
#elif defined(__x86_64__)
    __asm__ volatile (
        "syscall"
        : "=a" (result)
        : "a" (num)
    );
#endif
    
    return result;
}

static inline int32_t _syscall1(int num, uint32_t arg1) {
    int32_t result;
    
#if defined(__i386__)
    __asm__ volatile (
        "int $0x80"
        : "=a" (result)
        : "a" (num), "b" (arg1)
    );
#elif defined(__x86_64__)
    __asm__ volatile (
        "syscall"
        : "=a" (result)
        : "a" (num), "D" (arg1)
    );
#endif
    
    return result;
}


static inline int32_t _syscall2(int num, uint32_t arg1, uint32_t arg2) {
    int32_t result;
    
#if defined(__i386__)
    __asm__ volatile (
        "int $0x80"
        : "=a" (result)
        : "a" (num), "b" (arg1), "c" (arg2)
    );
#elif defined(__x86_64__)
    __asm__ volatile (
        "syscall"
        : "=a" (result)
        : "a" (num), "D" (arg1), "S" (arg2)
    );
#endif
    
    return result;
}

static inline int32_t _syscall3(int num, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    int32_t result;
    
#if defined(__i386__)
    __asm__ volatile (
        "int $0x80"
        : "=a" (result)
        : "a" (num), "b" (arg1), "c" (arg2), "d" (arg3)
    );
#elif defined(__x86_64__)
    __asm__ volatile (
        "syscall"
        : "=a" (result)
        : "a" (num), "D" (arg1), "S" (arg2), "d" (arg3)
    );
#endif
    
    return result;
}

#endif 
