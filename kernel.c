/*
 * kernel.c - Xinu Kernel Core Implementation
 * 
 * This file contains the core kernel functionality including
 * process table management, context switching, scheduling,
 * system initialization, and kernel services.
 */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/memory.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

/*------------------------------------------------------------------------
 * Global Kernel State
 *------------------------------------------------------------------------*/

/* Process table - array of all process control blocks */
proc_t proctab[NPROC];

/* Currently executing process ID */
pid32 currpid = 0;

/* System-wide semaphore table */
sem_t semtab[NSEM];

/* Number of currently active processes */
static int32_t numproc = 0;

/* Next available process ID for allocation */
static pid32 nextpid = 1;

/* System boot time (in ticks since boot) */
static uint32_t boot_time = 0;

/* System uptime counter (incremented by clock interrupt) */
static volatile uint32_t system_ticks = 0;

/* Kernel initialized flag */
static bool kernel_initialized = false;

/* Ready queue for processes (simple linked list using queue indices) */
static pid32 readylist_head = -1;
static pid32 readylist_tail = -1;

/* Sleep queue for sleeping processes */
static pid32 sleepq_head = -1;

/*------------------------------------------------------------------------
 * Kernel Initialization
 *------------------------------------------------------------------------*/

/**
 * kernel_init - Initialize the Xinu kernel
 * 
 * Performs complete kernel initialization:
 * - Clears and initializes process table
 * - Initializes semaphore table
 * - Sets up memory management
 * - Creates the null process (process 0)
 * - Initializes interrupt handling
 */
void kernel_init(void) {
    int i;
    intmask mask;
    
    /* Disable interrupts during initialization */
    mask = disable();
    
    /*
     * Initialize Process Table
     * All slots start as FREE with default values
     */
    for (i = 0; i < NPROC; i++) {
        proctab[i].pstate = PR_FREE;
        proctab[i].pprio = PRIORITY_DEFAULT;
        proctab[i].pstkbase = 0;
        proctab[i].pstklen = 0;
        memset(proctab[i].pname, 0, NAMELEN);
        memset(proctab[i].pregs, 0, sizeof(proctab[i].pregs));
        proctab[i].pwait = -1;
        proctab[i].pmsg = 0;
        proctab[i].phasmsg = false;
        proctab[i].pbase = 0;
        proctab[i].plen = 0;
        proctab[i].paddr = 0;
        proctab[i].pargs = 0;
    }
    
    /*
     * Initialize Semaphore Table
     * All semaphores start as unused
     */
    for (i = 0; i < NSEM; i++) {
        semtab[i].count = 0;
        semtab[i].queue = -1;  /* Empty queue */
    }
    
    /*
     * Initialize Memory Management
     * Sets up heap and page tables
     */
    init_memory();
    
    /*
     * Create Null Process (Process 0)
     * The null process runs when no other process is ready
     * It has the lowest priority and never terminates
     */
    currpid = 0;
    numproc = 1;
    
    proctab[0].pstate = PR_CURR;
    proctab[0].pprio = PRIORITY_MIN;
    strcpy(proctab[0].pname, "null");
    proctab[0].pstkbase = 0;  /* Uses kernel stack */
    proctab[0].pstklen = 0;
    proctab[0].pwait = -1;
    proctab[0].phasmsg = false;
    
    /* Initialize ready queue as empty */
    readylist_head = -1;
    readylist_tail = -1;
    
    /* Initialize sleep queue as empty */
    sleepq_head = -1;
    
    /* Reset system tick counter */
    system_ticks = 0;
    boot_time = 0;
    
    /* Mark kernel as initialized */
    kernel_initialized = true;
    
    /* Re-enable interrupts */
    restore(mask);
}

/*------------------------------------------------------------------------
 * Ready Queue Management
 *------------------------------------------------------------------------*/

/**
 * enqueue_ready - Add process to ready queue (priority ordered)
 * 
 * @param pid: Process ID to enqueue
 * 
 * Inserts process into ready queue in priority order (highest first)
 */
static void enqueue_ready(pid32 pid) {
    pid32 prev = -1;
    pid32 curr = readylist_head;
    uint32_t prio = proctab[pid].pprio;
    
    /* Find insertion point (priority ordered, highest first) */
    while (curr != -1 && proctab[curr].pprio >= prio) {
        prev = curr;
        curr = proctab[curr].pwait;  /* Using pwait as next pointer */
    }
    
    /* Insert into queue */
    proctab[pid].pwait = curr;
    
    if (prev == -1) {
        readylist_head = pid;
    } else {
        proctab[prev].pwait = pid;
    }
    
    if (curr == -1) {
        readylist_tail = pid;
    }
}

