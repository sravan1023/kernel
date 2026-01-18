/*
 * semaphore.c - Xinu Semaphore Implementation
 * 
 * This file implements counting semaphores for process synchronization.
 * Semaphores are the primary synchronization primitive in Xinu.
 */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdbool.h>

/*------------------------------------------------------------------------
 * External Declarations
 *------------------------------------------------------------------------*/

extern sem_t semtab[];
extern proc_t proctab[];
extern pid32 currpid;
extern void resched(void);

/*------------------------------------------------------------------------
 * Semaphore Queue Management
 *------------------------------------------------------------------------*/

/* Semaphore free list */
static sid32 semfree = 0;

/* Number of active semaphores */
static int32_t nsem_used = 0;

/* Wait queue for each semaphore (uses process table indices) */
static struct {
    pid32 head;
    pid32 tail;
} sem_queues[NSEM];

/**
 * init_semaphores - Initialize the semaphore subsystem
 * 
 * Called during kernel initialization to set up semaphore table.
 */
void init_semaphores(void) {
    int i;
    
    /* Initialize all semaphores as free */
    for (i = 0; i < NSEM; i++) {
        semtab[i].count = 0;
        semtab[i].queue = -1;  /* -1 indicates free */
        sem_queues[i].head = -1;
        sem_queues[i].tail = -1;
    }
    
    /* Build free list (using count field as next pointer) */
    for (i = 0; i < NSEM - 1; i++) {
        semtab[i].count = i + 1;
    }
    semtab[NSEM - 1].count = -1;  /* End of free list */
    
    semfree = 0;
    nsem_used = 0;
}

/**
 * enqueue_sem - Add process to semaphore wait queue
 * 
 * @param sem: Semaphore ID
 * @param pid: Process ID to enqueue
 * 
 * Processes are queued in FIFO order (could also do priority order).
 */
static void enqueue_sem(sid32 sem, pid32 pid) {
    if (sem_queues[sem].tail == -1) {
        /* Empty queue */
        sem_queues[sem].head = pid;
        sem_queues[sem].tail = pid;
    } else {
        /* Add to tail */
        proctab[sem_queues[sem].tail].pwait = pid;
        sem_queues[sem].tail = pid;
    }
    proctab[pid].pwait = -1;  /* Mark end */
}

/**
 * dequeue_sem - Remove first process from semaphore wait queue
 * 
 * @param sem: Semaphore ID
 * 
 * Returns: Process ID of first waiting process, or -1 if empty
 */
static pid32 dequeue_sem(sid32 sem) {
    pid32 pid = sem_queues[sem].head;
    
    if (pid == -1) {
        return -1;  /* Empty queue */
    }
    
    sem_queues[sem].head = proctab[pid].pwait;
    if (sem_queues[sem].head == -1) {
        sem_queues[sem].tail = -1;  /* Queue now empty */
    }
    
    proctab[pid].pwait = -1;
    return pid;
}

/*------------------------------------------------------------------------
 * Semaphore Operations
 *------------------------------------------------------------------------*/

/**
 * semcreate - Create and initialize a semaphore
 * 
 * @param count: Initial count value (>= 0)
 * 
 * Returns: Semaphore ID on success, SYSERR on failure
 * 
 * A semaphore with count > 0 means that many processes can
 * proceed without blocking. Count of 1 creates a mutex.
 * 
 * Example:
 *   sid32 mutex = semcreate(1);     // Binary semaphore (mutex)
 *   sid32 rsrc = semcreate(5);      // Resource counter (5 available)
 */
sid32 semcreate(int32_t count) {
    intmask mask;
    sid32 sem;
    
    /* Count must be non-negative */
    if (count < 0) {
        return SYSERR;
    }
    
    mask = disable();
    
    /* Check if any semaphores available */
    if (semfree == -1) {
        restore(mask);
        return SYSERR;
    }
    
    /* Allocate semaphore from free list */
    sem = semfree;
    semfree = semtab[sem].count;  /* Next free */
    
    /* Initialize semaphore */
    semtab[sem].count = count;
    semtab[sem].queue = 0;  /* Mark as allocated (not -1) */
    sem_queues[sem].head = -1;
    sem_queues[sem].tail = -1;
    
    nsem_used++;
    
    restore(mask);
    return sem;
}

