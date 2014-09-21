#ifndef __KERN_DRIVER_CONSOLE_H__
#define __KERN_DRIVER_CONSOLE_H__

void cons_init(void);
void cons_putc(int c);
int cons_getc(void);
void serial1_intr(void);
void kbd_intr(void);

// using in kgdb.c
int serial2_getc(void *buf);
void serial2_putc(int c);

#endif /* !__KERN_DRIVER_CONSOLE_H__ */

