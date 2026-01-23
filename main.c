/* main.c - Kernel main entry point */

#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/memory.h"
#include "../include/interrupts.h"
#include "../schedulers/scheduler.h"

#include <stdbool.h>
#include <string.h>


extern proc_t proctab[];
extern pid32 currpid;
extern void kernel_init(void);
extern void irq_init(void);
extern void init_exception_handlers(void);
extern void clkhandler(void);

/*------------------------------------------------------------------------
 * Boot Configuration
 *------------------------------------------------------------------------*/

/* Boot parameters (would be passed from bootloader) */
typedef struct boot_params {
    uint32_t    mem_lower;      /* Lower memory size (KB) */
    uint32_t    mem_upper;      /* Upper memory size (KB) */
    char        *cmdline;       /* Kernel command line */
    uint32_t    initrd_start;   /* Initial ramdisk start address */
    uint32_t    initrd_end;     /* Initial ramdisk end address */
} boot_params_t;

static boot_params_t boot_info;

/*------------------------------------------------------------------------
 * System Initialization Stages
 *------------------------------------------------------------------------*/

/**
 * early_init - Very early initialization
 * 
 * Called before any other initialization. Sets up minimal
 * hardware needed for further initialization.
 */
static void early_init(void) {
    /* 
     * In a real implementation:
     * - Set up early console for debugging
     * - Initialize CPU (enable caches, set modes)
     * - Clear BSS section
     * - Set up initial stack
     */
    
    /* Initialize boot info with defaults */
    boot_info.mem_lower = 640;          /* 640 KB conventional */
    boot_info.mem_upper = 15 * 1024;    /* 15 MB extended */
    boot_info.cmdline = NULL;
    boot_info.initrd_start = 0;
    boot_info.initrd_end = 0;
}

/**
 * arch_init - Architecture-specific initialization
 * 
 * Performs CPU and architecture-specific setup.
 */
static void arch_init(void) {
    /*
     * x86:
     * - Set up GDT (Global Descriptor Table)
     * - Set up IDT (Interrupt Descriptor Table)
     * - Initialize TSS (Task State Segment)
     * - Enable paging
     * 
     * ARM:
     * - Set up MMU page tables
     * - Configure exception vectors
     * - Set up domain access
     * 
     * RISC-V:
     * - Set up page tables
     * - Configure trap handlers
     * - Set up PMP (Physical Memory Protection)
     */
}

/**
 * mem_init - Initialize memory management
 * 
 * Sets up memory allocator, page tables, and memory map.
 */
static void mem_init(void) {
    /* Calculate total memory */
    uint32_t total_mem = (boot_info.mem_lower + boot_info.mem_upper) * 1024;
    
    (void)total_mem;
    
    /* Initialize heap allocator */
    init_memory();
    
    /* 
     * Initialize paging if supported:
     * init_paging();
     */
    
    /* 
     * Create kernel memory map:
     * - Mark kernel code/data as used
     * - Set up DMA zones
     * - Reserve BIOS/hardware regions
     */
}

/**
 * intr_init - Initialize interrupt subsystem
 * 
 * Sets up interrupt controllers and handlers.
 */
static void intr_init(void) {
    /* Initialize interrupt subsystem */
    irq_init();
    
    /* Set up exception handlers */
    init_exception_handlers();
    
    /*
     * Initialize platform interrupt controller:
     * - x86: PIC or APIC
     * - ARM: GIC
     * - RISC-V: PLIC/CLINT
     */
}

/**
 * clock_init - Initialize system clock
 * 
 * Sets up the system timer for preemptive scheduling.
 */
static void clock_init(void) {
    /*
     * x86:
     * - Program PIT (Programmable Interval Timer)
     * - Or use APIC timer or HPET
     * 
     * ARM:
     * - Configure ARM timer
     * - Or use platform-specific timer
     * 
     * Set up to generate interrupts at ~1000 Hz (1ms ticks)
     */
    
    /* Register clock interrupt handler */
    set_irq_handler(0, (void (*)(int))clkhandler);  /* IRQ 0 = timer */
    enable_irq(0);
}

/**
 * dev_init - Initialize device subsystem
 * 
 * Sets up device drivers and device table.
 */
