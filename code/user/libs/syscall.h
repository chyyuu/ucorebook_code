#ifndef __USER_LIBS_SYSCALL_H__
#define __USER_LIBS_SYSCALL_H__

#include <types.h>

int sys_exit(int error_code);
int sys_fork(void);
int sys_wait(int pid, int *store);
int sys_yield(void);
int sys_sleep(unsigned int time);
int sys_kill(int pid);
size_t sys_gettime(void);
int sys_getpid(void);
int sys_brk(uintptr_t *brk_store);
int sys_putc(int c);
int sys_pgdir(void);

#endif /* !__USER_LIBS_SYSCALL_H__ */

