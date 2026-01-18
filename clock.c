/*
 * clock.c - Xinu Clock and Timer Management
 * 
 * This file implements clock handling, timer management, and
 * time-related services for the Xinu operating system.
 */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdbool.h>

/*------------------------------------------------------------------------
 * Clock Constants and Configuration
 *------------------------------------------------------------------------*/

#define CLKFREQ         1000        /* Clock frequency in Hz (1ms tick) */
#define CLKTICKS_PER_SEC CLKFREQ    /* Ticks per second */
#define MS_PER_TICK     (1000/CLKFREQ)  /* Milliseconds per tick */

/* Timer array size */
#define NTIMERS         32          /* Maximum number of timers */

/* Timer states */
#define TMR_FREE        0           /* Timer is free */
#define TMR_ACTIVE      1           /* Timer is running */
#define TMR_EXPIRED     2           /* Timer has expired */
#define TMR_STOPPED     3           /* Timer was stopped */

/* Sleep queue */
#define MAXSLEEPTIME    0x7FFFFFFF  /* Maximum sleep time */

/*------------------------------------------------------------------------
 * Clock Data Structures
 *------------------------------------------------------------------------*/

/**
 * Timer callback function type
 */
typedef void (*timer_callback_t)(void *arg);

/**
 * Timer structure
 */
typedef struct timer {
    uint8_t     state;          /* Timer state */
    uint32_t    expires;        /* Expiration time (absolute ticks) */
    uint32_t    period;         /* Period for periodic timers (0=one-shot) */
    timer_callback_t callback;  /* Callback function */
    void        *arg;           /* Callback argument */
} timer_t;

/* Timer table */
static timer_t timertab[NTIMERS];

/* Clock variables - these may be declared in kernel.c, using extern */
volatile uint32_t clktime = 0;      /* Current time in seconds */
volatile uint32_t ctr1000 = 0;      /* Millisecond counter (0-999) */
volatile uint64_t clkticks = 0;     /* Total clock ticks since boot */

/* Deferred clock processing count */
volatile int32_t  clkdefer = 0;     /* Deferred ticks */

/* Time slicing */
static uint32_t preempt_count = 0;  /* Preemption counter */
#define QUANTUM         10          /* Default time quantum in ticks */
static uint32_t time_quantum = QUANTUM;

/* Sleep queue (delta list) */
static qid32 sleepq = -1;           /* Sleep queue ID */
static int32_t slnempty = 0;        /* Non-empty flag */

/* System uptime */
static struct {
    uint32_t days;
    uint8_t  hours;
    uint8_t  minutes;
    uint8_t  seconds;
    uint32_t ticks;
} uptime;

/*------------------------------------------------------------------------
 * Clock Initialization
 *------------------------------------------------------------------------*/

/**
 * clkinit - Initialize the clock subsystem
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Initializes clock variables, timer table, and programs the
 * hardware clock to generate periodic interrupts.
 */
syscall clkinit(void) {
    int i;
    
    /* Initialize clock counters */
    clktime = 0;
    ctr1000 = 0;
    clkticks = 0;
    clkdefer = 0;
    preempt_count = time_quantum;
    
    /* Initialize uptime */
    uptime.days = 0;
    uptime.hours = 0;
    uptime.minutes = 0;
    uptime.seconds = 0;
    uptime.ticks = 0;
    
    /* Initialize timer table */
    for (i = 0; i < NTIMERS; i++) {
        timertab[i].state = TMR_FREE;
        timertab[i].expires = 0;
        timertab[i].period = 0;
        timertab[i].callback = NULL;
        timertab[i].arg = NULL;
    }
    
    /* Create sleep queue */
    sleepq = newqueue();
    if (sleepq == SYSERR) {
        return SYSERR;
    }
    slnempty = 0;
    
    /* Program hardware timer
     * Platform-specific: x86 uses PIT, ARM uses SysTick, etc.
     */
    
#if defined(__i386__) || defined(__x86_64__)
    /* x86: Program the 8254 PIT (Programmable Interval Timer)
     * 
     * - Use channel 0
     * - Mode 3 (square wave)
     * - Binary counting
     * - Divisor for ~1ms intervals: 1193182 / 1000 = 1193
     */
    #define PIT_CHANNEL0    0x40
    #define PIT_COMMAND     0x43
    #define PIT_FREQ        1193182
    #define PIT_DIVISOR     (PIT_FREQ / CLKFREQ)
    
    /* outb(PIT_COMMAND, 0x36);  // Channel 0, lobyte/hibyte, mode 3, binary
     * outb(PIT_CHANNEL0, PIT_DIVISOR & 0xFF);
     * outb(PIT_CHANNEL0, (PIT_DIVISOR >> 8) & 0xFF);
     */
    
    /* Register clock interrupt handler (IRQ 0) */
    /* set_irq_handler(0, clkhandler); */
    /* enable_irq(0); */
    
#elif defined(__arm__) || defined(__aarch64__)
    /* ARM: Configure SysTick timer
     * 
     * SysTick is a 24-bit down counter that generates
     * an interrupt when it reaches 0.
     */
    
    /* SYSTICK_LOAD = (SystemCoreClock / CLKFREQ) - 1;
     * SYSTICK_VAL = 0;
     * SYSTICK_CTRL = SYSTICK_ENABLE | SYSTICK_TICKINT | SYSTICK_CLKSOURCE;
     */
    
#elif defined(__riscv)
    /* RISC-V: Configure machine timer
     * 
     * Use the CLINT (Core Local Interruptor) mtimecmp register
     * to generate timer interrupts.
     */
    
    /* uint64_t mtime = *(volatile uint64_t*)MTIME_ADDR;
     * *(volatile uint64_t*)MTIMECMP_ADDR = mtime + (MTIME_FREQ / CLKFREQ);
     */
    
#endif
    
    return OK;
}

