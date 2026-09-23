/* Appended after the complete, unmodified driver body by run.py. */
static unsigned int checks, failures;
#define CHECK(name, expr) do { checks++; if (!(expr)) { \
	failures++; printf("FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static FLASHLIGHT_FUNCTION_STRUCT *flash;
#define IOCTL(cmd, value) flash->flashlight_ioctl(cmd, value)
#define OPEN() flash->flashlight_open(NULL)
#define CLOSE() flash->flashlight_release(NULL)
#define CHANNELS() (pmic_registers[0x326 / 2] & 3)
#define CORE_IOCTL(cmd) flashlight_ioctl_core(NULL, cmd, (unsigned long)&user_arg)

/* Independent oracle: address/value/mask/shift decoded from stock arm64 code. */
static const struct pmic_write stock_init[] = {
	{ 0x326, 0, 1, 0 }, { 0x274, 0, 1, 1 }, { 0x26e, 0, 1, 12 },
	{ 0x328, 0, 3, 14 }, { 0x312, 5, 7, 12 }, { 0x312, 31, 31, 7 },
	{ 0x310, 0, 0xffff, 0 }, { 0x326, 0, 1, 1 },
	{ 0x274, 0, 1, 1 }, { 0x26e, 0, 1, 13 },
	{ 0x328, 0, 3, 12 }, { 0x31a, 5, 7, 12 }, { 0x31a, 31, 31, 7 },
	{ 0x318, 0, 0xffff, 0 },
};

static void reset_trace(void)
{
	write_count = 0;
	fail_at = 0;
	write_error = 0;
}

static void *timeout_thread(void *arg)
{
	CHECK("queued worker runs", run_work(&flash_timeout_work));
	return NULL;
}

static void *close_thread(void *arg)
{
	*(int *)arg = CLOSE();
	return NULL;
}

static void *open_thread(void *arg)
{
	*(int *)arg = OPEN();
	return NULL;
}

static void *reopen_torch_thread(void *arg)
{
	int ret = OPEN();
	if (!ret)
		ret = IOCTL(FLASH_IOC_SET_TIME_OUT_TIME_MS, 0);
	if (!ret)
		ret = IOCTL(FLASH_IOC_SET_ONOFF, 1);
	*(int *)arg = ret;
	return NULL;
}

int main(void)
{
	unsigned int before, i;
	pthread_t worker, closer, opener;
	int close_result, open_result;
	kdStrobeDrvArg user_arg = { e_CAMERA_MAIN_SENSOR, 1, 0 };

	CHECK("HAL callback table", constantFlashlightInit(&flash) == 0 && flash);
	CHECK("unopened ioctl rejected", IOCTL(FLASH_IOC_SET_ONOFF, 1) == -ENODEV);
	CHECK("unopened release", CLOSE() == 0 && write_count == 0);
	memset(pmic_registers, 0xff, sizeof(pmic_registers));
	CHECK("open", OPEN() == 0);
	CHECK("exact stock PMIC init", write_count == ARRAY_SIZE(stock_init) &&
		!memcmp(writes, stock_init, sizeof(stock_init)));
	CHECK("only flash channels disabled", pmic_registers[0x326 / 2] == 0xfffc);
	before = write_count;
	CHECK("duplicate open", OPEN() == -EBUSY && write_count == before);
	CHECK("HAL duty and step", IOCTL(FLASH_IOC_SET_DUTY, ULONG_MAX) == 0 &&
		IOCTL(FLASH_IOC_SET_STEP, 42) == 0 && write_count == before);
	CHECK("unknown ioctl", IOCTL(0, 0) == -EPERM);
	CHECK("enable both channels", IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 && CHANNELS() == 3);
	CHECK("stock default timeout", flash_timer.active && flash_timer.expires == 1000000000LL);
	CHECK("timeout queues work", expire_timer(&flash_timer) && flash_timeout_work.pending);
	CHECK("timeout disables both", run_work(&flash_timeout_work) && CHANNELS() == 0);

	CHECK("large valid timeout", IOCTL(FLASH_IOC_SET_TIME_OUT_TIME_MS, INT_MAX) == 0 &&
		IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 &&
		flash_timer.expires == (int64_t)INT_MAX * 1000000);
	CHECK("negative timeout rejected", IOCTL(FLASH_IOC_SET_TIME_OUT_TIME_MS, ULONG_MAX) == -EINVAL);
	CHECK("oversize timeout rejected", IOCTL(FLASH_IOC_SET_TIME_OUT_TIME_MS,
		(unsigned long)INT_MAX + 1) == -EINVAL && flash_timeout_ms == INT_MAX);
	CHECK("fractional second timeout", IOCTL(FLASH_IOC_SET_TIME_OUT_TIME_MS, 1201) == 0 &&
		IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 && flash_timer.expires == 1201000000LL);
	CHECK("old exposure expires", expire_timer(&flash_timer));
	CHECK("rearm drains queued timeout", IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 &&
		!run_work(&flash_timeout_work) && CHANNELS() == 3 && flash_timer.active);
	CHECK("off cancels timer", IOCTL(FLASH_IOC_SET_ONOFF, 0) == 0 &&
		CHANNELS() == 0 && !expire_timer(&flash_timer));
	CHECK("prepare queued off", IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 && expire_timer(&flash_timer));
	CHECK("off drains queued work", IOCTL(FLASH_IOC_SET_ONOFF, 0) == 0 &&
		!run_work(&flash_timeout_work) && CHANNELS() == 0);
	CHECK("continuous torch", IOCTL(FLASH_IOC_SET_TIME_OUT_TIME_MS, 0) == 0 &&
		IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 && CHANNELS() == 3 && !expire_timer(&flash_timer));
	CHECK("release torch", CLOSE() == 0 && CHANNELS() == 0);
	CHECK("reopen resets default timeout", OPEN() == 0 && flash_timeout_ms == 1000);
	CHECK("prepare queued release", IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 && expire_timer(&flash_timer));
	CHECK("release drains queued work", CLOSE() == 0 && !flash_timeout_work.pending);
	CHECK("old timeout cannot turn off new torch", OPEN() == 0 &&
		IOCTL(FLASH_IOC_SET_TIME_OUT_TIME_MS, 0) == 0 && IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 &&
		!run_work(&flash_timeout_work) && CHANNELS() == 3);
	CHECK("close before fault tests", CLOSE() == 0);

	for (i = 1; i <= ARRAY_SIZE(stock_init); i++) {
		reset_trace();
		pmic_registers[0x326 / 2] = 0xffff;
		fail_at = i;
		write_error = 7; /* PWRAP uses positive status codes. */
		CHECK("init error returned", OPEN() == -EIO && !flash_in_use);
		CHECK("failed init disables both", write_count == i + 2 && CHANNELS() == 0);
		CHECK("failed open can retry", OPEN() == 0 && CLOSE() == 0);
	}
	reset_trace();
	CHECK("open for enable errors", OPEN() == 0);
	for (i = 1; i <= 2; i++) {
		before = write_count;
		fail_at = before + i;
		write_error = -ENXIO;
		CHECK("enable errno returned", IOCTL(FLASH_IOC_SET_ONOFF, 1) == -ENXIO);
		CHECK("partial enable rolled back", write_count == before + i + 2 &&
			CHANNELS() == 0 && !flash_timer.active);
	}
	CHECK("enable recovers", IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0);
	before = write_count;
	fail_at = before + 1;
	write_error = -ETIMEDOUT;
	CHECK("off errno returned", IOCTL(FLASH_IOC_SET_ONOFF, 0) == -ETIMEDOUT);
	CHECK("off still attempts second channel", write_count == before + 2 &&
		CHANNELS() == 1 && !flash_timer.active);
	CHECK("off can retry", IOCTL(FLASH_IOC_SET_ONOFF, 0) == 0 && CHANNELS() == 0);
	CHECK("prepare async error", IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 && expire_timer(&flash_timer));
	fail_at = write_count + 1;
	before = logged_errors;
	CHECK("async error is logged", run_work(&flash_timeout_work) && logged_errors == before + 1);
	CHECK("prepare release error", IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0);
	fail_at = write_count + 1;
	CHECK("release errno returned", CLOSE() == -ETIMEDOUT && !flash_in_use && !flash_timer.active);
	CHECK("failed release can recover on reopen", OPEN() == 0 && CHANNELS() == 0 && CLOSE() == 0);

	reset_trace();
	assert(pthread_create(&closer, NULL, open_thread, &close_result) == 0);
	assert(pthread_create(&opener, NULL, open_thread, &open_result) == 0);
	pthread_join(closer, NULL);
	pthread_join(opener, NULL);
	CHECK("concurrent open has one owner", (close_result == 0 && open_result == -EBUSY) ||
		(close_result == -EBUSY && open_result == 0));
	CHECK("concurrent open initializes once", write_count == ARRAY_SIZE(stock_init));
	CHECK("prepare running timeout", IOCTL(FLASH_IOC_SET_ONOFF, 1) == 0 && expire_timer(&flash_timer));
	mutex_lock(&flash_hw_lock);
	assert(pthread_create(&worker, NULL, timeout_thread, NULL) == 0);
	mutex_lock(&flash_timeout_work.lock);
	while (!flash_timeout_work.running)
		pthread_cond_wait(&flash_timeout_work.changed, &flash_timeout_work.lock);
	mutex_unlock(&flash_timeout_work.lock);
	assert(pthread_create(&closer, NULL, close_thread, &close_result) == 0);
	mutex_lock(&flash_timeout_work.lock);
	while (!flash_timeout_work.cancel_waiters)
		pthread_cond_wait(&flash_timeout_work.changed, &flash_timeout_work.lock);
	mutex_unlock(&flash_timeout_work.lock);
	assert(pthread_create(&opener, NULL, reopen_torch_thread, &open_result) == 0);
	mutex_unlock(&flash_hw_lock);
	pthread_join(worker, NULL);
	pthread_join(closer, NULL);
	pthread_join(opener, NULL);
	CHECK("release waits for running timeout without deadlock", close_result == 0 && open_result == 0);
	CHECK("old running timeout cannot disable reopened torch", CHANNELS() == 3 &&
		!flash_timer.active && !run_work(&flash_timeout_work));
	CHECK("release after concurrent reopen", CLOSE() == 0 && CHANNELS() == 0);

	reset_trace();
	g_strobePartId[0][0] = 1;
	fail_at = 1;
	write_error = -ENXIO;
	CHECK("SET_DRIVER propagates PMIC open failure", setFlashDrv(e_CAMERA_MAIN_SENSOR, 1) == -ENXIO);
	CHECK("failed selection exposes no callbacks", g_pFlashInitFunc[0][0][0] == NULL && !flash_in_use);
	CHECK("selection retry publishes initialized driver", setFlashDrv(e_CAMERA_MAIN_SENSOR, 1) == 0 &&
		g_pFlashInitFunc[0][0][0] == flash && flash_in_use);
	CHECK("selected callback enables flash", g_pFlashInitFunc[0][0][0]->flashlight_ioctl(
		FLASH_IOC_SET_ONOFF, 1) == 0 && CHANNELS() == 3);
	before = write_count;
	CHECK("busy selection preserves owner", setFlashDrv(e_CAMERA_MAIN_SENSOR, 1) == -EBUSY &&
		g_pFlashInitFunc[0][0][0] == flash && CHANNELS() == 3 && write_count == before);
	CHECK("selected callback releases", g_pFlashInitFunc[0][0][0]->flashlight_release(NULL) == 0);
	fail_at = write_count + 1;
	write_error = 7;
	CHECK("failed reopen clears closed callback", setFlashDrv(e_CAMERA_MAIN_SENSOR, 1) == -EIO &&
		g_pFlashInitFunc[0][0][0] == NULL && !flash_in_use);

	reset_trace();
	CHECK("ioctl core selects driver", CORE_IOCTL(FLASHLIGHTIOC_X_SET_DRIVER) == 0);
	CHECK("ioctl core accepts torch timeout", CORE_IOCTL(FLASH_IOC_SET_TIME_OUT_TIME_MS) == 0);
	user_arg.arg = 1;
	CHECK("ioctl core enables torch", CORE_IOCTL(FLASH_IOC_SET_ONOFF) == 0 && CHANNELS() == 3);
	before = write_count;
	copy_missing = sizeof(user_arg);
	CHECK("bad user pointer returns EFAULT", flashlight_ioctl_core(NULL,
		FLASH_IOC_SET_ONOFF, 0) == -EFAULT);
	CHECK("bad pointer leaves hardware unchanged", write_count == before && CHANNELS() == 3);
	copy_missing = sizeof(user_arg.arg);
	i = pbm_calls;
	CHECK("partial argument copy returns EFAULT", CORE_IOCTL(FLASH_IOC_SET_ONOFF) == -EFAULT);
	CHECK("partial copy cannot disable torch or kick PBM", write_count == before &&
		CHANNELS() == 3 && pbm_calls == i);
	copy_missing = 0;
	CHECK("valid ioctl still works after copy fault", CORE_IOCTL(FLASH_IOC_SET_ONOFF) == 0 &&
		CHANNELS() == 3);
	CHECK("protocol response unchanged", CORE_IOCTL(FLASH_IOC_GET_PROTOCOL_VERSION) == 1);
	CHECK("UNINIT closes driver", CORE_IOCTL(FLASH_IOC_UNINIT) == 0 && !flash_in_use && CHANNELS() == 0);
	CHECK("UNINIT removes global callback", g_pFlashInitFunc[0][0][0] == NULL);
	before = write_count;
	i = pbm_calls;
	CHECK("closed slot cannot dispatch ONOFF", CORE_IOCTL(FLASH_IOC_SET_ONOFF) == 0 &&
		pbm_calls == i && write_count == before);
	CHECK("repeat UNINIT remains harmless", CORE_IOCTL(FLASH_IOC_UNINIT) == 0 && write_count == before);
	CHECK("normal reopen after UNINIT", CORE_IOCTL(FLASHLIGHTIOC_X_SET_DRIVER) == 0 &&
		g_pFlashInitFunc[0][0][0] == flash && CORE_IOCTL(FLASH_IOC_SET_ONOFF) == 0);
	fail_at = write_count + 1;
	write_error = -ENXIO;
	CHECK("UNINIT preserves shutdown errno", CORE_IOCTL(FLASH_IOC_UNINIT) == -ENXIO && !flash_in_use);
	CHECK("failed shutdown still ends callback lifetime", g_pFlashInitFunc[0][0][0] == NULL);
	CHECK("reopen recovers failed shutdown", CORE_IOCTL(FLASHLIGHTIOC_X_SET_DRIVER) == 0 &&
		CHANNELS() == 0 && CORE_IOCTL(FLASH_IOC_UNINIT) == 0);

	/* Exercise the production LED callbacks and completion-status ABI. */
	char status[PAGE_SIZE];
	int enabled, available, error;
	reset_trace();
	enforce_pbm = true;
	CHECK("LED registers", constant_flashlight_init() == 0 && led_registered);
	CHECK("LED advertises binary torch", torch_led.max_brightness == 1);
	torch_brightness_set(&torch_led, 1);
	CHECK("write is queued, not a success report", CHANNELS() == 0 && !torch_enabled);
	CHECK("status waits for queued enable", status_show(NULL, NULL, status) > 0 &&
		sscanf(status, "%d %d %d", &enabled, &available, &error) == 3 &&
		enabled == 1 && available == 1 && error == 0 && CHANNELS() == 3 && pbm_on);
	CHECK("legacy cannot take enabled torch", OPEN() == -EBUSY);
	torch_brightness_set(&torch_led, 0);
	CHECK("off status is confirmed", status_show(NULL, NULL, status) > 0 &&
		!strcmp(status, "0 1 0\n") && CHANNELS() == 0 && !pbm_on);
	CHECK("legacy can acquire idle torch", OPEN() == 0);
	torch_brightness_set(&torch_led, 1);
	CHECK("LED reports legacy conflict", status_show(NULL, NULL, status) > 0 &&
		torch_result == -EBUSY && !torch_enabled && !torch_requested);
	CHECK("LED conflict does not release legacy", flash_in_use && CLOSE() == 0);
	torch_brightness_set(&torch_led, 1);
	flush_work(&torch_work);
	CHECK("LED retries after legacy release", CHANNELS() == 3 && torch_enabled);
	k50_torch_set_low_power(K50_TORCH_LOW_VOLTAGE, true);
	/* Reproduce an ON overtaking the low-power OFF before work runs. */
	torch_brightness_set(&torch_led, 1);
	CHECK("low power wins newer ON", status_show(NULL, NULL, status) > 0 &&
		CHANNELS() == 0 && !torch_enabled && torch_result == -EPERM && !pbm_on);
	CHECK("legacy cannot bypass low power", OPEN() == -EPERM);
	k50_torch_set_low_power(K50_TORCH_LOW_CAPACITY, true);
	k50_torch_set_low_power(K50_TORCH_LOW_VOLTAGE, false);
	torch_brightness_set(&torch_led, 1);
	flush_work(&torch_work);
	CHECK("separate low-power sources stay latched", torch_result == -EPERM && CHANNELS() == 0);
	k50_torch_set_low_power(K50_TORCH_LOW_CAPACITY, false);
	torch_brightness_set(&torch_led, 1);
	flush_work(&torch_work);
	CHECK("enable after both sources recover", torch_result == 0 && CHANNELS() == 3);
	torch_brightness_set(&torch_led, 0);
	flush_work(&torch_work);
	fail_at = write_count + 1;
	write_error = -ENXIO;
	torch_brightness_set(&torch_led, 1);
	CHECK("PMIC write rejection is reported", status_show(NULL, NULL, status) > 0 &&
		torch_result == -ENXIO && !torch_enabled && CHANNELS() == 0 && !pbm_on);
	read_error = 7;
	torch_brightness_set(&torch_led, 1);
	CHECK("failed readback is not success", status_show(NULL, NULL, status) == -EIO &&
		CHANNELS() == 0 && torch_state_unknown && pbm_on);
	CHECK("unknown state keeps exclusive ownership", OPEN() == -EBUSY);
	read_error = 0;
	CHECK("recovered readback releases conservative budget", status_show(NULL, NULL, status) > 0 &&
		!torch_state_unknown && !torch_enabled && !pbm_on);
	torch_brightness_set(&torch_led, 0);
	CHECK("confirmed OFF clears unknown state", status_show(NULL, NULL, status) > 0 &&
		!strcmp(status, "0 1 0\n") && !torch_state_unknown && !pbm_on);
	torch_brightness_set(&torch_led, 1);
	flush_work(&torch_work);
	torch_shutdown = true;
	torch_brightness_set(&torch_led, 0);
	torch_brightness_set(&torch_led, 1);
	flush_work(&torch_work);
	CHECK("shutdown wins newer ON", CHANNELS() == 0 && torch_result == -EPERM);
	CHECK("shutdown prevents legacy reopen", OPEN() == -EPERM);
	constant_flashlight_exit();
	CHECK("exit unregisters and drains LED", !led_registered && !torch_work.pending &&
		!flash_in_use && CHANNELS() == 0);
	printf("K50 flashlight: %u/%u passed\n", checks - failures, checks);
	return failures != 0;
}
