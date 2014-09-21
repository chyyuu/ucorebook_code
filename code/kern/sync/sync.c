#include <sync.h>
#include <mbox.h>
#include <monitor.h>

void
sync_init(void) {
    mbox_init();
    monitor_init();
}

