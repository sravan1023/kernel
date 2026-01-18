/*
 * message.c - Xinu Inter-Process Message Passing
 * 
 * This file implements the message passing system for inter-process
 * communication (IPC) in the Xinu operating system.
 */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdbool.h>

/*------------------------------------------------------------------------
 * Message Configuration
 *------------------------------------------------------------------------*/

/* Message box size for buffered messages */
#define MSG_BOX_SIZE        16          /* Messages per mailbox */
#define MSG_TIMEOUT_INF     0xFFFFFFFF  /* Infinite timeout */

/*------------------------------------------------------------------------
 * Message Box Structure (Optional Buffered Messaging)
 *------------------------------------------------------------------------*/

/**
 * Message box for buffered inter-process messaging
 * 
 * Each process can optionally have a mailbox that stores
 * multiple messages in a FIFO queue.
 */
typedef struct msgbox {
    umsg32      messages[MSG_BOX_SIZE];     /* Message buffer */
    uint32_t    head;                       /* Head index (next read) */
    uint32_t    tail;                       /* Tail index (next write) */
    uint32_t    count;                      /* Number of messages */
    sid32       mutex;                      /* Access mutex */
    sid32       items;                      /* Items semaphore */
    sid32       slots;                      /* Free slots semaphore */
    bool        active;                     /* Is mailbox active */
} msgbox_t;

/* Mailbox array - one per process */
static msgbox_t mailboxes[NPROC];

/*------------------------------------------------------------------------
 * Message Statistics
 *------------------------------------------------------------------------*/

static struct {
    uint64_t    sent;           /* Total messages sent */
    uint64_t    received;       /* Total messages received */
    uint64_t    failed;         /* Failed send/receive operations */
    uint64_t    timeouts;       /* Timed-out receives */
} msg_stats;

/*------------------------------------------------------------------------
 * Basic Message Passing (Single Message per Process)
 *------------------------------------------------------------------------*/

/**
 * send - Send a message to a process
 * 
 * @param pid: Destination process ID
 * @param msg: Message to send
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * If the destination process already has a message pending,
 * this call fails. Use the mailbox API for buffered messaging.
 */
