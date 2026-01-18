/* message.c - Inter-process message passing */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdbool.h>

#define MSG_BOX_SIZE        16
#define MSG_TIMEOUT_INF     0xFFFFFFFF

typedef struct msgbox {
    umsg32      messages[MSG_BOX_SIZE];
    uint32_t    head;
    uint32_t    tail;
    uint32_t    count;
    sid32       mutex;
    sid32       items;
    sid32       slots;
    bool        active;
} msgbox_t;

static msgbox_t mailboxes[NPROC];

static struct {
    uint64_t    sent;
    uint64_t    received;
    uint64_t    failed;
    uint64_t    timeouts;
} msg_stats;

/* Send a message to a process */
syscall send(pid32 pid, umsg32 msg) {
    intmask mask;
    struct procent *pptr;
    
    mask = disable();
    
    if (pid < 0 || pid >= NPROC) {
        restore(mask);
        msg_stats.failed++;
        return SYSERR;
    }
    
    pptr = &proctab[pid];
    
    if (pptr->prstate == PR_FREE) {
        restore(mask);
        msg_stats.failed++;
        return SYSERR;
    }
    
    if (pptr->prhasmsg) {
        restore(mask);
        msg_stats.failed++;
        return SYSERR;
    }
    
    pptr->prmsg = msg;
    pptr->prhasmsg = true;
    
    /* Wake receiver if waiting */
    if (pptr->prstate == PR_RECV) {
        ready(pid);
    }
    
    msg_stats.sent++;
    
    restore(mask);
    return OK;
}

/* Receive a message (blocks until available) */
umsg32 receive(void) {
    intmask mask;
    struct procent *pptr;
    umsg32 msg;
    
    mask = disable();
    
    pptr = &proctab[currpid];
    
    while (!pptr->prhasmsg) {
        pptr->prstate = PR_RECV;
        resched();
    }
    
    msg = pptr->prmsg;
    pptr->prhasmsg = false;
    
    msg_stats.received++;
    
    restore(mask);
    return msg;
}

/* Non-blocking receive - returns OK if no message */
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

/* Receive a message with timeout */
umsg32 recvtime(uint32_t maxwait) {
    intmask mask;
    struct procent *pptr;
    umsg32 msg;
    
    mask = disable();
    
    pptr = &proctab[currpid];
    
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

/* Initialize mailbox subsystem */
void mailbox_init(void) {
    int i;
    
    for (i = 0; i < NPROC; i++) {
        mailboxes[i].head = 0;
        mailboxes[i].tail = 0;
        mailboxes[i].count = 0;
        mailboxes[i].mutex = SYSERR;
        mailboxes[i].items = SYSERR;
        mailboxes[i].slots = SYSERR;
        mailboxes[i].active = false;
    }
    
    msg_stats.sent = 0;
    msg_stats.received = 0;
    msg_stats.failed = 0;
    msg_stats.timeouts = 0;
}

/* Create a mailbox for a process */
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
        return SYSERR;
    }
    
    mbox->head = 0;
    mbox->tail = 0;
    mbox->count = 0;
    
    mbox->mutex = semcreate(1);
    mbox->items = semcreate(0);
    mbox->slots = semcreate(MSG_BOX_SIZE);
    
    if (mbox->mutex == SYSERR || mbox->items == SYSERR || mbox->slots == SYSERR) {
        if (mbox->mutex != SYSERR) semdelete(mbox->mutex);
        if (mbox->items != SYSERR) semdelete(mbox->items);
        if (mbox->slots != SYSERR) semdelete(mbox->slots);
        mbox->mutex = SYSERR;
        mbox->items = SYSERR;
        mbox->slots = SYSERR;
        mbox->head = 0;
        mbox->tail = 0;
        mbox->count = 0;
        restore(mask);
        return SYSERR;
    }
    
    semdelete(mbox->mutex);
    semdelete(mbox->items);
    
    restore(mask);
    return OK;
}

/* Delete a process's mailbox */
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
    
    mbox->active = false;
    semdelete(mbox->items);
    semdelete(mbox->slots);
    mbox->mutex = SYSERR;
    mbox->items = SYSERR;
    mbox->slots = SYSERR;
    
    mbox->active = false;
    
    restore(mask);
    return OK;
}

