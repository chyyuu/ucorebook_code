#include <types.h>
#include <mmu.h>
#include <pmm.h>
#include <vmm.h>
#include <ipc.h>
#include <proc.h>
#include <slab.h>
#include <sem.h>
#include <monitor.h>
#include <wait.h>
#include <list.h>
#include <error.h>
#include <assert.h>
#include <clock.h>
#include <string.h>

typedef struct monitor_s
{
     int status;
     union
     {
          struct
          {
               int conds_count;
               wait_queue_t entry;
               wait_queue_t entry_urgent;
               wait_queue_t *conds_wait;
          };

          struct monitor_s *free_next;
     };

} monitor_t;

#define MONITOR_OCCUPIED  2
#define MONITOR_AVAILABLE 1
#define MONITOR_FREE      0

#define MAX_MONITOR_NUM 8192

static semaphore_t monitor_alloc_sem;
static monitor_t *monitor_free_head;
static monitor_t monitors[MAX_MONITOR_NUM];

void
monitor_init(void)
{
     int i;
     for (i = 0; i < MAX_MONITOR_NUM - 1; ++ i)
          monitors[i].free_next = &monitors[i + 1];
     monitor_free_head = &monitors[0];
     sem_init(&monitor_alloc_sem, 1);
}

int
monitor_alloc(int conds_count)
{
     wait_queue_t *conds_wait = kmalloc(sizeof(wait_queue_t) * conds_count);
     if (conds_wait == NULL) return -1;

     down(&monitor_alloc_sem);

     if (monitor_free_head == NULL)
     {
          up(&monitor_alloc_sem);
          kfree(conds_wait);
          return -1;
     }

     monitor_t *result = monitor_free_head;
     monitor_free_head = result->free_next;

     wait_queue_init(&result->entry);
     wait_queue_init(&result->entry_urgent);
     result->conds_count = conds_count;
     result->conds_wait = conds_wait;

     int i;
     for (i = 0; i != conds_count; ++ i)
     {
          wait_queue_init(&conds_wait[i]);
     }
     
     up(&monitor_alloc_sem);

     result->status = MONITOR_AVAILABLE;
     return result - monitors;
}

int
monitor_free(int monitor)
{
     monitors[monitor].status = MONITOR_FREE;
     
     down(&monitor_alloc_sem);
     
     kfree(monitors[monitor].conds_wait);
     
     monitors[monitor].free_next = monitor_free_head;
     monitor_free_head = &monitors[monitor];
     
     up(&monitor_alloc_sem);
     return 0;
}

int
monitor_enter(int monitor)
{
     bool intr_flag;
     monitor_t *m = &monitors[monitor];
     local_intr_save(intr_flag);

     if (m->status == MONITOR_AVAILABLE)
     {
          m->status = MONITOR_OCCUPIED;
          local_intr_restore(intr_flag);
          return 0;
     }

     wait_t __wait, *wait = &__wait;
     while (1)
     {
          wait_current_set(&m->entry, wait, WT_MONITOR_ENTER);
          local_intr_restore(intr_flag);

          schedule();

          local_intr_save(intr_flag);
          if (wait_in_queue(wait)) continue;
          else break;
     }

     local_intr_restore(intr_flag);
     return 0;
}

int
monitor_leave(int monitor)
{
     bool intr_flag;
     monitor_t *m = &monitors[monitor];
     local_intr_save(intr_flag);
     
     if (wait_queue_empty(&m->entry) &&
         wait_queue_empty(&m->entry_urgent))
     {
          m->status = MONITOR_AVAILABLE;
          local_intr_restore(intr_flag);
          return 0;
     }

     if (!wait_queue_empty(&m->entry_urgent))
     {
          wakeup_first(&m->entry_urgent, WT_MONITOR_ENTER, 1);
     }
     else if (!wait_queue_empty(&m->entry))
     {
          wakeup_first(&m->entry, WT_MONITOR_ENTER, 1);
     }

     local_intr_restore(intr_flag);
     return 0;
}

int
monitor_cond_wait(int monitor, int cond)
{
     bool intr_flag;
     monitor_t *m = &monitors[monitor];
     local_intr_save(intr_flag);
     
     if (cond >= m->conds_count)
     {
          local_intr_restore(intr_flag);
          return -1;
     }

     /* Same logic as ``leave'' here  */
     if (wait_queue_empty(&m->entry) &&
         wait_queue_empty(&m->entry_urgent))
     {
          m->status = MONITOR_AVAILABLE;
     }
     else
     {
          if (!wait_queue_empty(&m->entry_urgent))
          {
               wakeup_first(&m->entry_urgent, WT_MONITOR_ENTER, 1);
          }
          else if (!wait_queue_empty(&m->entry))
          {
               wakeup_first(&m->entry, WT_MONITOR_ENTER, 1);
          }
     }

     wait_t __wait, *wait = &__wait;
     while (1)
     {
          wait_current_set(&m->conds_wait[cond], wait, WT_MONITOR_COND);
          local_intr_restore(intr_flag);

          schedule();

          local_intr_save(intr_flag);
          if (wait_in_queue(wait)) continue;
          else break;
     }

     local_intr_restore(intr_flag);
     return 0;
}

int
monitor_cond_signal(int monitor, int cond)
{
     bool intr_flag;
     monitor_t *m = &monitors[monitor];
     local_intr_save(intr_flag);

     if (cond >= m->conds_count)
     {
          local_intr_restore(intr_flag);
          return -1;
     }

     if (!wait_queue_empty(&m->conds_wait[cond]))
     {
          wait_t *wait = wait_queue_first(&m->conds_wait[cond]);
          wait_queue_del(&m->conds_wait[cond], wait);
          wait_queue_add(&m->entry_urgent, wait);
     }

     local_intr_restore(intr_flag);
     return 0;
}
