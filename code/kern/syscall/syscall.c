#include <proc.h>
#include <syscall.h>
#include <trap.h>
#include <stdio.h>
#include <pmm.h>
#include <clock.h>
#include <error.h>
#include <assert.h>
#include <sem.h>

static uint32_t
sys_exit(uint32_t arg[]) {
    int error_code = (int)arg[0];
    return do_exit(error_code);
}

static uint32_t
sys_fork(uint32_t arg[]) {
    struct trapframe *tf = current->tf;
    uintptr_t stack = tf->tf_esp;
    return do_fork(0, stack, tf);
}

static uint32_t
sys_wait(uint32_t arg[]) {
    int pid = (int)arg[0];
    int *store = (int *)arg[1];
    return do_wait(pid, store);
}

static uint32_t
sys_exec(uint32_t arg[]) {
    const char *name = (const char *)arg[0];
    size_t len = (size_t)arg[1];
    unsigned char *binary = (unsigned char *)arg[2];
    size_t size = (size_t)arg[3];
    return do_execve(name, len, binary, size);
}

static uint32_t
sys_clone(uint32_t arg[]) {
    struct trapframe *tf = current->tf;
    uint32_t clone_flags = (uint32_t)arg[0];
    uintptr_t stack = (uintptr_t)arg[1];
    if (stack == 0) {
        stack = tf->tf_esp;
    }
    return do_fork(clone_flags, stack, tf);
}

static uint32_t
sys_exit_thread(uint32_t arg[]) {
    int error_code = (int)arg[0];
    return do_exit_thread(error_code);
}

static uint32_t
sys_yield(uint32_t arg[]) {
    return do_yield();
}

static uint32_t
sys_sleep(uint32_t arg[]) {
    unsigned int time = (unsigned int)arg[0];
    return do_sleep(time);
}

static uint32_t
sys_kill(uint32_t arg[]) {
    int pid = (int)arg[0];
    return do_kill(pid, -E_KILLED);
}

static uint32_t
sys_gettime(uint32_t arg[]) {
    return (int)ticks;
}

static uint32_t
sys_getpid(uint32_t arg[]) {
    return current->pid;
}

static uint32_t
sys_brk(uint32_t arg[]) {
    uintptr_t *brk_store = (uintptr_t *)arg[0];
    return do_brk(brk_store);
}

static uint32_t
sys_mmap(uint32_t arg[]) {
    uintptr_t *addr_store = (uintptr_t *)arg[0];
    size_t len = (size_t)arg[1];
    uint32_t mmap_flags = (uint32_t)arg[2];
    return do_mmap(addr_store, len, mmap_flags);
}

static uint32_t
sys_munmap(uint32_t arg[]) {
    uintptr_t addr = (uintptr_t)arg[0];
    size_t len = (size_t)arg[1];
    return do_munmap(addr, len);
}

static uint32_t
sys_shmem(uint32_t arg[]) {
    uintptr_t *addr_store = (uintptr_t *)arg[0];
    size_t len = (size_t)arg[1];
    uint32_t mmap_flags = (uint32_t)arg[2];
    return do_shmem(addr_store, len, mmap_flags);
}

static uint32_t
sys_putc(uint32_t arg[]) {
    int c = (int)arg[0];
    cputchar(c);
    return 0;
}

static uint32_t
sys_pgdir(uint32_t arg[]) {
    print_pgdir();
    return 0;
}

static uint32_t
sys_sem_init(uint32_t arg[]) {
    int value = (int)arg[0];
    return ipc_sem_init(value);
}

static uint32_t
sys_sem_post(uint32_t arg[]) {
    sem_t sem_id = (sem_t)arg[0];
    return ipc_sem_post(sem_id);
}

static uint32_t
sys_sem_wait(uint32_t arg[]) {
    sem_t sem_id = (sem_t)arg[0];
    unsigned int timeout = (unsigned int)arg[1];
    return ipc_sem_wait(sem_id, timeout);
}

static uint32_t
sys_sem_free(uint32_t arg[]) {
    sem_t sem_id = (sem_t)arg[0];
    return ipc_sem_free(sem_id);
}

static uint32_t
sys_sem_get_value(uint32_t arg[]) {
    sem_t sem_id = (sem_t)arg[0];
    int *value_store = (int *)arg[1];
    return ipc_sem_get_value(sem_id, value_store);
}

static uint32_t (*syscalls[])(uint32_t arg[]) = {
    [SYS_exit]              sys_exit,
    [SYS_fork]              sys_fork,
    [SYS_wait]              sys_wait,
    [SYS_exec]              sys_exec,
    [SYS_clone]             sys_clone,
    [SYS_exit_thread]       sys_exit_thread,
    [SYS_yield]             sys_yield,
    [SYS_kill]              sys_kill,
    [SYS_sleep]             sys_sleep,
    [SYS_gettime]           sys_gettime,
    [SYS_getpid]            sys_getpid,
    [SYS_brk]               sys_brk,
    [SYS_mmap]              sys_mmap,
    [SYS_munmap]            sys_munmap,
    [SYS_shmem]             sys_shmem,
    [SYS_putc]              sys_putc,
    [SYS_pgdir]             sys_pgdir,
    [SYS_sem_init]          sys_sem_init,
    [SYS_sem_post]          sys_sem_post,
    [SYS_sem_wait]          sys_sem_wait,
    [SYS_sem_free]          sys_sem_free,
    [SYS_sem_get_value]     sys_sem_get_value,
};

#define NUM_SYSCALLS        ((sizeof(syscalls)) / (sizeof(syscalls[0])))

void
syscall(void) {
    struct trapframe *tf = current->tf;
    uint32_t arg[5];
    int num = tf->tf_regs.reg_eax;
    if (num >= 0 && num < NUM_SYSCALLS) {
        if (syscalls[num] != NULL) {
            arg[0] = tf->tf_regs.reg_edx;
            arg[1] = tf->tf_regs.reg_ecx;
            arg[2] = tf->tf_regs.reg_ebx;
            arg[3] = tf->tf_regs.reg_edi;
            arg[4] = tf->tf_regs.reg_esi;
            tf->tf_regs.reg_eax = syscalls[num](arg);
            return ;
        }
    }
    print_trapframe(tf);
    panic("undefined syscall %d, pid = %d, name = %s.\n",
            num, current->pid, current->name);
}