syscall send(pid32 pid, umsg32 msg) {
    intmask mask;
    struct procent *pptr;
    
    mask = disable();
    
    /* Validate PID */
    if (pid < 0 || pid >= NPROC) {
        restore(mask);
        msg_stats.failed++;
        return SYSERR;
    }
    
    pptr = &proctab[pid];
    
    /* Check process is active */
    if (pptr->prstate == PR_FREE) {
        restore(mask);
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Check if process already has a message */
    if (pptr->prhasmsg) {
        restore(mask);
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Store message */
    pptr->prmsg = msg;
    pptr->prhasmsg = true;
    
    /* If receiver is waiting, wake it up */
    if (pptr->prstate == PR_RECV) {
        ready(pid);
    }
    
    msg_stats.sent++;
    
    restore(mask);
    return OK;
}

/**
 * receive - Receive a message (blocking)
 * 
 * Returns: The received message
 * 
 * Blocks until a message is available.
 */
umsg32 receive(void) {
    intmask mask;
    struct procent *pptr;
    umsg32 msg;
    
    mask = disable();
    
    pptr = &proctab[currpid];
    
    /* Wait for message if none pending */
    while (!pptr->prhasmsg) {
        pptr->prstate = PR_RECV;
        resched();
    }
    
    /* Retrieve message */
    msg = pptr->prmsg;
    pptr->prhasmsg = false;
    
    msg_stats.received++;
    
    restore(mask);
    return msg;
}

/**
 * recvclr - Receive a message if one is waiting, clear otherwise
 * 
 * Returns: Message if available, OK (0) if no message
 * 
 * Non-blocking receive. Returns immediately.
 */
umsg32 recvclr(void) {
    intmask mask;
    struct procent *pptr;
    umsg32 msg;
    
    mask = disable();
    
    pptr = &proctab[currpid];
    
    if (pptr->prhasmsg) {
        msg = pptr->prmsg;
        pptr->prhasmsg = false;
        msg_stats.received++;
    } else {
        msg = OK;
    }
    
    restore(mask);
    return msg;
}

/**
 * recvtime - Receive a message with timeout
 * 
 * @param maxwait: Maximum wait time in milliseconds
 * 
 * Returns: Message if received, TIMEOUT if timed out, SYSERR on error
 * 
 * Waits up to maxwait milliseconds for a message.
 */
umsg32 recvtime(uint32_t maxwait) {
    intmask mask;
    struct procent *pptr;
    umsg32 msg;
    
    mask = disable();
    
    pptr = &proctab[currpid];
    
    /* Check for pending message */
    if (pptr->prhasmsg) {
        msg = pptr->prmsg;
        pptr->prhasmsg = false;
        msg_stats.received++;
        restore(mask);
        return msg;
    }
    
    if (maxwait == 0) {
        restore(mask);
        return TIMEOUT;
    }
    
    /* Convert to ticks and set up timed wait */
    /* Insert into sleep queue and wait for message or timeout */
    
    /* Simplified implementation - use sleep with periodic checks */
    uint32_t elapsed = 0;
    uint32_t interval = (maxwait < 10) ? maxwait : 10;
    
    while (elapsed < maxwait) {
        pptr->prstate = PR_RECV;
        
        /* Wait a short interval */
        restore(mask);
        sleepms(interval);
        mask = disable();
        
        /* Check for message */
        if (pptr->prhasmsg) {
            msg = pptr->prmsg;
            pptr->prhasmsg = false;
            msg_stats.received++;
            restore(mask);
            return msg;
        }
        
        elapsed += interval;
    }
    
    msg_stats.timeouts++;
    
    restore(mask);
    return TIMEOUT;
}

/*------------------------------------------------------------------------
 * Mailbox (Buffered Message) API
 *------------------------------------------------------------------------*/

/**
 * mailbox_init - Initialize mailbox subsystem
 * 
 * Should be called during system initialization.
 */
void mailbox_init(void) {
    int i;
    
    for (i = 0; i < NPROC; i++) {
        mailboxes[i].head = 0;
        mailboxes[i].tail = 0;
        mailboxes[i].count = 0;
        mailboxes[i].mutex = -1;
        mailboxes[i].items = -1;
        mailboxes[i].slots = -1;
        mailboxes[i].active = false;
    }
    
    msg_stats.sent = 0;
    msg_stats.received = 0;
    msg_stats.failed = 0;
    msg_stats.timeouts = 0;
}

/**
 * mailbox_create - Create a mailbox for a process
 * 
 * @param pid: Process ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall mailbox_create(pid32 pid) {
    intmask mask;
    msgbox_t *mbox;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    mbox = &mailboxes[pid];
    
    if (mbox->active) {
        restore(mask);
        return SYSERR;  /* Already exists */
    }
    
    /* Initialize mailbox */
    mbox->head = 0;
    mbox->tail = 0;
    mbox->count = 0;
    
    /* Create semaphores */
    mbox->mutex = semcreate(1);
    mbox->items = semcreate(0);
    mbox->slots = semcreate(MSG_BOX_SIZE);
    
    if (mbox->mutex == SYSERR || mbox->items == SYSERR || mbox->slots == SYSERR) {
        /* Cleanup on failure */
        if (mbox->mutex != SYSERR) semdelete(mbox->mutex);
        if (mbox->items != SYSERR) semdelete(mbox->items);
        if (mbox->slots != SYSERR) semdelete(mbox->slots);
        restore(mask);
        return SYSERR;
    }
    
    mbox->active = true;
    
    restore(mask);
    return OK;
}

/**
 * mailbox_delete - Delete a process's mailbox
 * 
 * @param pid: Process ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall mailbox_delete(pid32 pid) {
    intmask mask;
    msgbox_t *mbox;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    mbox = &mailboxes[pid];
    
    if (!mbox->active) {
        restore(mask);
        return SYSERR;
    }
    
    /* Delete semaphores */
    semdelete(mbox->mutex);
    semdelete(mbox->items);
    semdelete(mbox->slots);
    
    mbox->active = false;
    
    restore(mask);
    return OK;
}

