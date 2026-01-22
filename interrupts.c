/* interrupts.c - Interrupt management */

#include "../include/interrupts.h"
#include "../include/kernel.h"

#include <string.h>
#include <stdbool.h>

#define MAX_INTERRUPTS      256
#define MAX_EXCEPTIONS      32
#define IPL_SOFT            1
#define IPL_BIO             2 
#define IPL_NET             3 
#define IPL_TTY             4 
#define IPL_CLOCK           5
#define IPL_HIGH            6 


static volatile intmask interrupt_state = 0;
static volatile int interrupt_depth = 0;


#define INT_STACK_SIZE  16
static intmask int_state_stack[INT_STACK_SIZE];
static int int_stack_ptr = 0;

typedef void (*int_handler_t)(int irq);

static int_handler_t interrupt_handlers[MAX_INTERRUPTS];

/* Interrupt enabled flags */
static bool interrupt_enabled[MAX_INTERRUPTS];

/* Interrupt counts for statistics */
static uint32_t interrupt_counts[MAX_INTERRUPTS];

/* Exception handler table */
static int_handler_t exception_handlers[MAX_EXCEPTIONS];

/* Exception names for debugging */
static const char *exception_names[MAX_EXCEPTIONS] = {
    "Division by Zero",         /* 0 */
    "Debug Exception",          /* 1 */
    "NMI Interrupt",            /* 2 */
    "Breakpoint",               /* 3 */
    "Overflow",                 /* 4 */
    "Bound Range Exceeded",     /* 5 */
    "Invalid Opcode",           /* 6 */
    "Device Not Available",     /* 7 */
    "Double Fault",             /* 8 */
    "Coprocessor Segment",      /* 9 */
    "Invalid TSS",              /* 10 */
    "Segment Not Present",      /* 11 */
    "Stack Fault",              /* 12 */
    "General Protection",       /* 13 */
    "Page Fault",               /* 14 */
    "Reserved",                 /* 15 */
    "x87 FPU Error",            /* 16 */
    "Alignment Check",          /* 17 */
    "Machine Check",            /* 18 */
    "SIMD Exception",           /* 19 */
    "Virtualization Exception", /* 20 */
    "Control Protection",       /* 21 */
    "Reserved", "Reserved", "Reserved", "Reserved",  /* 22-25 */
    "Reserved", "Reserved",     /* 26-27 */
    "Hypervisor Injection",     /* 28 */
    "VMM Communication",        /* 29 */
    "Security Exception",       /* 30 */
    "Reserved"                  /* 31 */
};

/* Interrupt Enable/Disable Functions */
intmask disable(void) {
    intmask old_state = interrupt_state;
    
    interrupt_state = 1;
    
    if (int_stack_ptr < INT_STACK_SIZE) {
        int_state_stack[int_stack_ptr++] = old_state;
    }
    
    return old_state;
}

/* Restore interrupt state */
void restore(intmask mask) {
    interrupt_state = mask;
    
    if (int_stack_ptr > 0) {
        int_stack_ptr--;
    }
}


void enable(void) {
    
    interrupt_state = 0;
    int_stack_ptr = 0;
}

bool interrupts_enabled(void) {
    return (interrupt_state == 0);
}

bool in_interrupt(void) {
    return (interrupt_depth > 0);
}

/* Initialize interrupt subsystem */
void irq_init(void) {
    int i;
    
    for (i = 0; i < MAX_INTERRUPTS; i++) {
        interrupt_handlers[i] = NULL;
        interrupt_enabled[i] = false;
        interrupt_counts[i] = 0;
    }
    
    for (i = 0; i < MAX_EXCEPTIONS; i++) {
        exception_handlers[i] = NULL;
    }
    
    interrupt_state = 1;
    interrupt_depth = 0;
    int_stack_ptr = 0;
}

/* Register an interrupt handler */
syscall set_irq_handler(int irq, void (*handler)(int)) {
    intmask mask;
    
    if (irq < 0 || irq >= MAX_INTERRUPTS) {
        return SYSERR;
    }
    
    mask = disable();
    
    interrupt_handlers[irq] = handler;
    
    restore(mask);
    return OK;
}

/* Remove an interrupt handler */
syscall clear_irq_handler(int irq) {
    intmask mask;
    
    if (irq < 0 || irq >= MAX_INTERRUPTS) {
        return SYSERR;
    }
    
    mask = disable();
    
    interrupt_handlers[irq] = NULL;
    interrupt_enabled[irq] = false;
    
    restore(mask);
    return OK;
}

/* Enable a specific interrupt */
syscall enable_irq(int irq) {
    intmask mask;
    
    if (irq < 0 || irq >= MAX_INTERRUPTS) {
        return SYSERR;
    }
    
    mask = disable();
    
    interrupt_enabled[irq] = true;
    restore(mask);
    return OK;
}

syscall disable_irq(int irq) {
    intmask mask;
    
    if (irq < 0 || irq >= MAX_INTERRUPTS) {
        return SYSERR;
    }
    
    mask = disable();
    
    interrupt_enabled[irq] = false;
    
    /* Mask the interrupt at the controller */
    
    restore(mask);
    return OK;
}

