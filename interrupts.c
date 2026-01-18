/*
 * interrupts.c - Xinu Interrupt Management Implementation
 * 
 * This file implements interrupt handling including:
 * - Interrupt enable/disable
 * - Interrupt vector table management
 * - Interrupt service routine registration
 * - Exception handling
 */

#include "../include/interrupts.h"
#include "../include/kernel.h"

#include <string.h>
#include <stdbool.h>

/*------------------------------------------------------------------------
 * Interrupt Constants
 *------------------------------------------------------------------------*/

#define MAX_INTERRUPTS      256     /* Maximum interrupt vectors */
#define MAX_EXCEPTIONS      32      /* Maximum exception vectors */

/* Interrupt priority levels */
#define IPL_NONE            0       /* All interrupts enabled */
#define IPL_SOFT            1       /* Software interrupt level */
#define IPL_BIO             2       /* Block I/O level */
#define IPL_NET             3       /* Network level */
#define IPL_TTY             4       /* Terminal level */
#define IPL_CLOCK           5       /* Clock level */
#define IPL_HIGH            6       /* Highest level */

/*------------------------------------------------------------------------
 * Interrupt State Variables
 *------------------------------------------------------------------------*/

/* Current interrupt enable/disable state */
static volatile intmask interrupt_state = 0;

/* Interrupt nesting level (0 = not in interrupt context) */
static volatile int interrupt_depth = 0;

/* Previous interrupt state stack for nested interrupts */
#define INT_STACK_SIZE  16
static intmask int_state_stack[INT_STACK_SIZE];
static int int_stack_ptr = 0;

/* Interrupt handler function type */
typedef void (*int_handler_t)(int irq);

/* Interrupt handler table */
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

/*------------------------------------------------------------------------
 * Interrupt Enable/Disable Functions
 *------------------------------------------------------------------------*/

/**
 * disable - Disable interrupts and return previous state
 * 
 * Returns: Previous interrupt mask that should be passed to restore()
 * 
 * This function disables all maskable interrupts and returns the
 * previous interrupt state. Used to protect critical sections.
 * 
 * Usage:
 *   intmask mask = disable();
 *   // Critical section code
 *   restore(mask);
 */
intmask disable(void) {
    intmask old_state = interrupt_state;
    
    /*
     * In a real implementation, this would execute a CPU instruction
     * to disable interrupts:
     * 
     * x86:
     *   pushf           ; Push flags
     *   cli             ; Clear interrupt flag
     *   pop eax         ; Get old flags
     * 
     * ARM:
     *   mrs r0, cpsr    ; Read current program status
     *   cpsid i         ; Disable IRQ
     * 
     * RISC-V:
     *   csrrci a0, mstatus, MSTATUS_MIE
     */
    
    interrupt_state = 1;  /* Interrupts disabled */
    
    /* Save state for nested disable calls */
    if (int_stack_ptr < INT_STACK_SIZE) {
        int_state_stack[int_stack_ptr++] = old_state;
    }
    
    return old_state;
}

/**
 * restore - Restore interrupt state
 * 
 * @param mask: Interrupt mask from previous disable() call
 * 
 * Restores the interrupt state to what it was before disable() was called.
 */
void restore(intmask mask) {
    /*
     * In a real implementation:
     * 
     * x86:
     *   push eax        ; Push saved flags
     *   popf            ; Restore flags (including IF)
     * 
     * ARM:
     *   msr cpsr_c, r0  ; Write to control field of CPSR
     * 
     * RISC-V:
     *   csrw mstatus, a0
     */
    
    interrupt_state = mask;
    
    /* Pop state stack */
    if (int_stack_ptr > 0) {
        int_stack_ptr--;
    }
}

/**
 * enable - Enable all interrupts
 * 
 * Unconditionally enables interrupts. Use restore() if you need
 * to restore a previous state.
 */
void enable(void) {
    /*
     * x86: sti
     * ARM: cpsie i
     * RISC-V: csrsi mstatus, MSTATUS_MIE
     */
    
    interrupt_state = 0;
    int_stack_ptr = 0;
}

/**
 * interrupts_enabled - Check if interrupts are enabled
 * 
 * Returns: true if interrupts are currently enabled
 */
bool interrupts_enabled(void) {
    return (interrupt_state == 0);
}

/**
 * in_interrupt - Check if executing in interrupt context
 * 
 * Returns: true if currently handling an interrupt
 */
bool in_interrupt(void) {
    return (interrupt_depth > 0);
}

/*------------------------------------------------------------------------
 * Interrupt Handler Management
 *------------------------------------------------------------------------*/

/**
 * irq_init - Initialize interrupt subsystem
 * 
 * Sets up interrupt tables and default handlers.
 */
