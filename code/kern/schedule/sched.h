#ifndef __KERN_SCHEDULE_SCHED_H__
#define __KERN_SCHEDULE_SCHED_H__

#include <types.h>
#include <list.h>
#include <proc.h>

typedef struct {
    unsigned int expires;
    struct proc_struct *proc;
    list_entry_t timer_link;
} timer_t;

#define le2timer(le, member)            \
    to_struct((le), timer_t, member)

static inline timer_t *
timer_init(timer_t *timer, struct proc_struct *proc, int expires) {
    timer->expires = expires;
    timer->proc = proc;
    list_init(&(timer->timer_link));
    return timer;
}

void sched_init(void);
void wakeup_proc(struct proc_struct *proc);
void schedule(void);
void add_timer(timer_t *timer);
void del_timer(timer_t *timer);
void run_timer_list(void);

#endif /* !__KERN_SCHEDULE_SCHED_H__ */

