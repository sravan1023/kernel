/* semaphore.c - Semaphore implementation */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdbool.h>

extern sem_t semtab[];
extern proc_t proctab[];
extern pid32 currpid;
extern void resched(void);

static sid32 semfree = 0;
static int32_t nsem_used = 0;

static struct {
    pid32 head;
    pid32 tail;
} sem_queues[NSEM];

/* Initialize semaphore subsystem */
void init_semaphores(void) {
    int i;
    
    for (i = 0; i < NSEM; i++) {
        semtab[i].count = 0;
        semtab[i].queue = -1;
        sem_queues[i].head = -1;
        sem_queues[i].tail = -1;
    }
    
    for (i = 0; i < NSEM - 1; i++) {
        semtab[i].count = i + 1;
    }
    semtab[NSEM - 1].count = -1;
    
    semfree = 0;
    nsem_used = 0;
}

/* Add process to semaphore wait queue (FIFO) */
static void enqueue_sem(sid32 sem, pid32 pid) {
    if (sem_queues[sem].tail == -1) {
        sem_queues[sem].head = pid;
        sem_queues[sem].tail = pid;
    } else {
        proctab[sem_queues[sem].tail].pwait = pid;
        sem_queues[sem].tail = pid;
    }
    proctab[pid].pwait = -1;
}

/* Remove first process from semaphore wait queue */
/* Remove first process from semaphore wait queue */
static pid32 dequeue_sem(sid32 sem) {
    pid32 pid = sem_queues[sem].head;
    
    if (pid == -1) {
        return -1;
    }
    
    sem_queues[sem].head = proctab[pid].pwait;
    if (sem_queues[sem].head == -1) {
        sem_queues[sem].tail = -1;
    }
    
    proctab[pid].pwait = -1;
    return pid;
}

/* Create a semaphore with initial count */
sid32 semcreate(int32_t count) {
    intmask mask;
    sid32 sem;
    
    if (count < 0) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semfree == -1) {
        restore(mask);
        return SYSERR;
    }
    
    sem = semfree;
    semfree = semtab[sem].count;
    
    /* Initialize semaphore */
    semtab[sem].count = count;
    semtab[sem].queue = 0;  /* Mark as allocated (not -1) */
    sem_queues[sem].head = -1;
    sem_queues[sem].tail = -1;
    
    nsem_used++;
    
    restore(mask);
    return sem;
}

/* Delete a semaphore and wake all waiting processes */
syscall semdelete(sid32 sem) {
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
    
    while ((pid = dequeue_sem(sem)) != -1) {
        proctab[pid].pstate = PR_READY;
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

/* Wait on semaphore (P operation) */
syscall wait(sid32 sem) {
    intmask mask;
    
    if (sem < 0 || sem >= NSEM) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (semtab[sem].queue == -1) {
        restore(mask);
        return SYSERR;
    }
    
    semtab[sem].count--;
    
    if (semtab[sem].count < 0) {
        proctab[currpid].pstate = PR_WAIT;
        proctab[currpid].pwait = sem;
        enqueue_sem(sem, currpid);
        resched();
        
        /* Check if semaphore was deleted while waiting */
        if (semtab[sem].queue == -1) {
            restore(mask);
            return SYSERR;
        }
    }
    
    restore(mask);
    return OK;
}

/* Signal semaphore (V operation) */
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

/* Signal a semaphore n times */
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

/* Get current semaphore count */
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

/* Non-blocking wait on semaphore */
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
    return SYSERR;
}

/* Wait on semaphore with timeout */
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
