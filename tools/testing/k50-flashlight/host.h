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

#define __KERNEL__
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
static bool pbm_on, enforce_pbm;
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
	pbm_on = on != 0;
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
	if (enforce_pbm && addr == PMIC_ISINK_CH0_EN_ADDR && value == 1)
		assert(pbm_on);
	assert(!(addr & 1) && addr / 2 < ARRAY_SIZE(pmic_registers));
	assert(value <= mask && (mask << shift) <= 0xffff);
	writes[write_count++] = (struct pmic_write){ addr, value, mask, shift };
	if (write_count == fail_at)
		return write_error;
	pmic_registers[addr / 2] &= ~(mask << shift);
	pmic_registers[addr / 2] |= value << shift;
	return 0;
}

#define READ_ONCE(value) (value)
#define DEFINE_SPINLOCK(name) DEFINE_MUTEX(name)
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; mutex_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); mutex_unlock(lock); } while (0)
#define __init
#define module_init(func)
#define PAGE_SIZE 4096
#define scnprintf snprintf
#define NOTIFY_DONE 0

struct device { int unused; };
struct attribute { int unused; };
struct device_attribute { struct attribute attr; };
struct attribute_group { struct attribute **attrs; };
#define DEVICE_ATTR_RO(name) struct device_attribute dev_attr_##name

enum led_brightness { LED_OFF = 0, LED_FULL = 255 };
struct led_classdev {
	const char *name;
	unsigned int max_brightness;
	void (*brightness_set)(struct led_classdev *, enum led_brightness);
	enum led_brightness (*brightness_get)(struct led_classdev *);
	const struct attribute_group **groups;
};
static bool led_registered;
static int led_classdev_register(struct device *parent, struct led_classdev *led)
{
	led_registered = true;
	return 0;
}
static void led_classdev_unregister(struct led_classdev *led)
{
	led_registered = false;
	led->brightness_set(led, LED_OFF);
}
struct notifier_block {
	int (*notifier_call)(struct notifier_block *, unsigned long, void *);
};
static int register_reboot_notifier(struct notifier_block *notifier) { return 0; }
static void unregister_reboot_notifier(struct notifier_block *notifier) { }

static void flush_work(struct work_struct *work)
{
	for (;;) {
		mutex_lock(&work->lock);
		while (work->running)
			pthread_cond_wait(&work->changed, &work->lock);
		bool pending = work->pending;
		mutex_unlock(&work->lock);
		if (!pending) return;
		run_work(work);
	}
}
static int read_error;
static int pmic_read_interface(unsigned int addr, unsigned int *value,
			      unsigned int mask, unsigned int shift)
{
	if (read_error) return read_error;
	*value = (pmic_registers[addr / 2] >> shift) & mask;
	return 0;
}
