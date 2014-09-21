#include <ulib.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#define scprintf(...)               \
    do {                            \
        sem_wait(sem_console);      \
        cprintf(__VA_ARGS__);       \
        sem_post(sem_console);      \
    } while (0)


sem_t sem_console;
int   monitor;
int  *pCountR, *pCountW;

#define COND_READ  0
#define COND_WRITE 1
#define COND_COUNT 2

void
failed(void) {
    cprintf("FAIL: T.T\n");
    exit(-1);
}

void
init(void) {
    if ((sem_console = sem_init(1)) < 0) {
        failed();
    }
    
    if ((monitor = monitor_alloc(COND_COUNT)) < 0)
    {
         failed();
    }
         
    if ((pCountR = shmem_malloc(sizeof(int))) == NULL || (pCountW = shmem_malloc(sizeof(int))) == NULL) {
        failed();
    }
    *pCountR = *pCountW = 0;
}

void
reader(int id, int time) {
    scprintf("reader %d: (pid:%d) arrive\n", id, getpid());
    monitor_enter(monitor);
    
    (*pCountR) ++;
    if (*pCountW > 0) {
         monitor_cond_wait(monitor, COND_READ);
         monitor_cond_signal(monitor, COND_READ);
    }
    monitor_leave(monitor);

    scprintf("    reader_rf %d: (pid:%d) start %d\n", id, getpid(), time);
    sleep(time);
    scprintf("    reader_rf %d: (pid:%d) end %d\n", id, getpid(), time);

    monitor_enter(monitor);
    (*pCountR) --;
    if (*pCountW > 0) {
         monitor_cond_signal(monitor, COND_WRITE);
    }
    monitor_leave(monitor);
}

void
writer(int id, int time) {
    scprintf("writer %d: (pid:%d) arrive\n", id, getpid());
    monitor_enter(monitor);
    
    (*pCountW) ++;
    if (*pCountR > 0) {
         monitor_cond_wait(monitor, COND_WRITE);
    }

    scprintf("    writer_rf %d: (pid:%d) start %d\n", id, getpid(), time);
    sleep(time);
    scprintf("    writer_rf %d: (pid:%d) end %d\n", id, getpid(), time);

    (*pCountW) --;
    if (*pCountW == 0) {
         monitor_cond_signal(monitor, COND_READ);
    }
    else
    {
         monitor_cond_signal(monitor, COND_WRITE);
    }

    monitor_leave(monitor);
}

void
read_test_wf(void) {
    cprintf("---------------------------------\n");
    srand(0);
    int i, total = 10, time;
    for (i = 0; i < total; i ++) {
        time = (unsigned int)rand() % 3;
        if (fork() == 0) {
            yield();
            reader(i, 100 + time * 10);
            exit(0);
        }
    }

    for (i = 0; i < total; i ++) {
        if (wait() != 0) {
            failed();
        }
    }
    cprintf("read_test_wf ok.\n");
}

void
write_test_wf(void) {
    cprintf("---------------------------------\n");
    srand(100);
    int i, total = 10, time;
    for (i = 0; i < total; i ++) {
        time = (unsigned int)rand() % 3;
        if (fork() == 0) {
            yield();
            writer(i, 100 + time * 10);
            exit(0);
        }
    }

    for (i = 0; i < total; i ++) {
        if (wait() != 0) {
            failed();
        }
    }
    cprintf("write_test_wf ok.\n");
}

void
read_write_test_wf(void) {
    cprintf("---------------------------------\n");
    srand(200);
    int i, total = 10, time;
    for (i = 0; i < total; i ++) {
        time = (unsigned int)rand() % 3;
        if (fork() == 0) {
            yield();
            if (time == 0) {
                writer(i, 100 + time * 10);
            }
            else {
                reader(i, 100 + time * 10);
            }
            exit(0);
        }
    }

    for (i = 0; i < total; i ++) {
        if (wait() != 0) {
            failed();
        }
    }
    cprintf("read_write_test_wf ok.\n");
}

int
main(void) {
    init();
    read_test_wf();
    write_test_wf();
    read_write_test_wf();

    cprintf("monitor_wf pass..\n");
    return 0;
}