/**
 * dequeue_ready - Remove highest priority process from ready queue
 * 
 * Returns: Process ID of highest priority ready process, or -1 if empty
 */
static pid32 dequeue_ready(void) {
    pid32 pid = readylist_head;
    
    if (pid == -1) {
        return -1;
    }
    
    readylist_head = proctab[pid].pwait;
    proctab[pid].pwait = -1;
    
    if (readylist_head == -1) {
        readylist_tail = -1;
    }
    
    return pid;
}

/**
 * remove_from_ready - Remove specific process from ready queue
 * 
 * @param pid: Process ID to remove
 */
static void remove_from_ready(pid32 pid) {
    pid32 prev = -1;
    pid32 curr = readylist_head;
    
    while (curr != -1 && curr != pid) {
        prev = curr;
        curr = proctab[curr].pwait;
    }
    
    if (curr == pid) {
        if (prev == -1) {
            readylist_head = proctab[pid].pwait;
        } else {
            proctab[prev].pwait = proctab[pid].pwait;
        }
        
        if (readylist_tail == pid) {
            readylist_tail = prev;
        }
        
        proctab[pid].pwait = -1;
    }
}

/*------------------------------------------------------------------------
 * Context Switching
 *------------------------------------------------------------------------*/

/**
 * context_switch - Switch execution context between two processes
 * 
 * @param oldpid: Process ID to switch from (current process)
 * @param newpid: Process ID to switch to (next process)
 * 
 * This function saves the register state of the old process and
 * restores the register state of the new process. In a real
 * implementation, this would be written in assembly.
 */
void context_switch(pid32 oldpid, pid32 newpid) {
    proc_t *oldproc, *newproc;
    
    if (oldpid == newpid) {
        return;  /* No switch needed */
    }
    
    oldproc = &proctab[oldpid];
    newproc = &proctab[newpid];
    
    /*
     * Save old process state
     * In real implementation, would save:
     * - General purpose registers (R0-R15 or EAX, EBX, etc.)
     * - Program counter (PC/EIP)
     * - Stack pointer (SP/ESP)
     * - Status register (PSR/EFLAGS)
     */
    
    /* Assembly equivalent would be:
     *   push all registers
     *   save SP to oldproc->pregs[15]
     *   save PC to return address
     */
    
    /*
     * Restore new process state
     * Load all saved registers from newproc
     */
    
    /* Assembly equivalent would be:
     *   load SP from newproc->pregs[15]
     *   pop all registers
     *   return to saved PC
     */
    
    /* Update current process ID */
    currpid = newpid;
    newproc->pstate = PR_CURR;
}

/**
 * ctxsw - Low-level context switch (assembly wrapper)
 * 
 * @param old_sp: Pointer to save old stack pointer
 * @param new_sp: New stack pointer to load
 * 
 * This would be implemented in assembly in a real kernel.
 */
void ctxsw(uint32_t *old_sp, uint32_t new_sp) {
    /* 
     * Assembly implementation would be:
     * 
     * ARM:
     *   stmfd sp!, {r0-r12, lr}    ; Save registers
     *   str   sp, [r0]             ; Save old SP
     *   mov   sp, r1               ; Load new SP
     *   ldmfd sp!, {r0-r12, pc}    ; Restore registers and return
     *
     * x86:
     *   pushad                     ; Save all registers
     *   mov [eax], esp             ; Save old ESP
     *   mov esp, ebx               ; Load new ESP
     *   popad                      ; Restore all registers
     *   ret                        ; Return to new process
     */
    (void)old_sp;
    (void)new_sp;
}

/*------------------------------------------------------------------------
 * Process Scheduling
 *------------------------------------------------------------------------*/

/**
 * resched - Reschedule processes
 * 
 * Selects the highest priority ready process and switches to it.
 * If the current process is still the highest priority, no switch occurs.
 * 
 * This is the core scheduling function called whenever:
 * - A process blocks (wait, sleep, receive)
 * - A process is created or resumed
 * - A time quantum expires
 */
void resched(void) {
    intmask mask;
    pid32 oldpid, newpid;
    proc_t *oldproc, *newproc;
    
    mask = disable();
    
    oldpid = currpid;
    oldproc = &proctab[oldpid];
    
    /* If current process is still running, check if preemption needed */
    if (oldproc->pstate == PR_CURR) {
        /* Check if there's a higher priority ready process */
        if (readylist_head != -1 && 
            proctab[readylist_head].pprio > oldproc->pprio) {
            /* Preempt current process */
            oldproc->pstate = PR_READY;
            enqueue_ready(oldpid);
        } else {
            /* Current process continues */
            restore(mask);
            return;
        }
    }
    
    /* Select next process to run */
    newpid = dequeue_ready();
    
    if (newpid == -1) {
        /* No ready process, run null process */
        newpid = 0;
    }
    
    newproc = &proctab[newpid];
    newproc->pstate = PR_CURR;
    currpid = newpid;
    
    /* Perform context switch */
    if (oldpid != newpid) {
        context_switch(oldpid, newpid);
    }
    
    restore(mask);
}