/**
 * mailbox_send - Send a message to a process's mailbox (blocking)
 * 
 * @param pid: Destination process ID
 * @param msg: Message to send
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Blocks if mailbox is full.
 */
syscall mailbox_send(pid32 pid, umsg32 msg) {
    msgbox_t *mbox;
    
    if (pid < 0 || pid >= NPROC) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    mbox = &mailboxes[pid];
    
    if (!mbox->active) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Wait for available slot */
    if (wait(mbox->slots) == SYSERR) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Acquire mutex */
    wait(mbox->mutex);
    
    /* Add message to buffer */
    mbox->messages[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % MSG_BOX_SIZE;
    mbox->count++;
    
    /* Release mutex */
    signal(mbox->mutex);
    
    /* Signal item available */
    signal(mbox->items);
    
    msg_stats.sent++;
    
    return OK;
}

/**
 * mailbox_send_nb - Non-blocking send to mailbox
 * 
 * @param pid: Destination process ID
 * @param msg: Message to send
 * 
 * Returns: OK on success, SYSERR if mailbox full or error
 */
syscall mailbox_send_nb(pid32 pid, umsg32 msg) {
    msgbox_t *mbox;
    
    if (pid < 0 || pid >= NPROC) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    mbox = &mailboxes[pid];
    
    if (!mbox->active) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Try to get a slot without blocking */
    if (trywait(mbox->slots) == SYSERR) {
        msg_stats.failed++;
        return SYSERR;  /* Mailbox full */
    }
    
    /* Acquire mutex */
    wait(mbox->mutex);
    
    /* Add message */
    mbox->messages[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % MSG_BOX_SIZE;
    mbox->count++;
    
    /* Release mutex */
    signal(mbox->mutex);
    
    /* Signal item */
    signal(mbox->items);
    
    msg_stats.sent++;
    
    return OK;
}

/**
 * mailbox_recv - Receive from current process's mailbox (blocking)
 * 
 * Returns: Message, or SYSERR on error
 */
umsg32 mailbox_recv(void) {
    msgbox_t *mbox;
    umsg32 msg;
    
    mbox = &mailboxes[currpid];
    
    if (!mbox->active) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Wait for message */
    if (wait(mbox->items) == SYSERR) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Acquire mutex */
    wait(mbox->mutex);
    
    /* Get message */
    msg = mbox->messages[mbox->head];
    mbox->head = (mbox->head + 1) % MSG_BOX_SIZE;
    mbox->count--;
    
    /* Release mutex */
    signal(mbox->mutex);
    
    /* Signal slot available */
    signal(mbox->slots);
    
    msg_stats.received++;
    
    return msg;
}

/**
 * mailbox_recv_nb - Non-blocking receive from mailbox
 * 
 * Returns: Message if available, SYSERR if empty
 */
umsg32 mailbox_recv_nb(void) {
    msgbox_t *mbox;
    umsg32 msg;
    
    mbox = &mailboxes[currpid];
    
    if (!mbox->active) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Try to get item without blocking */
    if (trywait(mbox->items) == SYSERR) {
        return SYSERR;  /* No message */
    }
    
    /* Acquire mutex */
    wait(mbox->mutex);
    
    /* Get message */
    msg = mbox->messages[mbox->head];
    mbox->head = (mbox->head + 1) % MSG_BOX_SIZE;
    mbox->count--;
    
    /* Release mutex */
    signal(mbox->mutex);
    
    /* Signal slot */
    signal(mbox->slots);
    
    msg_stats.received++;
    
    return msg;
}

/**
 * mailbox_recv_timeout - Receive from mailbox with timeout
 * 
 * @param timeout: Timeout in milliseconds
 * 
 * Returns: Message if received, TIMEOUT if timed out, SYSERR on error
 */
umsg32 mailbox_recv_timeout(uint32_t timeout) {
    msgbox_t *mbox;
    umsg32 msg;
    syscall status;
    
    mbox = &mailboxes[currpid];
    
    if (!mbox->active) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Timed wait for message */
    status = timedwait(mbox->items, timeout);
    if (status == TIMEOUT) {
        msg_stats.timeouts++;
        return TIMEOUT;
    }
    if (status == SYSERR) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    /* Acquire mutex */
    wait(mbox->mutex);
    
    /* Get message */
    msg = mbox->messages[mbox->head];
    mbox->head = (mbox->head + 1) % MSG_BOX_SIZE;
    mbox->count--;
    
    /* Release mutex */
    signal(mbox->mutex);
    
    /* Signal slot */
    signal(mbox->slots);
    
    msg_stats.received++;
    
    return msg;
}

/**
 * mailbox_count - Get number of messages in mailbox
 * 
 * @param pid: Process ID
 * 
 * Returns: Message count, or -1 on error
 */
int32_t mailbox_count(pid32 pid) {
    msgbox_t *mbox;
    int32_t count;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return -1;
    }
    
    mbox = &mailboxes[pid];
    
    if (!mbox->active) {
        return -1;
    }
    
    mask = disable();
    count = mbox->count;
    restore(mask);
    
    return count;
}

/**
 * mailbox_isempty - Check if mailbox is empty
 * 
 * @param pid: Process ID
 * 
 * Returns: true if empty, false if has messages
 */
bool mailbox_isempty(pid32 pid) {
    return (mailbox_count(pid) == 0);
}

/**
 * mailbox_isfull - Check if mailbox is full
 * 
 * @param pid: Process ID
 * 
 * Returns: true if full, false otherwise
 */
bool mailbox_isfull(pid32 pid) {
    return (mailbox_count(pid) == MSG_BOX_SIZE);
}

/*------------------------------------------------------------------------
 * Message Ports (Named Channels)
 *------------------------------------------------------------------------*/

#define NPORTS          32          /* Maximum message ports */
#define PORT_MSG_SIZE   8           /* Messages per port */

/* Port state */
#define PORT_FREE       0
#define PORT_ALLOC      1

/**
 * Message port structure
 */
typedef struct msgport {
    uint8_t     state;              /* Port state */
    char        name[16];           /* Port name */
    pid32       owner;              /* Owning process */
    umsg32      messages[PORT_MSG_SIZE];
    uint32_t    head;
    uint32_t    tail;
    uint32_t    count;
    sid32       mutex;
    sid32       items;
    sid32       slots;
} msgport_t;

static msgport_t ports[NPORTS];

/**
 * port_init - Initialize port subsystem
 */
void port_init(void) {
    int i;
    
    for (i = 0; i < NPORTS; i++) {
        ports[i].state = PORT_FREE;
        ports[i].name[0] = '\0';
        ports[i].owner = -1;
    }
}

/**
 * port_create - Create a named message port
 * 
 * @param name: Port name (max 15 chars)
 * 
 * Returns: Port ID on success, SYSERR on error
 */
int32_t port_create(const char *name) {
    int i;
    intmask mask;
    
    if (name == NULL || name[0] == '\0') {
        return SYSERR;
    }
    
    mask = disable();
    
    /* Check for duplicate name */
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].state == PORT_ALLOC) {
            if (strcmp(ports[i].name, name) == 0) {
                restore(mask);
                return SYSERR;  /* Name exists */
            }
        }
    }
    
    /* Find free port */
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].state == PORT_FREE) {
            break;
        }
    }
    
    if (i >= NPORTS) {
        restore(mask);
        return SYSERR;  /* No free ports */
    }
    
    /* Initialize port */
    ports[i].state = PORT_ALLOC;
    strncpy(ports[i].name, name, 15);
    ports[i].name[15] = '\0';
    ports[i].owner = currpid;
    ports[i].head = 0;
    ports[i].tail = 0;
    ports[i].count = 0;
    
    ports[i].mutex = semcreate(1);
    ports[i].items = semcreate(0);
    ports[i].slots = semcreate(PORT_MSG_SIZE);
    
    if (ports[i].mutex == SYSERR || ports[i].items == SYSERR || 
        ports[i].slots == SYSERR) {
        if (ports[i].mutex != SYSERR) semdelete(ports[i].mutex);
        if (ports[i].items != SYSERR) semdelete(ports[i].items);
        if (ports[i].slots != SYSERR) semdelete(ports[i].slots);
        ports[i].state = PORT_FREE;
        restore(mask);
        return SYSERR;
    }
    
    restore(mask);
    return i;
}