/*------------------------------------------------------------------------
 * Clock Interrupt Handler
 *------------------------------------------------------------------------*/

/**
 * clkhandler - Handle clock interrupt
 * 
 * Called on each clock tick (typically 1ms).
 * Updates timers, manages preemption, and wakes sleeping processes.
 */
void clkhandler(void) {
    pid32 pid;
    
    /* Increment tick count */
    clkticks++;
    
    /* Update millisecond counter */
    ctr1000++;
    if (ctr1000 >= 1000) {
        ctr1000 = 0;
        clktime++;
        
        /* Update uptime */
        uptime.seconds++;
        if (uptime.seconds >= 60) {
            uptime.seconds = 0;
            uptime.minutes++;
            if (uptime.minutes >= 60) {
                uptime.minutes = 0;
                uptime.hours++;
                if (uptime.hours >= 24) {
                    uptime.hours = 0;
                    uptime.days++;
                }
            }
        }
    }
    uptime.ticks++;
    
    /* Check for deferred clock handling */
    if (clkdefer > 0) {
        clkdefer++;
        return;
    }
    
    /* Process timers */
    process_timers();
    
    /* Wake sleeping processes */
    wakeup();
    
    /* Handle preemption */
    if (--preempt_count <= 0) {
        preempt_count = time_quantum;
        resched();
    }
}

/**
 * defer_clock - Defer clock processing
 * 
 * Temporarily defers clock handling to prevent reentrancy issues.
 * Must be paired with undefer_clock().
 */
void defer_clock(void) {
    intmask mask = disable();
    clkdefer = 1;
    restore(mask);
}

/**
 * undefer_clock - Resume clock processing
 * 
 * Resumes clock handling after defer_clock().
 * Processes any deferred ticks.
 */
void undefer_clock(void) {
    intmask mask = disable();
    
    if (clkdefer > 1) {
        /* Process deferred ticks */
        int32_t deferred = clkdefer - 1;
        clkdefer = 0;
        
        while (deferred-- > 0) {
            process_timers();
            wakeup();
        }
        
        resched();
    } else {
        clkdefer = 0;
    }
    
    restore(mask);
}

/*------------------------------------------------------------------------
 * Sleep Queue Management
 *------------------------------------------------------------------------*/

/**
 * wakeup - Wake processes whose sleep time has expired
 * 
 * Checks the sleep queue (delta list) and wakes any processes
 * whose delay has elapsed.
 */
void wakeup(void) {
    pid32 pid;
    
    while (slnempty && proctab[firstid(sleepq)].pargs <= 0) {
        pid = dequeue(sleepq);
        if (pid != EMPTY && pid >= 0 && pid < NPROC) {
            if (proctab[pid].prstate == PR_SLEEP) {
                ready(pid);
            }
        }
        slnempty = nonempty(sleepq);
    }
    
    /* Decrement first entry's delta */
    if (slnempty) {
        pid = firstid(sleepq);
        if (pid != EMPTY && pid >= 0 && pid < NPROC) {
            proctab[pid].pargs--;
        }
    }
}

/**
 * sleep - Put current process to sleep for specified ticks
 * 
 * @param delay: Number of clock ticks to sleep
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall sleep(uint32_t delay) {
    intmask mask;
    
    if (delay == 0) {
        return OK;
    }
    
    mask = disable();
    
    if (currpid < 0 || currpid >= NPROC) {
        restore(mask);
        return SYSERR;
    }
    
    /* Insert into sleep queue (delta list) */
    proctab[currpid].prstate = PR_SLEEP;
    proctab[currpid].pargs = delay;
    
    /* insertd handles delta list management */
    insertd(currpid, sleepq, delay);
    slnempty = 1;
    
    resched();
    
    restore(mask);
    return OK;
}