/**
 * semdelete - Delete a semaphore
 * 
 * @param sem: Semaphore ID to delete
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Any processes waiting on the semaphore are made ready
 * and will receive SYSERR from their wait() call.
 */
syscall semdelete(sid32 sem) {
    intmask mask;
    pid32 pid;
    
    /* Validate semaphore ID */
    if (sem < 0 || sem >= NSEM) {
        return SYSERR;
    }
    
    mask = disable();
    
    /* Check if semaphore is allocated */
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    /* Wake all waiting processes */
    while ((pid = dequeue_sem(sem)) != -1) {
        proctab[pid].pstate = PR_READY;
        /* Process will return SYSERR from wait */
    }
    
    /* Return semaphore to free list */
    semtab[sem].queue = -1;
    semtab[sem].count = semfree;
    semfree = sem;
    
    nsem_used--;
    
    resched();  /* Woken processes may have higher priority */
    restore(mask);
    return OK;
}

/**
 * semreset - Reset a semaphore to a new count
 * 
 * @param sem: Semaphore ID
 * @param count: New count value
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Wakes all waiting processes (if any) and sets new count.
 */
syscall semreset(sid32 sem, int32_t count) {
    intmask mask;
    pid32 pid;
    
    if (sem < 0 || sem >= NSEM || count < 0) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    /* Wake all waiting processes */
    while ((pid = dequeue_sem(sem)) != -1) {
        proctab[pid].pstate = PR_READY;
    }
    
    /* Set new count */
    semtab[sem].count = count;
    
    resched();
    restore(mask);
    return OK;
}

/**
 * wait - Decrement semaphore, block if count would go negative
 * 
 * @param sem: Semaphore ID
 * 
 * Returns: OK on success, SYSERR on error (deleted semaphore)
 * 
 * The classic P() operation. If count > 0, decrement and return.
 * Otherwise, block until signal() is called.
 * 
 * Example:
 *   wait(mutex);
 *   // Critical section
 *   signal(mutex);
 */
syscall wait(sid32 sem) {
    intmask mask;
    
    if (sem < 0 || sem >= NSEM) {
        return SYSERR;
    }
    
    mask = disable();
    
    /* Check if semaphore is valid */
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    semtab[sem].count--;
    
    if (semtab[sem].count < 0) {
        /* Must block */
        proctab[currpid].pstate = PR_WAIT;
        proctab[currpid].pwait = sem;  /* Store which semaphore */
        enqueue_sem(sem, currpid);
        resched();
        
        /* When we wake up, check if semaphore was deleted */
        if (semtab[sem].queue == -1) {
            restore(mask);
            return SYSERR;
        }
    }
    
    restore(mask);
    return OK;
}

/**
 * signal - Increment semaphore, wake one waiting process
 * 
 * @param sem: Semaphore ID
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * The classic V() operation. Increments count and, if any
 * processes are waiting, wakes the first one.
 */
syscall signal(sid32 sem) {
    intmask mask;
    pid32 pid;
    
    if (sem < 0 || sem >= NSEM) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    semtab[sem].count++;
    
    if (semtab[sem].count <= 0) {
        /* Wake one waiting process */
        pid = dequeue_sem(sem);
        if (pid != -1) {
            proctab[pid].pstate = PR_READY;
            proctab[pid].pwait = -1;
            resched();
        }
    }
    
    restore(mask);
    return OK;
}

/**
 * signaln - Signal a semaphore n times
 * 
 * @param sem: Semaphore ID
 * @param n: Number of times to signal
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Equivalent to calling signal() n times, but more efficient.
 */
syscall signaln(sid32 sem, int32_t n) {
    intmask mask;
    pid32 pid;
    
    if (sem < 0 || sem >= NSEM || n <= 0) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    while (n > 0) {
        semtab[sem].count++;
        
        if (semtab[sem].count <= 0) {
            pid = dequeue_sem(sem);
            if (pid != -1) {
                proctab[pid].pstate = PR_READY;
                proctab[pid].pwait = -1;
            }
        }
        n--;
    }
    
    resched();
    restore(mask);
    return OK;
}

/**
 * semcount - Get current semaphore count
 * 
 * @param sem: Semaphore ID
 * 
 * Returns: Current count, or SYSERR on error
 */
int32_t semcount(sid32 sem) {
    intmask mask;
    int32_t count;
    
    if (sem < 0 || sem >= NSEM) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    count = semtab[sem].count;
    
    restore(mask);
    return count;
}

