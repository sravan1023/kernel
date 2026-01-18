/*
 * syscall.c - Xinu System Call Implementation
 * 
 * This file implements the system call interface and dispatching
 * mechanism for the Xinu operating system.
 */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"
#include "../include/memory.h"

#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

/*------------------------------------------------------------------------
 * System Call Numbers
 *------------------------------------------------------------------------*/

/* Process system calls */
#define SYS_CREATE      1       /* Create process */
#define SYS_KILL        2       /* Kill process */
#define SYS_GETPID      3       /* Get current PID */
#define SYS_SUSPEND     4       /* Suspend process */
#define SYS_RESUME      5       /* Resume process */
#define SYS_YIELD       6       /* Yield CPU */
#define SYS_SLEEP       7       /* Sleep for ticks */
#define SYS_SLEEPMS     8       /* Sleep for milliseconds */
#define SYS_EXIT        9       /* Exit current process */
#define SYS_WAIT        10      /* Wait for child */
#define SYS_GETPRIO     11      /* Get priority */
#define SYS_SETPRIO     12      /* Set priority */

/* Memory system calls */
#define SYS_GETMEM      20      /* Allocate memory */
#define SYS_FREEMEM     21      /* Free memory */
#define SYS_GETSTK      22      /* Allocate stack */
#define SYS_FREESTK     23      /* Free stack */

/* Semaphore system calls */
#define SYS_SEMCREATE   30      /* Create semaphore */
#define SYS_SEMDELETE   31      /* Delete semaphore */
#define SYS_WAIT_SEM    32      /* Wait on semaphore */
#define SYS_SIGNAL      33      /* Signal semaphore */
#define SYS_SIGNALN     34      /* Signal N times */
#define SYS_SEMCOUNT    35      /* Get semaphore count */

/* I/O system calls */
#define SYS_READ        40      /* Read from device */
#define SYS_WRITE       41      /* Write to device */
#define SYS_OPEN        42      /* Open device */
#define SYS_CLOSE       43      /* Close device */
#define SYS_SEEK        44      /* Seek on device */
#define SYS_IOCTL       45      /* Device control */
#define SYS_GETC        46      /* Get character */
#define SYS_PUTC        47      /* Put character */

/* Message passing system calls */
#define SYS_SEND        50      /* Send message */
#define SYS_RECEIVE     51      /* Receive message */
#define SYS_RECVCLR     52      /* Receive and clear */
#define SYS_RECVTIME    53      /* Receive with timeout */

/* Time system calls */
#define SYS_GETTIME     60      /* Get system time */
#define SYS_GETTICKS    61      /* Get tick count */
#define SYS_GETUPTIME   62      /* Get uptime */

/* System control */
#define SYS_SHUTDOWN    70      /* Shutdown system */
#define SYS_REBOOT      71      /* Reboot system */

/* Maximum system call number */
#define NSYSCALLS       128

/*------------------------------------------------------------------------
 * System Call Table
 *------------------------------------------------------------------------*/

/* System call handler function type */
typedef int32_t (*syscall_handler_t)(void *args);

/* System call table entry */
typedef struct {
    syscall_handler_t   handler;    /* Handler function */
    const char          *name;      /* System call name */
    uint8_t             nargs;      /* Number of arguments */
    bool                enabled;    /* Is syscall enabled */
} syscall_entry_t;

/* System call table */
static syscall_entry_t syscall_table[NSYSCALLS];

/* System call statistics */
static struct {
    uint64_t total_calls;           /* Total system calls */
    uint64_t calls[NSYSCALLS];      /* Per-syscall counts */
    uint64_t errors;                /* Error count */
} syscall_stats;

/*------------------------------------------------------------------------
 * System Call Argument Access
 *------------------------------------------------------------------------*/

/**
 * System call argument structure
 * 
 * Architecture-dependent: arguments may be passed in registers
 * or on the stack. This structure provides a uniform interface.
 */
typedef struct {
    uint32_t arg[8];    /* Up to 8 arguments */
} syscall_args_t;

/*------------------------------------------------------------------------
 * System Call Handlers (Process)
 *------------------------------------------------------------------------*/

/**
 * sys_create - System call handler for create
 */
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

/**
 * sys_kill - System call handler for kill
 */
static int32_t sys_kill(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    
    return kill(pid);
}

/**
 * sys_getpid - System call handler for getpid
 */
static int32_t sys_getpid(void *args) {
    (void)args;
    return getpid();
}

/**
 * sys_suspend - System call handler for suspend
 */