/**
 * sleepms - Sleep for specified milliseconds
 * 
 * @param msec: Milliseconds to sleep
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall sleepms(uint32_t msec) {
    uint32_t ticks = msec / MS_PER_TICK;
    if (ticks == 0 && msec > 0) {
        ticks = 1;  /* Minimum 1 tick */
    }
    return sleep(ticks);
}

/**
 * unsleep - Remove process from sleep queue
 * 
 * @param pid: Process ID to wake
 * 
 * Returns: OK on success, SYSERR if not found
 */
syscall unsleep(pid32 pid) {
    intmask mask;
    syscall status;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (proctab[pid].prstate != PR_SLEEP) {
        restore(mask);
        return SYSERR;
    }
    
    /* Remove from sleep queue */
    status = getitem(pid, sleepq);
    if (status == OK) {
        proctab[pid].prstate = PR_SUSP;
        slnempty = nonempty(sleepq);
    }
    
    restore(mask);
    return status;
}

/*------------------------------------------------------------------------
 * Timer Management
 *------------------------------------------------------------------------*/

/**
 * timer_create - Create a new timer
 * 
 * @param callback: Function to call when timer expires
 * @param arg: Argument to pass to callback
 * @param delay: Delay in ticks before first expiration
 * @param period: Period for repeating (0 for one-shot)
 * 
 * Returns: Timer ID on success, SYSERR on error
 */
int32_t timer_create(timer_callback_t callback, void *arg,
                     uint32_t delay, uint32_t period) {
    int i;
    intmask mask;
    
    if (callback == NULL || delay == 0) {
        return SYSERR;
    }
    
    mask = disable();
    
    /* Find free timer slot */
    for (i = 0; i < NTIMERS; i++) {
        if (timertab[i].state == TMR_FREE) {
            timertab[i].state = TMR_ACTIVE;
            timertab[i].expires = clkticks + delay;
            timertab[i].period = period;
            timertab[i].callback = callback;
            timertab[i].arg = arg;
            
            restore(mask);
            return i;
        }
    }
    
    restore(mask);
    return SYSERR;
}

/**
 * timer_delete - Delete a timer
 * 
 * @param tid: Timer ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall timer_delete(int32_t tid) {
    intmask mask;
    
    if (tid < 0 || tid >= NTIMERS) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (timertab[tid].state == TMR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    timertab[tid].state = TMR_FREE;
    timertab[tid].callback = NULL;
    timertab[tid].arg = NULL;
    
    restore(mask);
    return OK;
}

/**
 * timer_stop - Stop a running timer
 * 
 * @param tid: Timer ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall timer_stop(int32_t tid) {
    intmask mask;
    
    if (tid < 0 || tid >= NTIMERS) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (timertab[tid].state != TMR_ACTIVE) {
        restore(mask);
        return SYSERR;
    }
    
    timertab[tid].state = TMR_STOPPED;
    
    restore(mask);
    return OK;
}

/**
 * timer_start - Restart a stopped timer
 * 
 * @param tid: Timer ID
 * @param delay: New delay (0 to use remaining time)
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall timer_start(int32_t tid, uint32_t delay) {
    intmask mask;
    
    if (tid < 0 || tid >= NTIMERS) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (timertab[tid].state == TMR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    if (delay > 0) {
        timertab[tid].expires = clkticks + delay;
    }
    timertab[tid].state = TMR_ACTIVE;
    
    restore(mask);
    return OK;
}

/**
 * process_timers - Process expired timers
 * 
 * Called from clock handler to check and fire expired timers.
 */
void process_timers(void) {
    int i;
    timer_callback_t callback;
    void *arg;
    
    for (i = 0; i < NTIMERS; i++) {
        if (timertab[i].state == TMR_ACTIVE) {
            if (clkticks >= timertab[i].expires) {
                /* Timer expired */
                callback = timertab[i].callback;
                arg = timertab[i].arg;
                
                if (timertab[i].period > 0) {
                    /* Periodic timer - reschedule */
                    timertab[i].expires = clkticks + timertab[i].period;
                } else {
                    /* One-shot timer */
                    timertab[i].state = TMR_EXPIRED;
                }
                
                /* Call callback */
                if (callback != NULL) {
                    callback(arg);
                }
            }
        }
    }
}

/*------------------------------------------------------------------------
 * Time Query Functions
 *------------------------------------------------------------------------*/

/**
 * gettime - Get current time in seconds since boot
 * 
 * Returns: Time in seconds
 */