/* Send a message to a mailbox (blocking) */
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
    
    if (wait(mbox->slots) == SYSERR) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    wait(mbox->mutex);
    
    mbox->messages[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % MSG_BOX_SIZE;
    mbox->count++;
    
    signal(mbox->mutex);
    signal(mbox->items);
    
    msg_stats.sent++;
    
    return OK;
}

/* Non-blocking send to mailbox */
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
    
    if (trywait(mbox->slots) == SYSERR) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    wait(mbox->mutex);
    
    mbox->messages[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % MSG_BOX_SIZE;
    mbox->count++;
    
    signal(mbox->mutex);
    signal(mbox->items);
    
    msg_stats.sent++;
    
    return OK;
}

/* Receive from mailbox (blocking) */
umsg32 mailbox_recv(void) {
    msgbox_t *mbox;
    umsg32 msg;
    
    mbox = &mailboxes[currpid];
    
    if (!mbox->active) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    if (wait(mbox->items) == SYSERR) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    wait(mbox->mutex);
    
    msg = mbox->messages[mbox->head];
    mbox->head = (mbox->head + 1) % MSG_BOX_SIZE;
    mbox->count--;
    
    signal(mbox->mutex);
    signal(mbox->slots);
    
    msg_stats.received++;
    
    return msg;
}

/* Non-blocking receive from mailbox */
umsg32 mailbox_recv_nb(void) {
    msgbox_t *mbox;
    umsg32 msg;
    
    mbox = &mailboxes[currpid];
    
    if (!mbox->active) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    if (trywait(mbox->items) == SYSERR) {
        return SYSERR;
    }
    
    wait(mbox->mutex);
    
    msg = mbox->messages[mbox->head];
    mbox->head = (mbox->head + 1) % MSG_BOX_SIZE;
    mbox->count--;
    
    signal(mbox->mutex);
    signal(mbox->slots);
    
    msg_stats.received++;
    
    return msg;
}

/* Receive from mailbox with timeout */
umsg32 mailbox_recv_timeout(uint32_t timeout) {
    msgbox_t *mbox;
    umsg32 msg;
    syscall status;
    
    mbox = &mailboxes[currpid];
    
    if (!mbox->active) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    status = timedwait(mbox->items, timeout);
    if (status == TIMEOUT) {
        msg_stats.timeouts++;
        return TIMEOUT;
    }
    if (status == SYSERR) {
        msg_stats.failed++;
        return SYSERR;
    }
    
    wait(mbox->mutex);
    
    msg = mbox->messages[mbox->head];
    mbox->head = (mbox->head + 1) % MSG_BOX_SIZE;
    mbox->count--;
    
    signal(mbox->mutex);
    
    signal(mbox->slots);
    
    msg_stats.received++;
    
    return msg;
}

/* Get number of messages in mailbox */
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

/* Check if mailbox is empty */
bool mailbox_isempty(pid32 pid) {
    return (mailbox_count(pid) == 0);
}

/* Check if mailbox is full */
bool mailbox_isfull(pid32 pid) {
    return (mailbox_count(pid) == MSG_BOX_SIZE);
}

#define NPORTS          32
#define PORT_MSG_SIZE   8

#define PORT_FREE       0
#define PORT_ALLOC      1

typedef struct msgport {
    uint8_t     state;
    char        name[16];
    pid32       owner;
    umsg32      messages[PORT_MSG_SIZE];
    uint32_t    head;
    uint32_t    tail;
    uint32_t    count;
    sid32       mutex;
    sid32       items;
    sid32       slots;
} msgport_t;

static msgport_t ports[NPORTS];

/* Initialize port subsystem */
void port_init(void) {
    int i;
    
    for (i = 0; i < NPORTS; i++) {
        ports[i].state = PORT_FREE;
        ports[i].name[0] = '\0';
        ports[i].owner = -1;
    }
}

/* Create a named message port */
int32_t port_create(const char *name) {
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
                return SYSERR;
            }
        }
    }
    
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].state == PORT_FREE) {
            break;
        }
    }
    
    if (i >= NPORTS) {
        restore(mask);
        return SYSERR;
    }
    
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

/* Delete a message port */
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

/* Find a port by name */
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

/* Send message to a port */
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

/* Receive message from a port */
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

/* Print message subsystem information */
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
