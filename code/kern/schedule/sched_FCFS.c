#include <types.h>
#include <list.h>
#include <proc.h>
#include <assert.h>
#include <sched_FCFS.h>

static void
FCFS_init(struct run_queue *rq) {
    list_init(&(rq->run_list));
    rq->proc_num = 0;
}

static void
FCFS_enqueue(struct run_queue *rq, struct proc_struct *proc) {
    assert(list_empty(&(proc->run_link)));
    list_add_before(&(rq->run_list), &(proc->run_link));
    proc->rq = rq;
    rq->proc_num ++;
}

static void
FCFS_dequeue(struct run_queue *rq, struct proc_struct *proc) {
    assert(!list_empty(&(proc->run_link)) && proc->rq == rq);
    list_del_init(&(proc->run_link));
    rq->proc_num --;
}

static struct proc_struct *
FCFS_pick_next(struct run_queue *rq) {
    list_entry_t *le = list_next(&(rq->run_list));
    if (le != &(rq->run_list)) {
        return le2proc(le, run_link);
    }
    return NULL;
}

static void
FCFS_proc_tick(struct run_queue *rq, struct proc_struct *proc) {
    /* do nothing */
}

struct sched_class FCFS_sched_class = {
    .name = "FCFS_scheduler",
    .init = FCFS_init,
    .enqueue = FCFS_enqueue,
    .dequeue = FCFS_dequeue,
    .pick_next = FCFS_pick_next,
    .proc_tick = FCFS_proc_tick,
};

