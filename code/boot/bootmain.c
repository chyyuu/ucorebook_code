#include <types.h>
#include <x86.h>

#define COM1            0x3F8
#define CRTPORT         0x3D4
#define LPTPORT         0x378
#define COM_TX          0           // Out: Transmit buffer (DLAB=0)
#define COM_LSR         5           // In:  Line Status Register
#define COM_LSR_TXRDY   20          // Transmit buffer avail

static uint16_t *crt = (uint16_t *) 0xB8000;        // CGA memory

/* stupid I/O delay routine necessitated by historical PC design flaws */
static void
delay(void) {
    inb(0x84);
    inb(0x84);
    inb(0x84);
    inb(0x84);
}

/* lpt_putc - copy console output to parallel port */
static void
lpt_putc(int c) {
    int i;
    for (i = 0; !(inb(LPTPORT + 1) & 0x80) && i < 12800; i ++) {
        delay();
    }
    outb(LPTPORT + 0, c);
    outb(LPTPORT + 2, 0x08 | 0x04 | 0x01);
    outb(LPTPORT + 2, 0x08);
}

/* cga_putc - print character to console */
static void
cga_putc(int c) {
    int pos;

    // cursor position: col + 80*row.
    outb(CRTPORT, 14);
    pos = inb(CRTPORT + 1) << 8;
    outb(CRTPORT, 15);
    pos |= inb(CRTPORT + 1);

    if (c == '\n') {
        pos += 80 - pos % 80;
    }
    else {
        crt[pos ++] = (c & 0xff) | 0x0700;
    }

    outb(CRTPORT, 14);
    outb(CRTPORT + 1, pos >> 8);
    outb(CRTPORT, 15);
    outb(CRTPORT + 1, pos);
}

/* serial_putc - copy console output to serial port */
static void
serial_putc(int c) {
    int i;
    for (i = 0; !(inb(COM1 + COM_LSR) & COM_LSR_TXRDY) && i < 12800; i ++) {
        delay();
    }
    outb(COM1 + COM_TX, c);
}

/* cons_putc - print a single character to console*/
static void
cons_putc(int c) {
    lpt_putc(c);
    cga_putc(c);
    serial_putc(c);
}

/* cons_puts - print a string to console */
static void
cons_puts(const char *str) {
    while (*str != '\0') {
        cons_putc(*str ++);
    }
}

/* bootmain - the entry of bootloader */
void
bootmain(void) {
    cons_puts("This is a bootloader: Hello world!!");

    /* do nothing */
    while (1);
}

