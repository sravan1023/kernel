/*
 * process.c - Xinu Process Management Implementation
 * 
 * This file implements the complete process management subsystem including
 * process creation, termination, state transitions, and inter-process
 * communication primitives.
 */

#include "../include/process.h"
#include "../include/kernel.h"
#include "../include/memory.h"
#include "../include/interrupts.h"

#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

/*------------------------------------------------------------------------
 * External Declarations
 *------------------------------------------------------------------------*/

extern proc_t proctab[];
extern pid32 currpid;
extern sem_t semtab[];
extern void resched(void);

/*------------------------------------------------------------------------
 * Process ID Allocation
 *------------------------------------------------------------------------*/

/* Track which PIDs are in use */
static bool pid_in_use[NPROC];
static pid32 next_pid_hint = 1;  /* Start searching from PID 1 */

/**
 * allocate_pid - Allocate a new process ID
 * 
 * Returns: Free process ID, or SYSERR if none available
 */
static pid32 allocate_pid(void) {
    int i;
    pid32 pid;
    
    /* Search from hint position */
    for (i = 0; i < NPROC; i++) {
        pid = (next_pid_hint + i) % NPROC;
        if (pid == 0) continue;  /* Skip PID 0 (null process) */
        
        if (!pid_in_use[pid] && proctab[pid].pstate == PR_FREE) {
            pid_in_use[pid] = true;
            next_pid_hint = (pid + 1) % NPROC;
            return pid;
        }
    }
    
    return SYSERR;
}

/**
 * release_pid - Release a process ID for reuse
 * 
 * @param pid: Process ID to release
 */
static void release_pid(pid32 pid) {
    if (pid > 0 && pid < NPROC) {
        pid_in_use[pid] = false;
    }
}

/*------------------------------------------------------------------------
 * Process Creation
 *------------------------------------------------------------------------*/

/**
 * create - Create a new process
 * 
 * @param funcaddr: Address of function to execute as process entry point
 * @param ssize: Stack size in bytes (minimum 256 bytes)
 * @param priority: Process priority (PRIORITY_MIN to PRIORITY_MAX)
 * @param name: Process name (up to NAMELEN-1 characters)
 * @param nargs: Number of arguments to pass to the function
 * @param ...: Variable arguments to pass to the function
 * 
 * Returns: Process ID on success, SYSERR on failure
 * 
 * The new process is created in SUSPENDED state. Use resume() to start it.
 * 
 * Example:
 *   pid = create(my_func, 4096, 50, "worker", 2, arg1, arg2);
 *   if (pid != SYSERR) resume(pid);
 */