/**
 * port_delete - Delete a message port
 * 
 * @param portid: Port ID
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall port_delete(int32_t portid) {
    intmask mask;
    
    if (portid < 0 || portid >= NPORTS) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (ports[portid].state == PORT_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    /* Only owner can delete */
    if (ports[portid].owner != currpid) {
        restore(mask);
        return SYSERR;
    }
    
    semdelete(ports[portid].mutex);
    semdelete(ports[portid].items);
    semdelete(ports[portid].slots);
    
    ports[portid].state = PORT_FREE;
    ports[portid].name[0] = '\0';
    
    restore(mask);
    return OK;
}

/**
 * port_lookup - Find a port by name
 * 
 * @param name: Port name to find
 * 
 * Returns: Port ID if found, SYSERR if not found
 */
int32_t port_lookup(const char *name) {
    int i;
    intmask mask;
    
    if (name == NULL || name[0] == '\0') {
        return SYSERR;
    }
    
    mask = disable();
    
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].state == PORT_ALLOC) {
            if (strcmp(ports[i].name, name) == 0) {
                restore(mask);
                return i;
            }
        }
    }
    
    restore(mask);
    return SYSERR;
}

/**
 * port_send - Send message to a port
 * 
 * @param portid: Port ID
 * @param msg: Message to send
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall port_send(int32_t portid, umsg32 msg) {
    if (portid < 0 || portid >= NPORTS || ports[portid].state == PORT_FREE) {
        return SYSERR;
    }
    
    wait(ports[portid].slots);
    wait(ports[portid].mutex);
    
    ports[portid].messages[ports[portid].tail] = msg;
    ports[portid].tail = (ports[portid].tail + 1) % PORT_MSG_SIZE;
    ports[portid].count++;
    
    signal(ports[portid].mutex);
    signal(ports[portid].items);
    
    return OK;
}

/**
 * port_recv - Receive message from a port
 * 
 * @param portid: Port ID
 * 
 * Returns: Message, or SYSERR on error
 */