/**
 * resched_cntl - Control rescheduling behavior
 * 
 * @param defer: If true, defer rescheduling until later
 * 
 * Returns: Previous defer state
 */
static bool resched_deferred = false;
static bool resched_pending = false;

bool resched_cntl(bool defer) {
    bool old = resched_deferred;
    
    resched_deferred = defer;
    
    if (!defer && resched_pending) {
        resched_pending = false;
        resched();
    }
    
    return old;
}

/*------------------------------------------------------------------------
 * System Time Management
 *------------------------------------------------------------------------*/

/**
 * clkhandler - Clock interrupt handler
 * 
 * Called on each clock tick (typically 1ms or 10ms intervals).
 * Handles:
 * - System tick counting
 * - Waking sleeping processes
 * - Time slice expiration
 */
void clkhandler(void) {
    pid32 pid;
    pid32 prev;
    pid32 next;
    bool need_resched = false;
    
    /* Increment system tick counter */
    system_ticks++;
    
    /* Process sleep queue - wake processes whose time has expired */
    prev = -1;
    pid = sleepq_head;
    
    while (pid != -1) {
        /* Decrement sleep time (stored in pargs temporarily) */
        if (proctab[pid].pargs > 0) {
            proctab[pid].pargs--;
        }
        
        if (proctab[pid].pargs == 0) {
            /* Wake this process */
            next = proctab[pid].pwait;
            
            /* Remove from sleep queue */
            if (prev == -1) {
                sleepq_head = next;
            } else {
                proctab[prev].pwait = next;
            }
            
            /* Add to ready queue */
            proctab[pid].pstate = PR_READY;
            proctab[pid].pwait = -1;
            enqueue_ready(pid);
            need_resched = true;
            
            pid = next;
        } else {
            prev = pid;
            pid = proctab[pid].pwait;
        }
    }
    
    /* Check for time slice expiration (round-robin within priority) */
    /* In a real implementation, would track per-process time quantum */
    
    /* Reschedule if needed */
    if (need_resched && !resched_deferred) {
        resched();
    } else if (need_resched) {
        resched_pending = true;
    }
}

/**
 * get_system_time - Get current system time in ticks
 * 
 * Returns: Number of ticks since system boot
 */
uint32_t get_system_time(void) {
    return system_ticks;
}

/**
 * get_uptime_seconds - Get system uptime in seconds
 * 
 * Returns: Number of seconds since system boot
 */
uint32_t get_uptime_seconds(void) {
    /* Assuming 1000 ticks per second (1ms tick) */
    return system_ticks / 1000;
}

/*------------------------------------------------------------------------
 * Kernel Services
 *------------------------------------------------------------------------*/

/**
 * getprio - Get priority of a process
 * 
 * @param pid: Process ID
 * 
 * Returns: Priority value, or SYSERR if invalid PID
 */