static int32_t sys_suspend(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    
    return suspend(pid);
}

/**
 * sys_resume - System call handler for resume
 */
static int32_t sys_resume(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    
    return resume(pid);
}

/**
 * sys_yield - System call handler for yield
 */
static int32_t sys_yield(void *args) {
    (void)args;
    yield();
    return OK;
}

/**
 * sys_sleep - System call handler for sleep
 */
static int32_t sys_sleep(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    uint32_t delay = a->arg[0];
    
    return sleep(delay);
}

/**
 * sys_sleepms - System call handler for sleepms
 */
static int32_t sys_sleepms(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    uint32_t msec = a->arg[0];
    
    return sleepms(msec);
}

/**
 * sys_exit - System call handler for exit
 */
static int32_t sys_exit(void *args) {
    (void)args;
    /* Kill the current process */
    kill(getpid());
    /* Should not return */
    return OK;
}

/**
 * sys_getprio - System call handler for getprio
 */
static int32_t sys_getprio(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    
    return getprio(pid);
}

/**
 * sys_setprio - System call handler for chprio
 */
static int32_t sys_setprio(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    pri16 newprio = (pri16)a->arg[1];
    
    return chprio(pid, newprio);
}

/*------------------------------------------------------------------------
 * System Call Handlers (Memory)
 *------------------------------------------------------------------------*/

/**
 * sys_getmem - System call handler for getmem
 */
static int32_t sys_getmem(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    uint32_t nbytes = a->arg[0];
    
    return (int32_t)getmem(nbytes);
}

/**
 * sys_freemem - System call handler for freemem
 */
static int32_t sys_freemem(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    void *block = (void *)a->arg[0];
    uint32_t nbytes = a->arg[1];
    
    return freemem(block, nbytes);
}

/*------------------------------------------------------------------------
 * System Call Handlers (Semaphore)
 *------------------------------------------------------------------------*/

/**
 * sys_semcreate - System call handler for semcreate
 */
static int32_t sys_semcreate(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    int32_t count = (int32_t)a->arg[0];
    
    return semcreate(count);
}

/**
 * sys_semdelete - System call handler for semdelete
 */
static int32_t sys_semdelete(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    
    return semdelete(sem);
}

/**
 * sys_wait_sem - System call handler for wait (semaphore)
 */
static int32_t sys_wait_sem(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    
    return wait(sem);
}

/**
 * sys_signal - System call handler for signal
 */
static int32_t sys_signal(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    
    return signal(sem);
}

/**
 * sys_signaln - System call handler for signaln
 */
static int32_t sys_signaln(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    int32_t count = (int32_t)a->arg[1];
    
    return signaln(sem, count);
}

/**
 * sys_semcount - System call handler for semcount
 */
static int32_t sys_semcount(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    sid32 sem = (sid32)a->arg[0];
    
    return semcount(sem);
}

/*------------------------------------------------------------------------
 * System Call Handlers (Message Passing)
 *------------------------------------------------------------------------*/

/**
 * sys_send - System call handler for send
 */
static int32_t sys_send(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    pid32 pid = (pid32)a->arg[0];
    umsg32 msg = (umsg32)a->arg[1];
    
    return send(pid, msg);
}

/**
 * sys_receive - System call handler for receive
 */
static int32_t sys_receive(void *args) {
    (void)args;
    return receive();
}

/**
 * sys_recvclr - System call handler for recvclr
 */
static int32_t sys_recvclr(void *args) {
    (void)args;
    return recvclr();
}

/**
 * sys_recvtime - System call handler for recvtime
 */
static int32_t sys_recvtime(void *args) {
    syscall_args_t *a = (syscall_args_t *)args;
    uint32_t timeout = a->arg[0];
    
    return recvtime(timeout);
}

/*------------------------------------------------------------------------
 * System Call Handlers (Time)
 *------------------------------------------------------------------------*/

/**
 * sys_gettime - System call handler for gettime
 */
static int32_t sys_gettime(void *args) {
    (void)args;
    return gettime();
}

/**
 * sys_getticks - System call handler for getticks
 */
static int32_t sys_getticks(void *args) {
    (void)args;
    return (int32_t)(getticks() & 0xFFFFFFFF);
}

/*------------------------------------------------------------------------
 * System Call Handlers (System Control)
 *------------------------------------------------------------------------*/

/**
 * sys_shutdown - System call handler for shutdown
 */
static int32_t sys_shutdown(void *args) {
    (void)args;
    shutdown();
    return OK;
}

