/* clock.c - Clock and timer management */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdbool.h>

#define CLKFREQ         1000
#define CLKTICKS_PER_SEC CLKFREQ
#define MS_PER_TICK     (1000/CLKFREQ)

#define NTIMERS         32

#define TMR_FREE        0
#define TMR_ACTIVE      1
#define TMR_EXPIRED     2
#define TMR_STOPPED     3

#define MAXSLEEPTIME    0x7FFFFFFF

/*Clock Data Structures*/

typedef void (*timer_callback_t)(void *arg);

/* Timer structure */
typedef struct timer {
    uint8_t     state; 
    uint32_t    expires; 
    uint32_t    period;
    timer_callback_t callback;  /* Callback function */
    void        *arg;           /* Callback argument */
} timer_t;

/* Timer table */
static timer_t timertab[NTIMERS];

volatile uint32_t clktime = 0;  
volatile uint32_t ctr1000 = 0; 
volatile uint64_t clkticks = 0; 
volatile int32_t  clkdefer = 0; 
static uint32_t preempt_count = 0;
#define QUANTUM         10  
static uint32_t time_quantum = QUANTUM;
static qid32 sleepq = -1;
static int32_t slnempty = 0;

/* System uptime */
static struct {
    uint32_t days;
    uint8_t  hours;
    uint8_t  minutes;
    uint8_t  seconds;
    uint32_t ticks;
} uptime;

/* Initialize clock subsystem */
syscall clkinit(void) {
    int i;
    
    clktime = 0;
    ctr1000 = 0;
    clkticks = 0;
    clkdefer = 0;
    preempt_count = time_quantum;
    
    uptime.days = 0;
    uptime.hours = 0;
    uptime.minutes = 0;
    uptime.seconds = 0;
    uptime.ticks = 0;
    
    for (i = 0; i < NTIMERS; i++) {
        timertab[i].state = TMR_FREE;
        timertab[i].expires = 0;
        timertab[i].period = 0;
        timertab[i].callback = NULL;
        timertab[i].arg = NULL;
    }
    
    sleepq = newqueue();
    if (sleepq == SYSERR) {
        return SYSERR;
    }
    slnempty = 0;
    
    return OK;
}

/* Clock interrupt handler */
void clkhandler(void) {
    pid32 pid;
    
    clkticks++;
    
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
    
    if (clkdefer > 0) {
        clkdefer++;
        return;
    }
    
    process_timers();
    wakeup();
    
    if (--preempt_count <= 0) {
        preempt_count = time_quantum;
        resched();
    }
}

/* Defer clock processing */
void defer_clock(void) {
    intmask mask = disable();
    clkdefer = 1;
    restore(mask);
}

/* Resume clock processing */
void undefer_clock(void) {
    intmask mask = disable();
    
    if (clkdefer > 1) {
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

/* Wake processes whose sleep time has expired */
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
    
    if (slnempty) {
        pid = firstid(sleepq);
        if (pid != EMPTY && pid >= 0 && pid < NPROC) {
            proctab[pid].pargs--;
        }
    }
}

/* Put current process to sleep */
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
    
    proctab[currpid].prstate = PR_SLEEP;
    proctab[currpid].pargs = delay;
    
    insertd(currpid, sleepq, delay);
    slnempty = 1;
    
    resched();
    
    restore(mask);
    return OK;
}

/* Sleep for specified milliseconds */
syscall sleepms(uint32_t msec) {
    uint32_t ticks = msec / MS_PER_TICK;
    if (ticks == 0 && msec > 0) {
        ticks = 1;
    }
    return sleep(ticks);
}

/* Remove process from sleep queue */
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
    
    status = getitem(pid, sleepq);
    if (status == OK) {
        proctab[pid].prstate = PR_SUSP;
        slnempty = nonempty(sleepq);
    }
    
    restore(mask);
    return status;
}

/* Create a new timer */
int32_t timer_create(timer_callback_t callback, void *arg,
                     uint32_t delay, uint32_t period) {
    int i;
    intmask mask;
    
    if (callback == NULL || delay == 0) {
        return SYSERR;
    }
    
    mask = disable();
    
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

/* Delete a timer */
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

/* Stop a running timer */
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

/* Restart a stopped timer */
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

/* Process expired timers */
void process_timers(void) {
    int i;
    timer_callback_t callback;
    void *arg;
    
    for (i = 0; i < NTIMERS; i++) {
        if (timertab[i].state == TMR_ACTIVE) {
            if (clkticks >= timertab[i].expires) {
                callback = timertab[i].callback;
                arg = timertab[i].arg;
                
                if (timertab[i].period > 0) {
                    timertab[i].expires = clkticks + timertab[i].period;
                } else {
                    timertab[i].state = TMR_EXPIRED;
                }
                
                if (callback != NULL) {
                    callback(arg);
                }
            }
        }
    }
}

/* Get current time in seconds since boot */
uint32_t gettime(void) {
    return clktime;
}

/* Get total ticks since boot */
uint64_t getticks(void) {
    return clkticks;
}

/* Get structured uptime information */
void getuptime(uint32_t *days, uint8_t *hours, 
               uint8_t *minutes, uint8_t *seconds) {
    intmask mask = disable();
    
    if (days) *days = uptime.days;
    if (hours) *hours = uptime.hours;
    if (minutes) *minutes = uptime.minutes;
    if (seconds) *seconds = uptime.seconds;
    
    restore(mask);
}

/* Convert ticks to milliseconds */
uint32_t ticks_to_ms(uint32_t ticks) {
    return ticks * MS_PER_TICK;
}

/* Convert milliseconds to ticks */
uint32_t ms_to_ticks(uint32_t ms) {
    return ms / MS_PER_TICK;
}

/* Set the time quantum for preemption */
uint32_t setquantum(uint32_t quantum) {
    uint32_t old;
    intmask mask;
    
    if (quantum == 0) {
        quantum = 1;
    }
    
    mask = disable();
    old = time_quantum;
    time_quantum = quantum;
    restore(mask);
    
    return old;
}

/* Get the current time quantum */
uint32_t getquantum(void) {
    return time_quantum;
}

/* Yield remaining time quantum */
void yield_quantum(void) {
    intmask mask = disable();
    preempt_count = 0;
    resched();
    restore(mask);
}

/* Busy-wait delay */
void delay(uint32_t ticks) {
    uint64_t target = clkticks + ticks;
    while (clkticks < target) {
    }
}

/* Microsecond busy-wait delay */
void udelay(uint32_t usec) {
    volatile uint32_t i;
    uint32_t loops = usec * 10;
    
    for (i = 0; i < loops; i++) {
    }
}

/* Millisecond busy-wait delay */
void mdelay(uint32_t msec) {
    while (msec-- > 0) {
        udelay(1000);
    }
}

/* Print clock information */
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