void irq_init(void) {
    int i;
    
    /* Clear all interrupt handlers */
    for (i = 0; i < MAX_INTERRUPTS; i++) {
        interrupt_handlers[i] = NULL;
        interrupt_enabled[i] = false;
        interrupt_counts[i] = 0;
    }
    
    /* Clear all exception handlers */
    for (i = 0; i < MAX_EXCEPTIONS; i++) {
        exception_handlers[i] = NULL;
    }
    
    /* Initialize hardware interrupt controller */
    /* In real implementation:
     * - Set up PIC/APIC (x86)
     * - Configure GIC (ARM)
     * - Set up PLIC/CLINT (RISC-V)
     */
    
    /* Start with interrupts disabled */
    interrupt_state = 1;
    interrupt_depth = 0;
    int_stack_ptr = 0;
}

/**
 * set_irq_handler - Register an interrupt handler
 * 
 * @param irq: Interrupt number (0-255)
 * @param handler: Handler function to call when interrupt occurs
 * 
 * Returns: OK on success, SYSERR on error
 */
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

/**
 * clear_irq_handler - Remove an interrupt handler
 * 
 * @param irq: Interrupt number
 * 
 * Returns: OK on success, SYSERR on error
 */
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

/**
 * enable_irq - Enable a specific interrupt
 * 
 * @param irq: Interrupt number to enable
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall enable_irq(int irq) {
    intmask mask;
    
    if (irq < 0 || irq >= MAX_INTERRUPTS) {
        return SYSERR;
    }
    
    mask = disable();
    
    interrupt_enabled[irq] = true;
    
    /*
     * In real implementation, unmask the interrupt at the controller:
     * - PIC: outb(PIC_DATA, inb(PIC_DATA) & ~(1 << irq))
     * - APIC: write to IOAPIC redirection entry
     * - GIC: set bit in GICD_ISENABLER
     */
    
    restore(mask);
    return OK;
}

/**
 * disable_irq - Disable a specific interrupt
 * 
 * @param irq: Interrupt number to disable
 * 
 * Returns: OK on success, SYSERR on error
 */
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

/*------------------------------------------------------------------------
 * Interrupt Dispatching
 *------------------------------------------------------------------------*/

/**
 * irq_dispatch - Main interrupt dispatcher
 * 
 * @param irq: Interrupt number that occurred
 * 
 * Called by low-level interrupt stub. Dispatches to registered handler.
 */
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
    
    /* Send End-Of-Interrupt to controller */
    /*
     * x86 PIC: outb(PIC1_CMD, PIC_EOI)
     * x86 APIC: write to EOI register
     * ARM GIC: write to GICC_EOIR
     */
    
    /* Leave interrupt context */
    interrupt_depth--;
    
    restore(mask);
}

/**
 * irq_handler - Generic interrupt handler entry point
 * 
 * @param frame: Pointer to saved register frame on stack
 * 
 * This would be called from assembly interrupt stub.
 */
typedef struct interrupt_frame {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
    uint32_t r8, r9, r10, r11, r12;
    uint32_t sp, lr, pc;
    uint32_t cpsr;
} interrupt_frame_t;

void irq_handler(interrupt_frame_t *frame) {
    int irq;
    
    /*
     * In real implementation:
     * - Read IRQ number from interrupt controller
     * - x86: Read from PIC or APIC
     * - ARM: Read from GIC
     */
    
    irq = 0;  /* Placeholder - would read from hardware */
    (void)frame;
    
    irq_dispatch(irq);
}

/*------------------------------------------------------------------------
 * Exception Handling
 *------------------------------------------------------------------------*/

/**
 * set_exception_handler - Register an exception handler
 * 
 * @param exc: Exception number (0-31)
 * @param handler: Handler function
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall set_exception_handler(int exc, void (*handler)(int)) {
    if (exc < 0 || exc >= MAX_EXCEPTIONS) {
        return SYSERR;
    }
    
    exception_handlers[exc] = handler;
    return OK;
}

/**
 * exception_dispatch - Exception dispatcher
 * 
 * @param exc: Exception number
 * @param frame: Saved register frame
 * 
 * Called when a CPU exception occurs.
 */
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
    
    /* Default exception handling */
    /*
     * kprintf("\n*** EXCEPTION: %s ***\n", exception_names[exc]);
     * kprintf("Exception %d at PC=0x%08x\n", exc, frame->pc);
     * 
     * switch (exc) {
     *     case 14:  // Page fault (x86)
     *         kprintf("Fault address: 0x%08x\n", get_cr2());
     *         break;
     * }
     */
    
    (void)frame;
    (void)exception_names;
    
    /* Fatal exception - panic */
    panic("Unhandled exception");
}

/**
 * divide_by_zero_handler - Handle division by zero exception
 */
static void divide_by_zero_handler(int exc) {
    (void)exc;
    panic("Division by zero");
}

