/* kernel.c - Kernel core implementation */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/memory.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

proc_t proctab[NPROC];
pid32 currpid = 0;
sem_t semtab[NSEM];

static int32_t numproc = 0;
static pid32 nextpid = 1;
static uint32_t boot_time = 0;
static volatile uint32_t system_ticks = 0;
static bool kernel_initialized = false;
static pid32 readylist_head = -1;
static pid32 readylist_tail = -1;
static pid32 sleepq_head = -1;

void kernel_init(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    /* Initialize process table */
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
    
    /* Initialize semaphore table */
    for (i = 0; i < NSEM; i++) {
        semtab[i].count = 0;
        semtab[i].queue = -1;
    }
    
    init_memory();
    
    /* Create null process */
    currpid = 0;
    numproc = 1;
    
    proctab[0].pstate = PR_CURR;
    proctab[0].pprio = PRIORITY_MIN;
    strcpy(proctab[0].pname, "null");
    proctab[0].pstkbase = 0;
    proctab[0].pstklen = 0;
    proctab[0].pwait = -1;
    proctab[0].phasmsg = false;
    
    readylist_head = -1;
    readylist_tail = -1;
    sleepq_head = -1;
    
    system_ticks = 0;
    boot_time = 0;
    kernel_initialized = true;
    
    restore(mask);
}

/* Add process to ready queue (priority ordered) */
static void enqueue_ready(pid32 pid) {
    pid32 prev = -1;
    pid32 curr = readylist_head;
    uint32_t prio = proctab[pid].pprio;
    
    while (curr != -1 && proctab[curr].pprio >= prio) {
        prev = curr;
        curr = proctab[curr].pwait;
    }
    
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

/* Remove and return highest priority process from ready queue */
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

/* Remove specific process from ready queue */
static void remove_from_ready(pid32 pid) {
    pid32 prev = -1;
    pid32 curr = readylist_head;

    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    while (curr != -1 && curr != pid) {
        if (curr < 0 || curr >= NPROC) {
            return;
        }
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

/* Switch execution context between processes */
void context_switch(pid32 oldpid, pid32 newpid) {
    proc_t *oldproc, *newproc;
    
    if (oldpid == newpid) {
        return;
    }
    
    oldproc = &proctab[oldpid];
    newproc = &proctab[newpid];
    
    currpid = newpid;
    newproc->pstate = PR_CURR;
}

/* Low-level context switch */
/* Low-level context switch */
void ctxsw(uint32_t *old_sp, uint32_t new_sp) {
    (void)old_sp;
    (void)new_sp;
}

/* Reschedule processes */
void resched(void) {
    intmask mask;
    pid32 oldpid, newpid;
    proc_t *oldproc, *newproc;
    
    mask = disable();
    
    oldpid = currpid;
    oldproc = &proctab[oldpid];
    
    if (oldproc->pstate == PR_CURR) {
        if (readylist_head != -1 && 
            proctab[readylist_head].pprio > oldproc->pprio) {
            oldproc->pstate = PR_READY;
            enqueue_ready(oldpid);
        } else {
            restore(mask);
            return;
        }
    }
    
    newpid = dequeue_ready();
    
    if (newpid == -1) {
        newpid = 0;
    }
    
    newproc = &proctab[newpid];
    newproc->pstate = PR_CURR;
    currpid = newpid;
    
    if (oldpid != newpid) {
        context_switch(oldpid, newpid);
    }
    
    restore(mask);
}

/* Control rescheduling behavior */
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

/* Clock interrupt handler */
void clkhandler(void) {
    pid32 pid;
    pid32 prev;
    pid32 next;
    bool need_resched = false;
    
    system_ticks++;
    
    prev = -1;
    pid = sleepq_head;
    
    while (pid != -1) {
        if (proctab[pid].pargs > 0) {
            proctab[pid].pargs--;
        }
        
        if (proctab[pid].pargs == 0) {
            next = proctab[pid].pwait;
            
            if (prev == -1) {
                sleepq_head = next;
            } else {
                proctab[prev].pwait = next;
            }
            
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
    
    if (need_resched && !resched_deferred) {
        resched();
    } else if (need_resched) {
        resched_pending = true;
    }
}

/* Get current system time in ticks */
uint32_t get_system_time(void) {
    return system_ticks;
}

/* Get system uptime in seconds */
uint32_t get_uptime_seconds(void) {
    return system_ticks / 1000;
}

/* Get priority of a process */
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

/* Change priority of a process */
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
    
    if (pptr->pstate == PR_READY) {
        remove_from_ready(pid);
        enqueue_ready(pid);
    }
    
    if (pid == currpid || pptr->pstate == PR_READY) {
        resched();
    }
    
    restore(mask);
    return oldprio;
}

/* Get name of a process */
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

/* Get number of active processes */
int32_t nprocs(void) {
    return numproc;
}

/* Kernel panic handler */
void panic(char *msg) {
    intmask mask;
    
    mask = disable();
    (void)mask;
    (void)msg;
    
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
    len = 0;
    (void)buffer;
    (void)fmt;
    va_end(args);
    
    return len;
}

/* Check if kernel is initialized */
bool kernel_is_initialized(void) {
    return kernel_initialized;
}

/* Get count of processes by state */
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

/* Dump process table for debugging */
void dump_proc_table(void) {
    int i;
    const char *state_names[] = {
        "FREE", "CURR", "READY", "RECV", "SLEEP", "SUSP", "WAIT"
    };
    
    for (i = 0; i < NPROC; i++) {
        if (proctab[i].pstate != PR_FREE) {
            const char *state = (proctab[i].pstate < 7) ? 
                                state_names[proctab[i].pstate] : "???";
            (void)state;
        }
    }
}

/* The null (idle) process */
void null_process(void) {
    while (1) {
        __asm__("nop");
    }
}