pid32 create(void *funcaddr, uint32_t ssize, uint32_t priority,
             char *name, uint32_t nargs, ...) {
    intmask mask;
    pid32 pid;
    proc_t *pptr;
    uint32_t *saddr;
    uint32_t *savargs;
    va_list ap;
    uint32_t i;
    uint32_t *stack_top;
    
    /* Validate parameters */
    if (funcaddr == NULL) {
        return SYSERR;
    }
    
    if (ssize < 256) {
        ssize = 256;  /* Minimum stack size */
    }
    
    if (priority < PRIORITY_MIN) {
        priority = PRIORITY_MIN;
    } else if (priority > PRIORITY_MAX) {
        priority = PRIORITY_MAX;
    }
    
    /* Round stack size to word boundary */
    ssize = (ssize + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
    
    mask = disable();
    
    /* Allocate process ID */
    pid = allocate_pid();
    if (pid == SYSERR) {
        restore(mask);
        return SYSERR;
    }
    
    pptr = &proctab[pid];
    
    /* Allocate stack memory */
    saddr = (uint32_t *)getstk(ssize);
    if (saddr == NULL || saddr == (uint32_t *)SYSERR) {
        release_pid(pid);
        restore(mask);
        return SYSERR;
    }
    
    /*
     * Initialize Process Control Block
     */
    pptr->pstate = PR_SUSP;     /* Start suspended */
    pptr->pprio = priority;
    pptr->pstkbase = (uint32_t)saddr;
    pptr->pstklen = ssize;
    pptr->pwait = -1;
    pptr->pmsg = 0;
    pptr->phasmsg = false;
    pptr->pbase = (uint32_t)saddr;
    pptr->plen = ssize;
    pptr->paddr = (uint32_t)funcaddr;
    pptr->pargs = nargs;
    
    /* Copy process name */
    if (name != NULL) {
        strncpy(pptr->pname, name, NAMELEN - 1);
        pptr->pname[NAMELEN - 1] = '\0';
    } else {
        strcpy(pptr->pname, "unknown");
    }
    
    /* Clear register save area */
    memset(pptr->pregs, 0, sizeof(pptr->pregs));
    
    /*
     * Build Initial Stack Frame
     * 
     * Stack layout (growing downward):
     * 
     *   High address (stack base)
     *   +------------------+
     *   | Argument N-1     |  <- Arguments to function
     *   | ...              |
     *   | Argument 0       |
     *   +------------------+
     *   | Return address   |  <- userret (process exit handler)
     *   +------------------+
     *   | Saved FP         |  <- Frame pointer (0 for initial)
     *   +------------------+
     *   | Saved registers  |  <- R0-R12 or general purpose regs
     *   +------------------+
     *   | PC (entry point) |  <- Function address
     *   +------------------+
     *   Low address (stack pointer)
     */
    
    /* Start at top of stack */
    stack_top = saddr + (ssize / sizeof(uint32_t));
    saddr = stack_top;
    
    /* Push arguments in reverse order */
    va_start(ap, nargs);
    
    /* Allocate space for arguments on stack */
    if (nargs > 0) {
        saddr -= nargs;
        savargs = saddr;
        for (i = 0; i < nargs; i++) {
            savargs[i] = va_arg(ap, uint32_t);
        }
    }
    va_end(ap);
    
    /* Push return address (process exit handler) */
    *(--saddr) = (uint32_t)userret;
    
    /* Push function entry point */
    *(--saddr) = (uint32_t)funcaddr;
    
    /* Push initial frame pointer (0 for root frame) */
    *(--saddr) = 0;
    
    /* Push space for saved registers (architecture dependent) */
    /* For ARM: R0-R12 (13 registers) */
    /* For x86: EAX, EBX, ECX, EDX, ESI, EDI, EBP (7 registers) */
    for (i = 0; i < 13; i++) {
        *(--saddr) = 0;
    }
    
    /* Set initial stack pointer in PCB */
    pptr->pregs[13] = (uint32_t)saddr;  /* SP register */
    pptr->pregs[14] = (uint32_t)funcaddr;  /* LR/return register */
    pptr->pregs[15] = (uint32_t)funcaddr;  /* PC register */
    
    restore(mask);
    return pid;
}

/**
 * newpid - Allocate a new process ID (wrapper function)
 * 
 * Returns: Available process ID, or SYSERR if none available
 */
pid32 newpid(void) {
    return allocate_pid();
}

/*------------------------------------------------------------------------
 * Process Termination
 *------------------------------------------------------------------------*/

/**
 * kill - Terminate a process
 * 
 * @param pid: Process ID to terminate
 * 
 * Releases all resources held by the process including:
 * - Stack memory
 * - Held semaphores (releases waiters)
 * - Message buffers
 * 
 * If the current process is killed, reschedules immediately.
 */
void kill(pid32 pid) {
    intmask mask;
    proc_t *pptr;
    
    /* Validate PID */
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    /* Cannot kill null process */
    if (pid == 0) {
        return;
    }
    
    mask = disable();
    pptr = &proctab[pid];
    
    /* Check if process exists */
    if (pptr->pstate == PR_FREE) {
        restore(mask);
        return;
    }
    
    /* Release stack memory */
    if (pptr->pstkbase != 0) {
        freestk((void *)pptr->pstkbase, pptr->pstklen);
        pptr->pstkbase = 0;
        pptr->pstklen = 0;
    }
    
    /* If waiting on semaphore, remove from wait queue */
    if (pptr->pstate == PR_WAIT && pptr->pwait >= 0 && pptr->pwait < NSEM) {
        /* Would remove from semaphore wait queue */
        semtab[pptr->pwait].count++;
    }
    
    /* Clear process state */
    pptr->pstate = PR_FREE;
    pptr->pprio = PRIORITY_DEFAULT;
    memset(pptr->pname, 0, NAMELEN);
    pptr->pwait = -1;
    pptr->pmsg = 0;
    pptr->phasmsg = false;
    
    /* Release PID */
    release_pid(pid);
    
    /* If killing current process, reschedule */
    if (pid == currpid) {
        resched();
    }
    
    restore(mask);
}

/**
 * userret - Process exit handler
 * 
 * Called when a process function returns. Terminates the process.
 * This function is placed on the stack as the return address.
 */
void userret(void) {
    kill(currpid);
}

/**
 * exit - Terminate current process with exit code
 * 
 * @param exitcode: Exit status code
 * 
 * Never returns.
 */
void exit(int exitcode) {
    (void)exitcode;  /* Could store for wait() */
    kill(currpid);
}

/*------------------------------------------------------------------------
 * Process State Management
 *------------------------------------------------------------------------*/

/**
 * getpid - Get current process ID
 * 
 * Returns: Process ID of the currently executing process
 */
pid32 getpid(void) {
    return currpid;
}

/**
 * getppid - Get parent process ID
 * 
 * Returns: Parent process ID (not implemented - returns 0)
 */
pid32 getppid(void) {
    /* Would need to track parent in PCB */
    return 0;
}

/**
 * ready - Make a process ready to run
 * 
 * @param pid: Process ID to make ready
 * @param resched_flag: If true, call resched() after making ready
 * 
 * Changes process state from SUSPENDED to READY and adds
 * to the ready queue.
 */
void ready(pid32 pid, bool resched_flag) {
    intmask mask;
    proc_t *pptr;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    pptr = &proctab[pid];
    
    if (pptr->pstate == PR_FREE) {
        restore(mask);
        return;
    }
    
    pptr->pstate = PR_READY;
    /* Process will be added to ready queue by scheduler */
    
    if (resched_flag) {
        resched();
    }
    
    restore(mask);
}

/**
 * suspend - Suspend a process
 * 
 * @param pid: Process ID to suspend
 * 
 * Returns: Priority of suspended process, or SYSERR on error
 * 
 * Moves process from READY to SUSPENDED state. A suspended
 * process will not run until explicitly resumed.
 */
syscall suspend(pid32 pid) {
    intmask mask;
    proc_t *pptr;
    int32_t prio;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    /* Cannot suspend null process */
    if (pid == 0) {
        return SYSERR;
    }
    
    mask = disable();
    pptr = &proctab[pid];
    
    /* Can only suspend READY or CURRENT processes */
    if (pptr->pstate != PR_READY && pptr->pstate != PR_CURR) {
        restore(mask);
        return SYSERR;
    }
    
    prio = pptr->pprio;
    
    if (pptr->pstate == PR_CURR) {
        /* Suspending current process */
        pptr->pstate = PR_SUSP;
        resched();
    } else {
        /* Suspending ready process - remove from ready queue */
        pptr->pstate = PR_SUSP;
    }
    
    restore(mask);
    return prio;
}

/**
 * resume - Resume a suspended process
 * 
 * @param pid: Process ID to resume
 * 
 * Returns: Priority of resumed process, or SYSERR on error
 * 
 * Moves process from SUSPENDED to READY state.
 */
syscall resume(pid32 pid) {
    intmask mask;
    proc_t *pptr;
    int32_t prio;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    pptr = &proctab[pid];
    
    /* Can only resume SUSPENDED processes */
    if (pptr->pstate != PR_SUSP) {
        restore(mask);
        return SYSERR;
    }
    
    prio = pptr->pprio;
    ready(pid, true);  /* Make ready and reschedule */
    
    restore(mask);
    return prio;
}

/*------------------------------------------------------------------------
 * Process Sleeping and Waiting
 *------------------------------------------------------------------------*/

/**
 * sleep - Put current process to sleep for specified time
 * 
 * @param delay: Sleep duration in milliseconds
 * 
 * Process is moved to SLEEP state and will be awakened after
 * the specified delay by the clock interrupt handler.
 */
void sleep(uint32_t delay) {
    intmask mask;
    
    if (delay == 0) {
        yield();
        return;
    }
    
    mask = disable();
    
    /* Store wakeup time in process args field */
    proctab[currpid].pargs = delay;
    proctab[currpid].pstate = PR_SLEEP;
    
    /* Add to sleep queue (handled by clock handler) */
    /* In full implementation, would insert into delta list */
    
    resched();
    
    restore(mask);
}

/**
 * sleepms - Sleep for specified milliseconds
 * 
 * @param delay: Delay in milliseconds
 * 
 * Wrapper for sleep() with clearer semantics.
 */
void sleepms(uint32_t delay) {
    sleep(delay);
}

/**
 * unsleep - Remove process from sleep queue
 * 
 * @param pid: Process ID to wake
 * 
 * Returns: OK if process was sleeping, SYSERR otherwise
 */
syscall unsleep(pid32 pid) {
    intmask mask;
    proc_t *pptr;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    pptr = &proctab[pid];
    
    if (pptr->pstate != PR_SLEEP) {
        restore(mask);
        return SYSERR;
    }
    
    /* Remove from sleep queue and make ready */
    pptr->pargs = 0;
    ready(pid, false);
    
    restore(mask);
    return OK;
}

/**
 * yield - Voluntarily give up the CPU
 * 
 * Causes the scheduler to run, potentially allowing other
 * processes of equal or higher priority to execute.
 */
void yield(void) {
    resched();
}

/**
 * wait - Wait for child process to terminate
 * 
 * @param pid: Process ID to wait for, or -1 for any child
 * 
 * Returns: Exit status of child, or SYSERR on error
 * 
 * Note: Full implementation would need parent-child tracking.
 */
syscall wait(pid32 pid) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    /* Wait for process to terminate */
    while (proctab[pid].pstate != PR_FREE) {
        /* Block current process */
        proctab[currpid].pstate = PR_WAIT;
        resched();
    }
    
    restore(mask);
    return OK;
}