static void dev_init(void) {
    /*
     * Initialize device table
     * Initialize console driver
     * Initialize disk drivers
     * Initialize network drivers
     * Initialize other devices
     */
}

/**
 * fs_init - Initialize file system
 * 
 * Mounts root file system and sets up VFS.
 */
static void fs_init(void) {
    /*
     * Initialize VFS layer
     * Mount root file system
     * Initialize special file systems (procfs, devfs)
     */
}

/**
 * net_init - Initialize networking subsystem
 * 
 * Sets up network stack and interfaces.
 */
static void net_init(void) {
    /*
     * Initialize protocol handlers (IP, TCP, UDP)
     * Initialize network interfaces
     * Start network daemon processes
     */
}

/*------------------------------------------------------------------------
 * System Process Creation
 *------------------------------------------------------------------------*/

/**
 * idle_process - Idle/null process function
 * 
 * This process runs when no other process is ready.
 * It should never exit.
 */
static void idle_process(void) {
    while (1) {
        /*
         * x86: hlt instruction (wait for interrupt)
         * ARM: wfi instruction (wait for interrupt)
         * 
         * This saves power by halting the CPU until
         * an interrupt occurs.
         */
        #if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("hlt");
        #elif defined(__arm__) || defined(__aarch64__)
        __asm__ volatile("wfi");
        #else
        __asm__ volatile("nop");
        #endif
    }
}

/* Init process (PID 1) */
static void init_process(void) {
    while (1) {
        sleep(1000);
    }
}

/* Shell process */
static void shell_process(void) {
    while (1) {
        sleep(1000);
    }
}

/* Create initial system processes */
static void create_system_processes(void) {
    pid32 init_pid;
    pid32 shell_pid;
    
    init_pid = create((void *)init_process, 4096, 80, "init", 0);
    if (init_pid != SYSERR) {
        resume(init_pid);
    }
    
    shell_pid = create((void *)shell_process, 8192, 50, "shell", 0);
    if (shell_pid != SYSERR) {
        resume(shell_pid);
    }
    
    /*
     * Additional system processes could be created here:
     * - Network daemon
     * - Log daemon
     * - Cron/scheduler daemon
     * - Device manager
     */
}

/*------------------------------------------------------------------------
 * Kernel Banner
 *------------------------------------------------------------------------*/

/**
 * print_banner - Print kernel startup banner
 */
static void print_banner(void) {
    /*
     * kprintf("\n");
     * kprintf("========================================\n");
     * kprintf("         Xinu Operating System         \n");
     * kprintf("========================================\n");
     * kprintf("Version: 1.0.0\n");
     * kprintf("Build: %s %s\n", __DATE__, __TIME__);
     * kprintf("Memory: %d KB lower, %d KB upper\n",
     *         boot_info.mem_lower, boot_info.mem_upper);
     * kprintf("========================================\n\n");
     */
}

/*------------------------------------------------------------------------
 * Main Entry Point
 *------------------------------------------------------------------------*/

/**
 * kernel_main - Main kernel entry point
 * 
 * This is the first C function called after the boot assembly code.
 * It performs all system initialization and starts the scheduler.
 * 
 * This function never returns.
 */
void kernel_main(void) {
    /*
     * Stage 1: Very early initialization
     * - Minimal hardware setup
     * - Clear BSS, set up stack
     */
    early_init();
    
    /*
     * Stage 2: Architecture-specific initialization
     * - CPU mode setup
     * - Descriptor tables (GDT/IDT on x86)
     */
    arch_init();
    
    /*
     * Stage 3: Memory initialization
     * - Physical memory manager
     * - Virtual memory / paging
     * - Heap allocator
     */
    mem_init();
    
    /*
     * Stage 4: Kernel initialization
     * - Process table
     * - Semaphore table
     * - System variables
     */
    kernel_init();
    
    /*
     * Stage 5: Interrupt initialization
     * - Interrupt controller
     * - Exception handlers
     * - Interrupt handlers
     */
    intr_init();
    
    /*
     * Stage 6: Clock initialization
     * - System timer
     * - Clock interrupt handler
     */
    clock_init();
    
    /*
     * Stage 7: Device initialization
     * - Console
     * - Disk
     * - Other hardware
     */
    dev_init();
    
    /*
     * Stage 8: File system initialization
     * - VFS layer
     * - Root mount
     */
    fs_init();
    
    /*
     * Stage 9: Network initialization
     * - Protocol stack
     * - Network interfaces
     */
    net_init();
    
    /*
     * Stage 10: Scheduler initialization
     */
    scheduler_init(SCHED_PRIORITY);
    
    /* Print startup banner */
    print_banner();
    
    /*
     * Stage 11: Create system processes
     * - Init (PID 1)
     * - Shell
     * - System daemons
     */
    create_system_processes();
    
    /*
     * Stage 12: Enable interrupts and start scheduling
     */
    enable();
    
    /*
     * Stage 13: Enter scheduling loop
     * 
     * The null process runs here when no other process is ready.
     * This loop should never terminate.
     */
    idle_process();
    
    /* Should never reach here */
    panic("kernel_main returned");
}