uint32_t gettime(void) {
    return clktime;
}

/**
 * getticks - Get total ticks since boot
 * 
 * Returns: Total tick count (64-bit)
 */
uint64_t getticks(void) {
    return clkticks;
}

/**
 * getuptime - Get structured uptime information
 * 
 * @param days: Pointer to store days
 * @param hours: Pointer to store hours
 * @param minutes: Pointer to store minutes
 * @param seconds: Pointer to store seconds
 */
void getuptime(uint32_t *days, uint8_t *hours, 
               uint8_t *minutes, uint8_t *seconds) {
    intmask mask = disable();
    
    if (days) *days = uptime.days;
    if (hours) *hours = uptime.hours;
    if (minutes) *minutes = uptime.minutes;
    if (seconds) *seconds = uptime.seconds;
    
    restore(mask);
}

/**
 * ticks_to_ms - Convert ticks to milliseconds
 * 
 * @param ticks: Number of ticks
 * 
 * Returns: Milliseconds
 */
uint32_t ticks_to_ms(uint32_t ticks) {
    return ticks * MS_PER_TICK;
}

/**
 * ms_to_ticks - Convert milliseconds to ticks
 * 
 * @param ms: Milliseconds
 * 
 * Returns: Number of ticks
 */
uint32_t ms_to_ticks(uint32_t ms) {
    return ms / MS_PER_TICK;
}

/*------------------------------------------------------------------------
 * Time Quantum Management
 *------------------------------------------------------------------------*/

/**
 * setquantum - Set the time quantum for preemption
 * 
 * @param quantum: New quantum in ticks
 * 
 * Returns: Previous quantum value
 */
uint32_t setquantum(uint32_t quantum) {
    uint32_t old;
    intmask mask;
    
    if (quantum == 0) {
        quantum = 1;  /* Minimum 1 tick */
    }
    
    mask = disable();
    old = time_quantum;
    time_quantum = quantum;
    restore(mask);
    
    return old;
}

/**
 * getquantum - Get the current time quantum
 * 
 * Returns: Current quantum in ticks
 */
uint32_t getquantum(void) {
    return time_quantum;
}

/**
 * yield_quantum - Yield remaining time quantum
 * 
 * Forces immediate rescheduling, giving up remaining time slice.
 */
void yield_quantum(void) {
    intmask mask = disable();
    preempt_count = 0;
    resched();
    restore(mask);
}

/*------------------------------------------------------------------------
 * Delay Functions
 *------------------------------------------------------------------------*/

/**
 * delay - Busy-wait delay for specified ticks
 * 
 * @param ticks: Number of ticks to delay
 * 
 * Note: Uses busy waiting - should be avoided in normal code.
 * Use sleep() for non-busy delays.
 */
void delay(uint32_t ticks) {
    uint64_t target = clkticks + ticks;
    while (clkticks < target) {
        /* Busy wait */
    }
}

/**
 * udelay - Microsecond delay (busy-wait)
 * 
 * @param usec: Microseconds to delay
 * 
 * Note: Approximate timing, uses busy-waiting.
 */
void udelay(uint32_t usec) {
    /* This requires calibration based on CPU speed */
    volatile uint32_t i;
    uint32_t loops = usec * 10;  /* Approximate - needs calibration */
    
    for (i = 0; i < loops; i++) {
        /* Busy wait */
    }
}

/**
 * mdelay - Millisecond delay (busy-wait)
 * 
 * @param msec: Milliseconds to delay
 */
void mdelay(uint32_t msec) {
    while (msec-- > 0) {
        udelay(1000);
    }
}

/*------------------------------------------------------------------------
 * Clock Statistics
 *------------------------------------------------------------------------*/

/**
 * clock_info - Print clock subsystem information
 */
void clock_info(void) {
    int active_timers = 0;
    int i;
    
    for (i = 0; i < NTIMERS; i++) {
        if (timertab[i].state == TMR_ACTIVE) {
            active_timers++;
        }
    }
    
    kprintf("\n===== Clock Information =====\n");
    kprintf("Clock frequency:   %d Hz\n", CLKFREQ);
    kprintf("Time since boot:   %lu seconds\n", clktime);
    kprintf("Total ticks:       %llu\n", clkticks);
    kprintf("Uptime:            %lu days, %02d:%02d:%02d\n",
            uptime.days, uptime.hours, uptime.minutes, uptime.seconds);
    kprintf("Time quantum:      %lu ticks (%lu ms)\n", 
            time_quantum, ticks_to_ms(time_quantum));
    kprintf("Active timers:     %d / %d\n", active_timers, NTIMERS);
    kprintf("Sleep queue:       %s\n", slnempty ? "non-empty" : "empty");
}
