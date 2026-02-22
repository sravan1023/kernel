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

/* Boot parameters (would be passed from bootloader) */
typedef struct boot_params {
    uint32_t    mem_lower;      /* Lower memory size (KB) */
    uint32_t    mem_upper;      /* Upper memory size (KB) */
    char        *cmdline;       /* Kernel command line */
    uint32_t    initrd_start;   /* Initial ramdisk start address */
    uint32_t    initrd_end;     /* Initial ramdisk end address */
} boot_params_t;

static boot_params_t boot_info;

static void early_init(void) {
    /* Initialize boot info with defaults */
    boot_info.mem_lower = 640;          /* 640 KB conventional */
    boot_info.mem_upper = 15 * 1024;    /* 15 MB extended */
    boot_info.cmdline = NULL;
    boot_info.initrd_start = 0;
    boot_info.initrd_end = 0;
}

static void arch_init(void) {
}

static void mem_init(void) {
    uint32_t total_mem = (boot_info.mem_lower + boot_info.mem_upper) * 1024;
    (void)total_mem;
    init_memory();
}

static void intr_init(void) {
    irq_init();
    init_exception_handlers();
}

static void clock_init(void) {
    set_irq_handler(0, (void (*)(int))clkhandler);  /* IRQ 0 = timer */
    enable_irq(0);
}

static void dev_init(void) {
}

static void fs_init(void) {
}

static void net_init(void) {
}

static void idle_process(void) {
    while (1) {
        #if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("hlt");
        #elif defined(__arm__) || defined(__aarch64__)
        __asm__ volatile("wfi");
        #else
        __asm__ volatile("nop");
        #endif
    }
}

static void init_process(void) {
    while (1) {
        sleep(1000);
    }
}

static void shell_process(void) {
    while (1) {
        sleep(500);
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
}

static void print_banner(void) {
}

void kernel_main(void) {
    early_init();
    arch_init();
    mem_init();
    kernel_init();
    intr_init();
    clock_init();
    dev_init();
    fs_init();
    net_init();
    scheduler_init(SCHED_PRIORITY);
    print_banner();
    create_system_processes();
    enable();
    idle_process();

    /* Should never reach here */
    panic("kernel_main returned");
}

void nulluser(void) {
    kernel_main();
}

void shutdown(bool reboot) {
    intmask mask;
    
    /* Disable interrupts */
    mask = disable();
    (void)mask;

    if (reboot) {
    } else {
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

void reboot(void) {
    shutdown(true);
}

void poweroff(void) {
    shutdown(false);
}

static const char kernel_version[] = "1.0.0";
static const char kernel_name[] = "Xinu";
static const char build_date[] = __DATE__;
static const char build_time[] = __TIME__;

const char* get_kernel_version(void) {
    return kernel_version;
}

const char* get_kernel_name(void) {
    return kernel_name;
}

void get_build_info(char *date, char *time) {
    if (date) strcpy(date, build_date);
    if (time) strcpy(time, build_time);
}