/**
 * nulluser - Null process entry point (alternative name)
 * 
 * Some Xinu versions use this name for the null process.
 */
void nulluser(void) {
    kernel_main();
}

/*------------------------------------------------------------------------
 * System Shutdown
 *------------------------------------------------------------------------*/

/**
 * shutdown - Perform system shutdown
 * 
 * @param reboot: If true, reboot after shutdown; otherwise halt
 * 
 * Cleanly shuts down the system:
 * 1. Signal all processes to terminate
 * 2. Sync file systems
 * 3. Stop device drivers
 * 4. Halt or reboot CPU
 */
void shutdown(bool reboot) {
    intmask mask;
    
    /* Disable interrupts */
    mask = disable();
    (void)mask;
    
    /*
     * kprintf("\n*** System shutting down ***\n");
     */
    
    /* Kill all user processes */
    /*
     * for (pid = NPROC - 1; pid > 0; pid--) {
     *     if (proctab[pid].pstate != PR_FREE) {
     *         kill(pid);
     *     }
     * }
     */
    
    /* Sync file systems */
    /* sync(); */
    
    /* Unmount file systems */
    /* umount_all(); */
    
    /* Stop device drivers */
    /* shutdown_devices(); */
    
    /* Stop network */
    /* shutdown_network(); */
    
    if (reboot) {
        /*
         * kprintf("Rebooting...\n");
         * 
         * x86:
         *   outb(0x64, 0xFE);  // Keyboard controller reset
         *   or use ACPI reset
         * 
         * ARM:
         *   Write to watchdog or reset controller
         */
    } else {
        /*
         * kprintf("System halted.\n");
         * 
         * x86:
         *   cli
         *   hlt
         *   or use ACPI power off (S5 state)
         */
    }
    
    /* Final halt loop */
    while (1) {
        #if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("cli; hlt");
        #else
        __asm__ volatile("wfi");
        #endif
    }
}

/**
 * halt - Halt the system immediately
 * 
 * Emergency halt - does not perform clean shutdown.
 */
void halt(void) {
    disable();
    
    while (1) {
        #if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("hlt");
        #else
        __asm__ volatile("wfi");
        #endif
    }
}

/**
 * reboot - Reboot the system
 */
void reboot(void) {
    shutdown(true);
}

/**
 * poweroff - Power off the system
 */
void poweroff(void) {
    shutdown(false);
}

/*------------------------------------------------------------------------
 * Kernel Version Information
 *------------------------------------------------------------------------*/

static const char kernel_version[] = "1.0.0";
static const char kernel_name[] = "Xinu";
static const char build_date[] = __DATE__;
static const char build_time[] = __TIME__;

/**
 * get_kernel_version - Get kernel version string
 * 
 * Returns: Pointer to version string
 */
const char* get_kernel_version(void) {
    return kernel_version;
}

/**
 * get_kernel_name - Get kernel name
 * 
 * Returns: Pointer to kernel name string
 */
const char* get_kernel_name(void) {
    return kernel_name;
}

/**
 * get_build_info - Get build information
 * 
 * @param date: Buffer for build date
 * @param time: Buffer for build time
 */
void get_build_info(char *date, char *time) {
    if (date) strcpy(date, build_date);
    if (time) strcpy(time, build_time);
}

