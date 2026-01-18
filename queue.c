/*
 * queue.c - Xinu Queue Management Implementation
 * 
 * This file implements queue data structures used throughout the kernel
 * for managing lists of processes (ready queue, wait queues, etc.).
 */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdbool.h>

/*------------------------------------------------------------------------
 * Queue Constants
 *------------------------------------------------------------------------*/

#define NQENT       (NPROC + NSEM + 4)  /* Number of queue entries */
#define EMPTY       -1                   /* Empty queue marker */

/* Queue entry states */
#define QE_FREE     0       /* Entry is free */
#define QE_HEAD     1       /* Entry is queue head */
#define QE_TAIL     2       /* Entry is queue tail */
#define QE_PROC     3       /* Entry contains a process */

/*------------------------------------------------------------------------
 * Queue Structures
 *------------------------------------------------------------------------*/

/**
 * Queue entry structure
 * 
 * Uses array-based implementation with indices as pointers.
 * Each entry can link to previous and next entries.
 */
typedef struct qentry {
    pid32   key;        /* Key for ordering (priority or time) */
    qid32   next;       /* Index of next entry */
    qid32   prev;       /* Index of previous entry */
    uint8_t state;      /* Entry state (free, head, tail, proc) */
} qentry_t;

/* Queue entry table */
static qentry_t queuetab[NQENT];

/* Free list of queue entries */
static qid32 qfree = EMPTY;

/* Number of queues allocated */
static int32_t nqueues = 0;

/*------------------------------------------------------------------------
 * Queue Initialization
 *------------------------------------------------------------------------*/

/**
 * init_queues - Initialize the queue subsystem
 * 
 * Sets up the queue entry table and free list.
 */
void init_queues(void) {
    int i;
    
    /* Initialize all entries as free */
    for (i = 0; i < NQENT; i++) {
        queuetab[i].key = 0;
        queuetab[i].next = i + 1;
        queuetab[i].prev = i - 1;
        queuetab[i].state = QE_FREE;
    }
    
    /* Fix up free list */
    queuetab[0].prev = EMPTY;
    queuetab[NQENT - 1].next = EMPTY;
    qfree = 0;
    nqueues = 0;
}

/**
 * newqueue - Allocate and initialize a new queue
 * 
 * Returns: Queue ID (index of head entry), or SYSERR on error
 * 
 * A queue consists of a head and tail entry. The head's next
 * points to the first element, and the tail's prev points to
 * the last element.
 */
qid32 newqueue(void) {
    qid32 head, tail;
    intmask mask;
    
    mask = disable();
    
    /* Need two entries (head and tail) */
    if (qfree == EMPTY || queuetab[qfree].next == EMPTY) {
        restore(mask);
        return SYSERR;
    }
    
    /* Allocate head */
    head = qfree;
    qfree = queuetab[qfree].next;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = EMPTY;
    }
    
    /* Allocate tail */
    tail = qfree;
    qfree = queuetab[qfree].next;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = EMPTY;
    }
    
    /* Initialize head */
    queuetab[head].state = QE_HEAD;
    queuetab[head].key = MAXINT;
    queuetab[head].prev = EMPTY;
    queuetab[head].next = tail;
    
    /* Initialize tail */
    queuetab[tail].state = QE_TAIL;
    queuetab[tail].key = MININT;
    queuetab[tail].prev = head;
    queuetab[tail].next = EMPTY;
    
    nqueues++;
    
    restore(mask);
    return head;
}

/**
 * freequeue - Free a queue
 * 
 * @param q: Queue ID to free
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Note: Queue must be empty before freeing.
 */