umsg32 port_recv(int32_t portid) {
    umsg32 msg;
    
    if (portid < 0 || portid >= NPORTS || ports[portid].state == PORT_FREE) {
        return SYSERR;
    }
    
    wait(ports[portid].items);
    wait(ports[portid].mutex);
    
    msg = ports[portid].messages[ports[portid].head];
    ports[portid].head = (ports[portid].head + 1) % PORT_MSG_SIZE;
    ports[portid].count--;
    
    signal(ports[portid].mutex);
    signal(ports[portid].slots);
    
    return msg;
}

/*------------------------------------------------------------------------
 * Message Statistics
 *------------------------------------------------------------------------*/

/**
 * msg_info - Print message subsystem information
 */
void msg_info(void) {
    int i;
    int active_mailboxes = 0;
    int active_ports = 0;
    
    for (i = 0; i < NPROC; i++) {
        if (mailboxes[i].active) active_mailboxes++;
    }
    
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].state == PORT_ALLOC) active_ports++;
    }
    
    kprintf("\n===== Message System Information =====\n");
    kprintf("Statistics:\n");
    kprintf("  Messages sent:     %llu\n", msg_stats.sent);
    kprintf("  Messages received: %llu\n", msg_stats.received);
    kprintf("  Failed operations: %llu\n", msg_stats.failed);
    kprintf("  Timeouts:          %llu\n", msg_stats.timeouts);
    kprintf("\nMailboxes: %d active / %d max\n", active_mailboxes, NPROC);
    kprintf("Ports: %d active / %d max\n", active_ports, NPORTS);
    
    if (active_ports > 0) {
        kprintf("\nActive ports:\n");
        for (i = 0; i < NPORTS; i++) {
            if (ports[i].state == PORT_ALLOC) {
                kprintf("  [%2d] '%s' (owner=%d, msgs=%d)\n",
                        i, ports[i].name, ports[i].owner, ports[i].count);
            }
        }
    }
    
    kprintf("======================================\n\n");
}