int32_t getprio(pid32 pid) {
    intmask mask;
    int32_t prio;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (proctab[pid].pstate == PR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    prio = proctab[pid].pprio;
    
    restore(mask);
    return prio;
}

/**
 * chprio - Change priority of a process
 * 
 * @param pid: Process ID
 * @param newprio: New priority value
 * 
 * Returns: Old priority, or SYSERR on error
 */
int32_t chprio(pid32 pid, int32_t newprio) {
    intmask mask;
    int32_t oldprio;
    proc_t *pptr;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    if (newprio < PRIORITY_MIN || newprio > PRIORITY_MAX) {
        return SYSERR;
    }
    
    mask = disable();
    pptr = &proctab[pid];
    
    if (pptr->pstate == PR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    oldprio = pptr->pprio;
    pptr->pprio = newprio;
    
    /* If process is ready, reorder in ready queue */
    if (pptr->pstate == PR_READY) {
        remove_from_ready(pid);
        enqueue_ready(pid);
    }
    
    /* Check if rescheduling is needed */
    if (pid == currpid || pptr->pstate == PR_READY) {
        resched();
    }
    
    restore(mask);
    return oldprio;
}

/**
 * getname - Get name of a process
 * 
 * @param pid: Process ID
 * @param buf: Buffer to store name
 * @param len: Buffer length
 * 
 * Returns: OK on success, SYSERR on error
 */
int32_t getname(pid32 pid, char *buf, int len) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC || buf == NULL || len <= 0) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (proctab[pid].pstate == PR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    strncpy(buf, proctab[pid].pname, len - 1);
    buf[len - 1] = '\0';
    
    restore(mask);
    return OK;
}

/**
 * nproc - Get number of active processes
 * 
 * Returns: Number of processes currently in use
 */
int32_t nprocs(void) {
    return numproc;
}

/*------------------------------------------------------------------------
 * Panic and Error Handling
 *------------------------------------------------------------------------*/

/**
 * panic - Kernel panic handler
 * 
 * @param msg: Error message describing the panic
 * 
 * Called when an unrecoverable error occurs. Disables interrupts,
 * prints error information, and halts the system.
 */
void panic(char *msg) {
    intmask mask;
    
    /* Disable interrupts - system is halting */
    mask = disable();
    (void)mask;  /* Won't be restored */
    
    /*
     * In a real implementation:
     * 1. Print panic message to console
     * 2. Print stack trace
     * 3. Print register dump
     * 4. Print process information
     */
    
    /* 
     * kprintf("\n\n*** KERNEL PANIC ***\n");
     * kprintf("Error: %s\n", msg);
     * kprintf("Current PID: %d (%s)\n", currpid, proctab[currpid].pname);
     * kprintf("System halted.\n");
     */
    
    /* Halt system - infinite loop */
    while (1) {
        /* 
         * Could add:
         * - CPU halt instruction (HLT on x86)
         * - Watchdog disable
         * - LED blink pattern for debugging
         */
        __asm__("nop");  /* Prevent optimization */
    }
}

/**
 * kprintf - Kernel printf function
 * 
 * @param fmt: Format string
 * @param ...: Variable arguments
 * 
 * Returns: Number of characters printed
 * 
 * Minimal printf for kernel use. In a real implementation,
 * would write to console/serial port.
 */
int kprintf(const char *fmt, ...) {
    va_list args;
    char buffer[256];
    int len;
    
    va_start(args, fmt);
    /* Would use vsnprintf in real implementation */
    len = 0;  /* Placeholder */
    (void)buffer;
    (void)fmt;
    va_end(args);
    
    /* 
     * In real implementation:
     * - Format string into buffer
     * - Write to console device
     * - Handle special characters (\n, \t, etc.)
     */
    
    return len;
}

/*------------------------------------------------------------------------
 * Kernel Utility Functions
 *------------------------------------------------------------------------*/

/**
 * kernel_is_initialized - Check if kernel is initialized
 * 
 * Returns: true if kernel initialization is complete
 */
bool kernel_is_initialized(void) {
    return kernel_initialized;
}

/**
 * get_proc_count - Get count of processes by state
 * 
 * @param state: Process state to count (or -1 for all)
 * 
 * Returns: Number of processes in specified state
 */
int32_t get_proc_count(int state) {
    int i;
    int32_t count = 0;
    
    for (i = 0; i < NPROC; i++) {
        if (state == -1) {
            if (proctab[i].pstate != PR_FREE) {
                count++;
            }
        } else if (proctab[i].pstate == (uint32_t)state) {
            count++;
        }
    }
    
    return count;
}

/**
 * dump_proc_table - Dump process table for debugging
 * 
 * Prints information about all active processes.
 */
void dump_proc_table(void) {
    int i;
    const char *state_names[] = {
        "FREE", "CURR", "READY", "RECV", "SLEEP", "SUSP", "WAIT"
    };
    
    /*
     * kprintf("PID  STATE    PRIO  NAME\n");
     * kprintf("---  -----    ----  ----\n");
     */
    
    for (i = 0; i < NPROC; i++) {
        if (proctab[i].pstate != PR_FREE) {
            const char *state = (proctab[i].pstate < 7) ? 
                                state_names[proctab[i].pstate] : "???";
            /*
             * kprintf("%3d  %-8s %3d   %s\n",
             *         i, state, proctab[i].pprio, proctab[i].pname);
             */
            (void)state;
        }
    }
}

/*------------------------------------------------------------------------
 * Idle Process
 *------------------------------------------------------------------------*/

/**
 * null_process - The null (idle) process
 * 
 * This process runs when no other process is ready.
 * It should never terminate and has the lowest priority.
 */
void null_process(void) {
    while (1) {
        /* 
         * In a real implementation:
         * - Execute HLT instruction to save power
         * - Wait for interrupt
         * - Could perform background maintenance
         */
        __asm__("nop");
    }
}