syscall freequeue(qid32 q) {
    qid32 head, tail;
    intmask mask;
    
    if (q < 0 || q >= NQENT) {
        return SYSERR;
    }
    
    mask = disable();
    
    /* Verify this is a queue head */
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return SYSERR;
    }
    
    head = q;
    tail = queuetab[head].next;
    
    /* Check if queue is empty */
    if (tail == EMPTY || queuetab[tail].state != QE_TAIL) {
        restore(mask);
        return SYSERR;
    }
    
    if (queuetab[tail].prev != head) {
        /* Queue not empty */
        restore(mask);
        return SYSERR;
    }
    
    /* Return head to free list */
    queuetab[head].state = QE_FREE;
    queuetab[head].next = qfree;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = head;
    }
    qfree = head;
    
    /* Return tail to free list */
    queuetab[tail].state = QE_FREE;
    queuetab[tail].next = qfree;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = tail;
    }
    qfree = tail;
    
    nqueues--;
    
    restore(mask);
    return OK;
}

/*------------------------------------------------------------------------
 * Basic Queue Operations
 *------------------------------------------------------------------------*/

/**
 * isempty - Check if queue is empty
 * 
 * @param q: Queue ID
 * 
 * Returns: true if empty, false otherwise
 */
bool isempty(qid32 q) {
    qid32 tail;
    
    if (q < 0 || q >= NQENT || queuetab[q].state != QE_HEAD) {
        return true;  /* Invalid queue treated as empty */
    }
    
    tail = queuetab[q].next;
    return (queuetab[tail].state == QE_TAIL && queuetab[tail].prev == q);
}

/**
 * nonempty - Check if queue is non-empty
 * 
 * @param q: Queue ID
 * 
 * Returns: true if not empty, false otherwise
 */
bool nonempty(qid32 q) {
    return !isempty(q);
}

/**
 * firstid - Get ID of first process in queue
 * 
 * @param q: Queue ID
 * 
 * Returns: Process ID of first entry, or EMPTY if queue is empty
 */
pid32 firstid(qid32 q) {
    qid32 first;
    
    if (q < 0 || q >= NQENT || queuetab[q].state != QE_HEAD) {
        return EMPTY;
    }
    
    first = queuetab[q].next;
    if (queuetab[first].state == QE_TAIL) {
        return EMPTY;  /* Queue is empty */
    }
    
    return queuetab[first].key;
}

/**
 * lastid - Get ID of last process in queue
 * 
 * @param q: Queue ID
 * 
 * Returns: Process ID of last entry, or EMPTY if queue is empty
 */
pid32 lastid(qid32 q) {
    qid32 tail, last;
    
    if (q < 0 || q >= NQENT || queuetab[q].state != QE_HEAD) {
        return EMPTY;
    }
    
    tail = queuetab[q].next;
    while (queuetab[tail].state != QE_TAIL) {
        tail = queuetab[tail].next;
    }
    
    last = queuetab[tail].prev;
    if (queuetab[last].state == QE_HEAD) {
        return EMPTY;  /* Queue is empty */
    }
    
    return queuetab[last].key;
}

/*------------------------------------------------------------------------
 * Queue Insertion Operations
 *------------------------------------------------------------------------*/

/**
 * enqueue - Insert process at tail of queue
 * 
 * @param pid: Process ID to insert
 * @param q: Queue ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall enqueue(pid32 pid, qid32 q) {
    qid32 tail, prev, entry;
    intmask mask;
    
    if (q < 0 || q >= NQENT || pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return SYSERR;
    }
    
    /* Find tail */
    tail = queuetab[q].next;
    while (queuetab[tail].state != QE_TAIL) {
        tail = queuetab[tail].next;
    }
    
    /* Allocate new entry */
    if (qfree == EMPTY) {
        restore(mask);
        return SYSERR;
    }
    
    entry = qfree;
    qfree = queuetab[qfree].next;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = EMPTY;
    }
    
    /* Insert before tail */
    prev = queuetab[tail].prev;
    
    queuetab[entry].state = QE_PROC;
    queuetab[entry].key = pid;
    queuetab[entry].next = tail;
    queuetab[entry].prev = prev;
    
    queuetab[prev].next = entry;
    queuetab[tail].prev = entry;
    
    restore(mask);
    return OK;
}

/**
 * dequeue - Remove and return first process from queue
 * 
 * @param q: Queue ID
 * 
 * Returns: Process ID, or EMPTY if queue is empty
 */