/**
 * sys_reboot - System call handler for reboot
 */
static int32_t sys_reboot(void *args) {
    (void)args;
    reboot();
    return OK;
}

/*------------------------------------------------------------------------
 * System Call Initialization
 *------------------------------------------------------------------------*/

/**
 * syscall_init - Initialize the system call subsystem
 */
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

/**
 * syscall_register - Register a system call handler
 * 
 * @param num: System call number
 * @param handler: Handler function
 * @param name: System call name
 * @param nargs: Number of arguments
 * 
 * Returns: OK on success, SYSERR on error
 */
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

/**
 * syscall_unregister - Unregister a system call
 * 
 * @param num: System call number
 * 
 * Returns: OK on success, SYSERR on error
 */
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

/*------------------------------------------------------------------------
 * System Call Dispatch
 *------------------------------------------------------------------------*/

/**
 * syscall_dispatch - Dispatch a system call
 * 
 * @param num: System call number
 * @param args: Pointer to arguments
 * 
 * Returns: System call result
 * 
 * This is called from the system call interrupt handler after
 * extracting the syscall number and arguments.
 */
int32_t syscall_dispatch(int num, void *args) {
    int32_t result;
    
    /* Validate syscall number */
    if (num < 0 || num >= NSYSCALLS) {
        syscall_stats.errors++;
        return SYSERR;
    }
    
    /* Check if syscall is registered and enabled */
    if (!syscall_table[num].enabled || syscall_table[num].handler == NULL) {
        syscall_stats.errors++;
        return SYSERR;
    }
    
    /* Update statistics */
    syscall_stats.total_calls++;
    syscall_stats.calls[num]++;
    
    /* Call the handler */
    result = syscall_table[num].handler(args);
    
    return result;
}

/**
 * syscall_handler - Low-level system call entry point
 * 
 * This is called from the interrupt/trap handler.
 * Architecture-specific code extracts syscall number and arguments
 * and calls this function.
 */
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
    
    /* Placeholder - actual implementation is architecture-specific */
}

/*------------------------------------------------------------------------
 * System Call Information
 *------------------------------------------------------------------------*/

/**
 * syscall_name - Get name of a system call
 * 
 * @param num: System call number
 * 
 * Returns: System call name, or NULL if invalid
 */
const char *syscall_name(int num) {
    if (num < 0 || num >= NSYSCALLS) {
        return NULL;
    }
    
    return syscall_table[num].name;
}

/**
 * syscall_count - Get number of times a syscall was invoked
 * 
 * @param num: System call number
 * 
 * Returns: Call count, or -1 if invalid
 */
int64_t syscall_count(int num) {
    if (num < 0 || num >= NSYSCALLS) {
        return -1;
    }
    
    return syscall_stats.calls[num];
}

/**
 * syscall_stats_print - Print system call statistics
 */
void syscall_stats_print(void) {
    int i;
    
    kprintf("\n===== System Call Statistics =====\n");
    kprintf("Total calls: %llu\n", syscall_stats.total_calls);
    kprintf("Total errors: %llu\n\n", syscall_stats.errors);
    
    kprintf("Per-syscall counts:\n");
    for (i = 0; i < NSYSCALLS; i++) {
        if (syscall_table[i].enabled && syscall_stats.calls[i] > 0) {
            kprintf("  [%3d] %-12s: %llu\n", 
                    i, syscall_table[i].name, syscall_stats.calls[i]);
        }
    }
    kprintf("==================================\n\n");
}

/**
 * syscall_list - List all registered system calls
 */
void syscall_list(void) {
    int i;
    
    kprintf("\n===== Registered System Calls =====\n");
    for (i = 0; i < NSYSCALLS; i++) {
        if (syscall_table[i].enabled) {
            kprintf("  [%3d] %-12s (%d args)\n",
                    i, syscall_table[i].name, syscall_table[i].nargs);
        }
    }
    kprintf("===================================\n\n");
}

/*------------------------------------------------------------------------
 * User-Space System Call Wrappers
 *------------------------------------------------------------------------*/

/* These would typically be in a separate user-space library */

#ifdef USER_SPACE_SYSCALLS

/**
 * _syscall0 - System call with 0 arguments
 */
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

/**
 * _syscall1 - System call with 1 argument
 */
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

/**
 * _syscall2 - System call with 2 arguments
 */
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

/**
 * _syscall3 - System call with 3 arguments
 */
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

#endif /* USER_SPACE_SYSCALLS */