void irq_dispatch(int irq) {
    intmask mask;
    
    if (irq < 0 || irq >= MAX_INTERRUPTS) {
        return;
    }
    
    mask = disable();
    
    /* Enter interrupt context */
    interrupt_depth++;
    
    /* Update statistics */
    interrupt_counts[irq]++;
    
    /* Call registered handler if any */
    if (interrupt_handlers[irq] != NULL && interrupt_enabled[irq]) {
        interrupt_handlers[irq](irq);
    }
    
    interrupt_depth--;
    
    restore(mask);
}

typedef struct interrupt_frame {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
    uint32_t r8, r9, r10, r11, r12;
    uint32_t sp, lr, pc;
    uint32_t cpsr;
} interrupt_frame_t;

void irq_handler(interrupt_frame_t *frame) {
    int irq;
    
    irq = 0;
    (void)frame;
    
    irq_dispatch(irq);
}

syscall set_exception_handler(int exc, void (*handler)(int)) {
    if (exc < 0 || exc >= MAX_EXCEPTIONS) {
        return SYSERR;
    }
    
    exception_handlers[exc] = handler;
    return OK;
}

void exception_dispatch(int exc, interrupt_frame_t *frame) {
    if (exc < 0 || exc >= MAX_EXCEPTIONS) {
        panic("Invalid exception number");
        return;
    }
    
    /* Try registered handler first */
    if (exception_handlers[exc] != NULL) {
        exception_handlers[exc](exc);
        return;
    }
    
    (void)frame;
    (void)exception_names;
    
    /* Fatal exception - panic */
    panic("Unhandled exception");
}

static void divide_by_zero_handler(int exc) {
    (void)exc;
    panic("Division by zero");
}

/* page_fault_handler - Handle page fault exception */
static void page_fault_handler(int exc) {
    (void)exc;
    panic("Page fault");
}

/* general_protection_handler - Handle general protection fault */
static void general_protection_handler(int exc) {
    (void)exc;
    panic("General protection fault");
}

/* init_exception_handlers - Set up default exception handlers */
void init_exception_handlers(void) {
    set_exception_handler(0, divide_by_zero_handler);
    set_exception_handler(14, page_fault_handler);
    set_exception_handler(13, general_protection_handler);
}

/* oftware Interrupts / System Calls */

typedef syscall (*syscall_handler_t)(uint32_t, uint32_t, uint32_t, uint32_t);
static syscall_handler_t syscall_table[128];
static int num_syscalls = 0;

syscall register_syscall(int num, syscall (*handler)(uint32_t, uint32_t, 
                                                      uint32_t, uint32_t)) {
    if (num < 0 || num >= 128) {
        return SYSERR;
    }
    
    syscall_table[num] = handler;
    if (num >= num_syscalls) {
        num_syscalls = num + 1;
    }
    
    return OK;
}

syscall syscall_dispatch(uint32_t num, uint32_t arg1, uint32_t arg2,
                         uint32_t arg3, uint32_t arg4) {
    if (num >= 128 || syscall_table[num] == NULL) {
        return SYSERR;
    }
    
    return syscall_table[num](arg1, arg2, arg3, arg4);
}

uint32_t get_irq_count(int irq) {
    if (irq < 0 || irq >= MAX_INTERRUPTS) {
        return 0;
    }
    return interrupt_counts[irq];
}

uint32_t get_total_irq_count(void) {
    int i;
    uint32_t total = 0;
    
    for (i = 0; i < MAX_INTERRUPTS; i++) {
        total += interrupt_counts[i];
    }
    
    return total;
}

void clear_irq_counts(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    for (i = 0; i < MAX_INTERRUPTS; i++) {
        interrupt_counts[i] = 0;
    }
    
    restore(mask);
}

/* Interrupt Controller Interface */

void pic_init(void) {
    /*
     * ICW1: Initialize + ICW4 needed
     * outb(PIC1_CMD, 0x11);
     * outb(PIC2_CMD, 0x11);
     * 
     * ICW2: Vector offset
     * outb(PIC1_DATA, 0x20);  // IRQ 0-7 -> INT 0x20-0x27
     * outb(PIC2_DATA, 0x28);  // IRQ 8-15 -> INT 0x28-0x2F
     * 
     * ICW3: Cascade identity
     * outb(PIC1_DATA, 0x04);  // Slave on IRQ2
     * outb(PIC2_DATA, 0x02);  // Slave ID = 2
     * 
     * ICW4: 8086 mode
     * outb(PIC1_DATA, 0x01);
     * outb(PIC2_DATA, 0x01);
     * 
     * Mask all interrupts initially
     * outb(PIC1_DATA, 0xFF);
     * outb(PIC2_DATA, 0xFF);
     */
}


void gic_init(void) {
    /*
     * GICD_CTLR = 1;
     * 
     * GICC_CTLR = 1;
     * 
     * GICC_PMR = 0xFF;
     */
}

void send_eoi(int irq) {
    /*
     * x86 PIC:
     *   if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
     *   outb(PIC1_CMD, PIC_EOI);
     * 
     * x86 APIC:
     *   *(volatile uint32_t*)APIC_EOI = 0;
     * 
     * ARM GIC:
     *   GICC_EOIR = irq;
     */
    (void)irq;
}

typedef volatile int spinlock_t;

intmask spin_lock_irqsave(spinlock_t *lock) {
    intmask mask = disable();
    
    while (*lock != 0) {
        /* Busy-wait */
    }
    *lock = 1;
    
    return mask;
}

void spin_unlock_irqrestore(spinlock_t *lock, intmask mask) {
    *lock = 0;
    restore(mask);
}