pid32 dequeue(qid32 q) {
    qid32 first, next;
    pid32 pid;
    intmask mask;
    
    if (q < 0 || q >= NQENT) {
        return EMPTY;
    }
    
    mask = disable();
    
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return EMPTY;
    }
    
    first = queuetab[q].next;
    
    if (queuetab[first].state == QE_TAIL) {
        /* Queue is empty */
        restore(mask);
        return EMPTY;
    }
    
    /* Remove first entry */
    pid = queuetab[first].key;
    next = queuetab[first].next;
    
    queuetab[q].next = next;
    queuetab[next].prev = q;
    
    /* Return entry to free list */
    queuetab[first].state = QE_FREE;
    queuetab[first].next = qfree;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = first;
    }
    qfree = first;
    
    restore(mask);
    return pid;
}

/*------------------------------------------------------------------------
 * Priority Queue Operations
 *------------------------------------------------------------------------*/

/**
 * insert - Insert process into queue in key (priority) order
 * 
 * @param pid: Process ID to insert
 * @param q: Queue ID
 * @param key: Key value for ordering (typically priority)
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Inserts in descending key order (highest key first).
 */
syscall insert(pid32 pid, qid32 q, int32_t key) {
    qid32 curr, entry;
    intmask mask;
    
    if (q < 0 || q >= NQENT || pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return SYSERR;
    }
    
    /* Allocate new entry */
    if (qfree == EMPTY) {
        restore(mask);
        return SYSERR;
    }
    
    entry = qfree;
    qfree = queuetab[qfree].next;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = EMPTY;
    }
    
    /* Find insertion point (descending order) */
    curr = queuetab[q].next;
    while (queuetab[curr].state == QE_PROC && queuetab[curr].key >= key) {
        curr = queuetab[curr].next;
    }
    
    /* Insert before curr */
    queuetab[entry].state = QE_PROC;
    queuetab[entry].key = pid;  /* Store PID in key field */
    queuetab[entry].next = curr;
    queuetab[entry].prev = queuetab[curr].prev;
    
    queuetab[queuetab[curr].prev].next = entry;
    queuetab[curr].prev = entry;
    
    restore(mask);
    return OK;
}

/**
 * insertd - Insert into delta list (for timing)
 * 
 * @param pid: Process ID
 * @param q: Queue ID
 * @param key: Delta value (time relative to previous)
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Used for sleep queues where each entry stores the delta time
 * from the previous entry. Total time is sum of deltas.
 */
syscall insertd(pid32 pid, qid32 q, int32_t key) {
    qid32 curr, entry;
    intmask mask;
    
    if (q < 0 || q >= NQENT || pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return SYSERR;
    }
    
    /* Allocate entry */
    if (qfree == EMPTY) {
        restore(mask);
        return SYSERR;
    }
    
    entry = qfree;
    qfree = queuetab[qfree].next;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = EMPTY;
    }
    
    /* Find insertion point and adjust deltas */
    curr = queuetab[q].next;
    while (queuetab[curr].state == QE_PROC) {
        if (key < proctab[queuetab[curr].key].pargs) {
            /* Reduce next entry's delta */
            proctab[queuetab[curr].key].pargs -= key;
            break;
        }
        key -= proctab[queuetab[curr].key].pargs;
        curr = queuetab[curr].next;
    }
    
    /* Insert before curr */
    queuetab[entry].state = QE_PROC;
    queuetab[entry].key = pid;
    queuetab[entry].next = curr;
    queuetab[entry].prev = queuetab[curr].prev;
    
    proctab[pid].pargs = key;  /* Store delta in process args */
    
    queuetab[queuetab[curr].prev].next = entry;
    queuetab[curr].prev = entry;
    
    restore(mask);
    return OK;
}

/*------------------------------------------------------------------------
 * Queue Removal Operations
 *------------------------------------------------------------------------*/

