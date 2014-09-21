#include <types.h>
#include <stdio.h>
#include <string.h>
#include <console.h>
#include <kdebug.h>
#include <picirq.h>
#include <trap.h>
#include <clock.h>
#include <intr.h>
#include <pmm.h>

int kern_init(void) __attribute__((noreturn));

static int watch_value = 0;

#define MACRO_ADC(x)        (x ++)
#define MACRO_ADC2(x)       MACRO_ADC(x); MACRO_ADC(x)
#define MACRO_ADC4(x)       MACRO_ADC2(x); MACRO_ADC2(x)
#define MACRO_ADC8(x)       MACRO_ADC4(x); MACRO_ADC4(x)
#define MACRO_ADC16(x)      MACRO_ADC8(x); MACRO_ADC8(x)
#define MACRO_ADC32(x)      MACRO_ADC16(x); MACRO_ADC16(x)
#define MACRO_ADC64(x)      MACRO_ADC32(x); MACRO_ADC32(x)
#define MACRO_ADC128(x)     MACRO_ADC64(x); MACRO_ADC64(x)
#define MACRO_ADC256(x)     MACRO_ADC128(x); MACRO_ADC128(x)

int
kern_init(void) {
    extern char edata[], end[];
    memset(edata, 0, end - edata);

    cons_init();                // init the console

    const char *message = "(THU.CST) os is loading ...";
    cprintf("%s\n\n", message);

    print_kerninfo();

    debug_init();               // init debug registers
    pmm_init();                 // init physical memory management

    pic_init();                 // init interrupt controller
    idt_init();                 // init interrupt descriptor table

    clock_init();               // init clock interrupt
    intr_enable();              // enable irq interrupt

    int num = 0;
    while (1) {
        MACRO_ADC256(num);
        watch_value ++;
    }
}

