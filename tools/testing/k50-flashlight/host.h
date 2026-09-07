/* Minimal host substitutes for the actual driver's kernel services. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "kd_flashlight.h"
#include "mach/mt6353_hw.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NSEC_PER_MSEC 1000000L
#define __user
#define __exit
#define EXPORT_SYMBOL(name)
#define MODULE_DESCRIPTION(text)
#define MODULE_LICENSE(text)
#define module_exit(func)
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock(lock) assert(pthread_mutex_lock(lock) == 0)
#define mutex_unlock(lock) assert(pthread_mutex_unlock(lock) == 0)

static unsigned int logged_errors;
#define pr_err(...) ((void)logged_errors++)
#define logI(...) ((void)0)

/* Fixture for the actual setFlashDrv() selector; other physical strobes are absent. */
static FLASHLIGHT_FUNCTION_STRUCT
	*g_pFlashInitFunc[e_Max_Sensor_Dev_Num][e_Max_Strobe_Num_Per_Dev][e_Max_Part_Num_Per_Dev];
static int g_strobePartId[e_Max_Sensor_Dev_Num][e_Max_Strobe_Num_Per_Dev];
static int gLowBatDuty[e_Max_Sensor_Dev_Num][e_Max_Strobe_Num_Per_Dev];
static int gLowPowerPer, gLowPowerVbat;
#define BATTERY_PERCENT_LEVEL_0 0
#define LOW_BATTERY_LEVEL_0 0
DEFINE_MUTEX(g_mutex);

struct file { int unused; };
static size_t copy_missing;
static unsigned int pbm_calls;
static size_t copy_from_user(void *to, const void *from, size_t size)
{
	assert(copy_missing <= size);
	if (size != copy_missing)
		memcpy(to, from, size - copy_missing);
	/* ARM64 copy_from_user() clears the uncopied tail on a fault. */
	memset((char *)to + size - copy_missing, 0, copy_missing);
	return copy_missing;
}
static size_t copy_to_user(void *to, const void *from, size_t size)
{
	memcpy(to, from, size);
	return 0;
}
static void kicker_pbm_by_flash(int on)
{
	pbm_calls++;
}
static int strobe_getPartId(int sensor, int strobe)
{
	return 1;
}

static MUINT32 unavailable_strobe(FLASHLIGHT_FUNCTION_STRUCT **func)
{
	*func = NULL;
	return 0;
}
#define strobeInit_main_sid1_part2 unavailable_strobe
#define strobeInit_main_sid2_part1 unavailable_strobe
#define strobeInit_main_sid2_part2 unavailable_strobe
#define subStrobeInit unavailable_strobe
#define strobeInit_sub_sid1_part2 unavailable_strobe
#define strobeInit_sub_sid2_part1 unavailable_strobe
#define strobeInit_sub_sid2_part2 unavailable_strobe

typedef int64_t ktime_t;
enum hrtimer_restart { HRTIMER_NORESTART };
#define HRTIMER_MODE_REL 0
struct hrtimer {
	enum hrtimer_restart (*function)(struct hrtimer *);
	ktime_t expires;
	bool active;
};
static void hrtimer_init(struct hrtimer *timer, int clock, int mode)
{
	timer->active = false;
}
static ktime_t ktime_set(int64_t seconds, long nanoseconds)
{
	assert(nanoseconds >= 0 && nanoseconds < 1000000000L);
	return seconds * 1000000000LL + nanoseconds;
}
static void hrtimer_start(struct hrtimer *timer, ktime_t expires, int mode)
{
	timer->expires = expires;
	timer->active = true;
}
static void hrtimer_cancel(struct hrtimer *timer)
{
	timer->active = false;
}
static bool expire_timer(struct hrtimer *timer)
{
	if (!timer->active)
		return false;
	timer->active = false;
	assert(timer->function(timer) == HRTIMER_NORESTART);
	return true;
}

struct work_struct {
	void (*function)(struct work_struct *);
	pthread_mutex_t lock;
	pthread_cond_t changed;
	bool pending, running;
	unsigned int cancel_waiters;
};
#define DECLARE_WORK(name, func) struct work_struct name = { \
	.function = func, .lock = PTHREAD_MUTEX_INITIALIZER, \
	.changed = PTHREAD_COND_INITIALIZER }
static void schedule_work(struct work_struct *work)
{
	mutex_lock(&work->lock);
	work->pending = true;
	mutex_unlock(&work->lock);
}
static void cancel_work_sync(struct work_struct *work)
{
	mutex_lock(&work->lock);
	work->pending = false;
	while (work->running) {
		work->cancel_waiters++;
		pthread_cond_broadcast(&work->changed);
		pthread_cond_wait(&work->changed, &work->lock);
		work->cancel_waiters--;
	}
	mutex_unlock(&work->lock);
}
static bool run_work(struct work_struct *work)
{
	mutex_lock(&work->lock);
	if (!work->pending) {
		mutex_unlock(&work->lock);
		return false;
	}
	assert(!work->running);
	work->pending = false;
	work->running = true;
	pthread_cond_broadcast(&work->changed);
	mutex_unlock(&work->lock);
	work->function(work);
	mutex_lock(&work->lock);
	work->running = false;
	pthread_cond_broadcast(&work->changed);
	mutex_unlock(&work->lock);
	return true;
}

struct pmic_write {
	unsigned int addr, value, mask, shift;
};
static struct pmic_write writes[1024];
static unsigned int write_count, fail_at;
static int write_error;
static uint16_t pmic_registers[0x400];
static unsigned int pmic_config_interface(unsigned int addr, unsigned int value,
					  unsigned int mask, unsigned int shift)
{
	assert(write_count < ARRAY_SIZE(writes));
	assert(!(addr & 1) && addr / 2 < ARRAY_SIZE(pmic_registers));
	assert(value <= mask && (mask << shift) <= 0xffff);
	writes[write_count++] = (struct pmic_write){ addr, value, mask, shift };
	if (write_count == fail_at)
		return write_error;
	pmic_registers[addr / 2] &= ~(mask << shift);
	pmic_registers[addr / 2] |= value << shift;
	return 0;
}