/**
 * getfirst - Remove and return first process from queue
 * 
 * @param q: Queue ID
 * 
 * Returns: Process ID, or EMPTY if queue is empty
 */
pid32 getfirst(qid32 q) {
    return dequeue(q);
}

/**
 * getlast - Remove and return last process from queue
 * 
 * @param q: Queue ID
 * 
 * Returns: Process ID, or EMPTY if queue is empty
 */
pid32 getlast(qid32 q) {
    qid32 tail, last;
    pid32 pid;
    intmask mask;
    
    if (q < 0 || q >= NQENT) {
        return EMPTY;
    }
    
    mask = disable();
    
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return EMPTY;
    }
    
    /* Find tail */
    tail = queuetab[q].next;
    while (queuetab[tail].state != QE_TAIL) {
        tail = queuetab[tail].next;
    }
    
    last = queuetab[tail].prev;
    if (queuetab[last].state == QE_HEAD) {
        /* Queue is empty */
        restore(mask);
        return EMPTY;
    }
    
    /* Remove last entry */
    pid = queuetab[last].key;
    queuetab[queuetab[last].prev].next = tail;
    queuetab[tail].prev = queuetab[last].prev;
    
    /* Return entry to free list */
    queuetab[last].state = QE_FREE;
    queuetab[last].next = qfree;
    if (qfree != EMPTY) {
        queuetab[qfree].prev = last;
    }
    qfree = last;
    
    restore(mask);
    return pid;
}

/**
 * getitem - Remove specific process from queue
 * 
 * @param pid: Process ID to remove
 * @param q: Queue ID
 * 
 * Returns: OK on success, SYSERR if not found
 */
syscall getitem(pid32 pid, qid32 q) {
    qid32 curr;
    intmask mask;
    
    if (q < 0 || q >= NQENT || pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return SYSERR;
    }
    
    /* Find the entry */
    curr = queuetab[q].next;
    while (queuetab[curr].state == QE_PROC) {
        if (queuetab[curr].key == pid) {
            /* Found - remove it */
            queuetab[queuetab[curr].prev].next = queuetab[curr].next;
            queuetab[queuetab[curr].next].prev = queuetab[curr].prev;
            
            /* Return to free list */
            queuetab[curr].state = QE_FREE;
            queuetab[curr].next = qfree;
            if (qfree != EMPTY) {
                queuetab[qfree].prev = curr;
            }
            qfree = curr;
            
            restore(mask);
            return OK;
        }
        curr = queuetab[curr].next;
    }
    
    restore(mask);
    return SYSERR;  /* Not found */
}

/*------------------------------------------------------------------------
 * Queue Information
 *------------------------------------------------------------------------*/

/**
 * queuelen - Get number of entries in queue
 * 
 * @param q: Queue ID
 * 
 * Returns: Number of entries, or -1 on error
 */
int32_t queuelen(qid32 q) {
    qid32 curr;
    int32_t count = 0;
    intmask mask;
    
    if (q < 0 || q >= NQENT) {
        return -1;
    }
    
    mask = disable();
    
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return -1;
    }
    
    curr = queuetab[q].next;
    while (queuetab[curr].state == QE_PROC) {
        count++;
        curr = queuetab[curr].next;
    }
    
    restore(mask);
    return count;
}

/**
 * inqueue - Check if process is in queue
 * 
 * @param pid: Process ID to check
 * @param q: Queue ID
 * 
 * Returns: true if in queue, false otherwise
 */
bool inqueue(pid32 pid, qid32 q) {
    qid32 curr;
    intmask mask;
    
    if (q < 0 || q >= NQENT || pid < 0 || pid >= NPROC) {
        return false;
    }
    
    mask = disable();
    
    if (queuetab[q].state != QE_HEAD) {
        restore(mask);
        return false;
    }
    
    curr = queuetab[q].next;
    while (queuetab[curr].state == QE_PROC) {
        if (queuetab[curr].key == pid) {
            restore(mask);
            return true;
        }
        curr = queuetab[curr].next;
    }
    
    restore(mask);
    return false;
}
