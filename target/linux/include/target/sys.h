#ifndef _SYS_H_
#define _SYS_H_

#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <semaphore.h>
#include <assert.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (1<<(PAGE_SHIFT))

#define local_irq_save(flags) \
  (flags = 0)
#define local_irq_restore(flags) \
  (flags = 1)

#define ASSERT(x) assert((x))
#define BUG_ON(x) assert(!((x)))
#define printk(...) printf(__VA_ARGS__)

/* shutdown */
#define TARGET_SHTDN_POWEROFF 0
#define TARGET_SHTDN_REBOOT 1
#define TARGET_SHTDN_SUSPEND 2

#define target_suspend() \
  do {} while(0)

#define target_halt() \
  do {} while(0)

#define target_reboot() \
  do {} while(0)

void app_shutdown(unsigned reason);

#endif /* _SYS_H_ */