/**
 * page_fault_handler - Handle page fault exception
 */
static void page_fault_handler(int exc) {
    (void)exc;
    /*
     * In a full implementation:
     * 1. Get faulting address from CR2 (x86) or FAR (ARM)
     * 2. Check if valid address (stack growth, COW, etc.)
     * 3. Allocate page if valid access
     * 4. Kill process if invalid access
     */
    panic("Page fault");
}

/**
 * general_protection_handler - Handle general protection fault
 */
static void general_protection_handler(int exc) {
    (void)exc;
    panic("General protection fault");
}

/**
 * init_exception_handlers - Set up default exception handlers
 */
void init_exception_handlers(void) {
    /* Division by zero */
    set_exception_handler(0, divide_by_zero_handler);
    
    /* Page fault (exception 14 on x86) */
    set_exception_handler(14, page_fault_handler);
    
    /* General protection fault (exception 13 on x86) */
    set_exception_handler(13, general_protection_handler);
}

/*------------------------------------------------------------------------
 * Software Interrupts / System Calls
 *------------------------------------------------------------------------*/

/**
 * Software interrupt handler table
 */
typedef syscall (*syscall_handler_t)(uint32_t, uint32_t, uint32_t, uint32_t);
static syscall_handler_t syscall_table[128];
static int num_syscalls = 0;

/**
 * register_syscall - Register a system call handler
 * 
 * @param num: System call number
 * @param handler: Handler function
 * 
 * Returns: OK on success, SYSERR on error
 */
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

/**
 * syscall_dispatch - System call dispatcher
 * 
 * @param num: System call number
 * @param arg1-arg4: System call arguments
 * 
 * Returns: Result from system call handler
 */
syscall syscall_dispatch(uint32_t num, uint32_t arg1, uint32_t arg2,
                         uint32_t arg3, uint32_t arg4) {
    if (num >= 128 || syscall_table[num] == NULL) {
        return SYSERR;
    }
    
    return syscall_table[num](arg1, arg2, arg3, arg4);
}

/*------------------------------------------------------------------------
 * Interrupt Statistics
 *------------------------------------------------------------------------*/

/**
 * get_irq_count - Get count of interrupts for a specific IRQ
 * 
 * @param irq: Interrupt number
 * 
 * Returns: Number of times this interrupt has occurred
 */
uint32_t get_irq_count(int irq) {
    if (irq < 0 || irq >= MAX_INTERRUPTS) {
        return 0;
    }
    return interrupt_counts[irq];
}

/**
 * get_total_irq_count - Get total interrupt count
 * 
 * Returns: Total number of interrupts handled
 */
uint32_t get_total_irq_count(void) {
    int i;
    uint32_t total = 0;
    
    for (i = 0; i < MAX_INTERRUPTS; i++) {
        total += interrupt_counts[i];
    }
    
    return total;
}

/**
 * clear_irq_counts - Reset all interrupt counters
 */
void clear_irq_counts(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    for (i = 0; i < MAX_INTERRUPTS; i++) {
        interrupt_counts[i] = 0;
    }
    
    restore(mask);
}

/*------------------------------------------------------------------------
 * Interrupt Controller Interface
 *------------------------------------------------------------------------*/

/**
 * pic_init - Initialize 8259 PIC (x86)
 * 
 * Sets up the Programmable Interrupt Controller for x86 systems.
 */
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

/**
 * gic_init - Initialize GIC (ARM)
 * 
 * Sets up the Generic Interrupt Controller for ARM systems.
 */
void gic_init(void) {
    /*
     * Enable distributor
     * GICD_CTLR = 1;
     * 
     * Enable CPU interface
     * GICC_CTLR = 1;
     * 
     * Set priority mask to allow all priorities
     * GICC_PMR = 0xFF;
     */
}

/**
 * send_eoi - Send End-Of-Interrupt signal
 * 
 * @param irq: Interrupt number
 */
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

/*------------------------------------------------------------------------
 * Critical Section Helpers
 *------------------------------------------------------------------------*/

/**
 * spin_lock_irqsave - Acquire spinlock and save interrupt state
 * 
 * @param lock: Pointer to lock
 * 
 * Returns: Saved interrupt state
 */
typedef volatile int spinlock_t;

intmask spin_lock_irqsave(spinlock_t *lock) {
    intmask mask = disable();
    
    /* Acquire spinlock (simplified - real would use atomic ops) */
    while (*lock != 0) {
        /* Spin */
    }
    *lock = 1;
    
    return mask;
}

/**
 * spin_unlock_irqrestore - Release spinlock and restore interrupts
 * 
 * @param lock: Pointer to lock
 * @param mask: Saved interrupt state from spin_lock_irqsave
 */
void spin_unlock_irqrestore(spinlock_t *lock, intmask mask) {
    *lock = 0;
    restore(mask);
}