/*------------------------------------------------------------------------
 * Extended Semaphore Operations
 *------------------------------------------------------------------------*/

/**
 * trywait - Non-blocking wait on semaphore
 * 
 * @param sem: Semaphore ID
 * 
 * Returns: OK if acquired, SYSERR if would block or error
 * 
 * Attempts to decrement the semaphore without blocking.
 * Returns immediately if the semaphore is not available.
 */
syscall trywait(sid32 sem) {
    intmask mask;
    
    if (sem < 0 || sem >= NSEM) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    if (semtab[sem].count > 0) {
        semtab[sem].count--;
        restore(mask);
        return OK;
    }
    
    restore(mask);
    return SYSERR;  /* Would block */
}

/**
 * timedwait - Wait on semaphore with timeout
 * 
 * @param sem: Semaphore ID
 * @param timeout: Maximum wait time in milliseconds
 * 
 * Returns: OK if acquired, TIMEOUT if timed out, SYSERR on error
 * 
 * Waits for the semaphore for at most 'timeout' milliseconds.
 */
syscall timedwait(sid32 sem, uint32_t timeout) {
    intmask mask;
    
    if (sem < 0 || sem >= NSEM) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    /* Try immediate acquire */
    if (semtab[sem].count > 0) {
        semtab[sem].count--;
        restore(mask);
        return OK;
    }
    
    /* Must wait with timeout */
    semtab[sem].count--;
    proctab[currpid].pstate = PR_WAIT;
    proctab[currpid].pwait = sem;
    proctab[currpid].pargs = timeout;  /* Store timeout */
    enqueue_sem(sem, currpid);
    
    resched();
    
    /* Check why we woke up */
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;  /* Semaphore deleted */
    }
    
    /* Check if we timed out (would need clock handler support) */
    /* For now, assume we got the semaphore */
    
    restore(mask);
    return OK;
}

/*------------------------------------------------------------------------
 * Semaphore Information
 *------------------------------------------------------------------------*/

/**
 * sem_count_used - Get number of semaphores in use
 * 
 * Returns: Number of allocated semaphores
 */
int32_t sem_count_used(void) {
    return nsem_used;
}

/**
 * sem_count_free - Get number of free semaphores
 * 
 * Returns: Number of available semaphores
 */
int32_t sem_count_free(void) {
    return NSEM - nsem_used;
}

/**
 * seminfo - Get information about a semaphore
 * 
 * @param sem: Semaphore ID
 * @param count: Pointer to store count (can be NULL)
 * @param nwait: Pointer to store number of waiters (can be NULL)
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall seminfo(sid32 sem, int32_t *count, int32_t *nwait) {
    intmask mask;
    pid32 pid;
    int32_t waiters = 0;
    
    if (sem < 0 || sem >= NSEM) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    if (count != NULL) {
        *count = semtab[sem].count;
    }
    
    if (nwait != NULL) {
        /* Count waiters */
        pid = sem_queues[sem].head;
        while (pid != -1) {
            waiters++;
            pid = proctab[pid].pwait;
        }
        *nwait = waiters;
    }
    
    restore(mask);
    return OK;
}

/*------------------------------------------------------------------------
 * Mutex Operations (Binary Semaphore Convenience)
 *------------------------------------------------------------------------*/

/**
 * mutex_create - Create a mutex (binary semaphore initialized to 1)
 * 
 * Returns: Semaphore ID, or SYSERR on error
 */
sid32 mutex_create(void) {
    return semcreate(1);
}

/**
 * mutex_lock - Acquire mutex
 * 
 * @param mutex: Mutex (semaphore) ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall mutex_lock(sid32 mutex) {
    return wait(mutex);
}

/**
 * mutex_trylock - Try to acquire mutex without blocking
 * 
 * @param mutex: Mutex (semaphore) ID
 * 
 * Returns: OK if acquired, SYSERR if would block
 */
syscall mutex_trylock(sid32 mutex) {
    return trywait(mutex);
}

/**
 * mutex_unlock - Release mutex
 * 
 * @param mutex: Mutex (semaphore) ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall mutex_unlock(sid32 mutex) {
    return signal(mutex);
}

/**
 * mutex_destroy - Destroy a mutex
 * 
 * @param mutex: Mutex (semaphore) ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall mutex_destroy(sid32 mutex) {
    return semdelete(mutex);
}
