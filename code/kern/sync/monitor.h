#ifndef __KERN_SYNC_MONITOR_H__
#define __KERN_SYNC_MONITOR_H__

#include <types.h>
void monitor_init(void);

int monitor_alloc(int cond_count);
int monitor_free(int monitor);

int monitor_enter(int monitor);
int monitor_leave(int monitor);
int monitor_cond_wait(int monitor, int cond);
int monitor_cond_signal(int monitor, int cond);

#endif    /* !__KERN_SYNC_MONITOR_H__ */