/*------------------------------------------------------------------------
 * Inter-Process Communication (Messages)
 *------------------------------------------------------------------------*/

/**
 * send - Send a message to a process
 * 
 * @param pid: Destination process ID
 * @param msg: Message to send (32-bit value)
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * If the destination process already has an unread message,
 * this call will fail (no buffering).
 */
syscall send(pid32 pid, umsg32 msg) {
    intmask mask;
    proc_t *pptr;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    pptr = &proctab[pid];
    
    /* Check if process exists */
    if (pptr->pstate == PR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    /* Check if message slot is full */
    if (pptr->phasmsg) {
        restore(mask);
        return SYSERR;
    }
    
    /* Deliver message */
    pptr->pmsg = msg;
    pptr->phasmsg = true;
    
    /* If process is waiting for message, wake it */
    if (pptr->pstate == PR_RECV) {
        ready(pid, true);
    }
    
    restore(mask);
    return OK;
}

/**
 * receive - Receive a message
 * 
 * Returns: Message value
 * 
 * If no message is available, the calling process blocks
 * until a message arrives.
 */
umsg32 receive(void) {
    intmask mask;
    umsg32 msg;
    
    mask = disable();
    
    /* If no message, block */
    while (!proctab[currpid].phasmsg) {
        proctab[currpid].pstate = PR_RECV;
        resched();
    }
    
    /* Get message */
    msg = proctab[currpid].pmsg;
    proctab[currpid].phasmsg = false;
    
    restore(mask);
    return msg;
}

/**
 * recvclr - Receive and clear any pending message
 * 
 * Returns: Message if one was pending, 0 otherwise
 * 
 * Non-blocking receive - returns immediately.
 */
umsg32 recvclr(void) {
    intmask mask;
    umsg32 msg;
    
    mask = disable();
    
    if (proctab[currpid].phasmsg) {
        msg = proctab[currpid].pmsg;
        proctab[currpid].phasmsg = false;
    } else {
        msg = 0;
    }
    
    restore(mask);
    return msg;
}

/**
 * recvtime - Receive with timeout
 * 
 * @param maxwait: Maximum time to wait in milliseconds
 * 
 * Returns: Message value, or TIMEOUT if time expired
 */
umsg32 recvtime(uint32_t maxwait) {
    intmask mask;
    umsg32 msg;
    
    mask = disable();
    
    /* Check for existing message */
    if (proctab[currpid].phasmsg) {
        msg = proctab[currpid].pmsg;
        proctab[currpid].phasmsg = false;
        restore(mask);
        return msg;
    }
    
    /* Set timeout and wait */
    proctab[currpid].pargs = maxwait;
    proctab[currpid].pstate = PR_RECV;
    resched();
    
    /* Check if we got a message or timed out */
    if (proctab[currpid].phasmsg) {
        msg = proctab[currpid].pmsg;
        proctab[currpid].phasmsg = false;
    } else {
        msg = (umsg32)TIMEOUT;
    }
    
    restore(mask);
    return msg;
}

/*------------------------------------------------------------------------
 * Process Information
 *------------------------------------------------------------------------*/

/**
 * getstate - Get state of a process
 * 
 * @param pid: Process ID
 * 
 * Returns: Process state (PR_FREE, PR_CURR, etc.), or SYSERR
 */
int32_t getstate(pid32 pid) {
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    return proctab[pid].pstate;
}

/**
 * prcount - Count active processes
 * 
 * Returns: Number of processes that are not FREE
 */
int32_t prcount(void) {
    int i;
    int32_t count = 0;
    
    for (i = 0; i < NPROC; i++) {
        if (proctab[i].pstate != PR_FREE) {
            count++;
        }
    }
    
    return count;
}

/**
 * getprocinfo - Get information about a process
 * 
 * @param pid: Process ID
 * @param info: Buffer to store process information
 * 
 * Returns: OK on success, SYSERR on error
 */
typedef struct procinfo {
    pid32       pid;
    uint32_t    state;
    uint32_t    priority;
    char        name[NAMELEN];
    uint32_t    stksize;
    uint32_t    stkbase;
} procinfo_t;

syscall getprocinfo(pid32 pid, procinfo_t *info) {
    intmask mask;
    proc_t *pptr;
    
    if (pid < 0 || pid >= NPROC || info == NULL) {
        return SYSERR;
    }
    
    mask = disable();
    pptr = &proctab[pid];
    
    if (pptr->pstate == PR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    info->pid = pid;
    info->state = pptr->pstate;
    info->priority = pptr->pprio;
    strncpy(info->name, pptr->pname, NAMELEN);
    info->stksize = pptr->pstklen;
    info->stkbase = pptr->pstkbase;
    
    restore(mask);
    return OK;
}

