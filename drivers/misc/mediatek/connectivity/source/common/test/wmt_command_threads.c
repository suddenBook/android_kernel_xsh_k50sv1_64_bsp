/* Host-only scheduling cases included after extracted production functions. */
#define REQUIRE(value) do { if (!(value)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; \
} } while (0)

static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t copy_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t copy_cv = PTHREAD_COND_INITIALIZER;
static bool copy_entered, copy_release;
static atomic_int stop, errors, deliveries, cancel_started;
static int producer_result, reader_result;
static const int transfers = 2000;

static void on_event(void)
{
    pthread_mutex_lock(&event_lock);
    pthread_cond_broadcast(&event_cv);
    pthread_mutex_unlock(&event_lock);
}

static void on_wait(void) {}

static void on_copy(void)
{
    if (scenario != 102) return;
    pthread_mutex_lock(&copy_lock);
    copy_entered = true;
    pthread_cond_broadcast(&copy_cv);
    while (!copy_release) pthread_cond_wait(&copy_cv, &copy_lock);
    pthread_mutex_unlock(&copy_lock);
}

static int await_command(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 4;
    pthread_mutex_lock(&event_lock);
    while (!stop && !wmt_lib_get_cmd_status()) {
        if (pthread_cond_timedwait(&event_cv, &event_lock, &deadline) == ETIMEDOUT) {
            errors++;
            break;
        }
    }
    int ready = wmt_lib_get_cmd_status();
    pthread_mutex_unlock(&event_lock);
    return ready;
}

static void *producer(void *unused)
{
    (void)unused;
    if (scenario != 101) {
        producer_result = wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)command);
        return NULL;
    }
    for (int i = 0; i < transfers; i++) {
        UINT8 value[32];
        snprintf((char *)value, sizeof(value), "cmd-%08d", i);
        if (wmt_ctrl_ul_cmd(&gDevWmt, value)) { errors++; break; }
    }
    stop = 1;
    on_event();
    return NULL;
}

static void *reader(void *unused)
{
    (void)unused;
    do {
        if (!await_command()) continue;
        char value[NAME_MAX + 1] = {0};
        int count = WMT_read(NULL, value, sizeof(value), NULL);
        if (scenario == 102) { reader_result = count; return NULL; }
        if (!count) continue;
        if (count != 12 || memcmp(value, "cmd-", 4)) { errors++; break; }
        int expected = atomic_fetch_add(&deliveries, 1);
        if (atoi(value + 4) != expected) errors++;
        if (WMT_write(NULL, "ok", 2, NULL) != 2) errors++;
    } while (!stop);
    return NULL;
}

static void *canceller(void *unused)
{
    (void)unused;
    cancel_started = 1;
    wmt_lib_cancel_cmd();
    return NULL;
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    scenario = atoi(argv[1]);
    pthread_t writer, readers[2], resetter;
    REQUIRE(pthread_create(&writer, NULL, producer, NULL) == 0);
    if (scenario == 100) {
        REQUIRE(await_command());
        char value[NAME_MAX + 1] = {0};
        REQUIRE(WMT_read(NULL, value, sizeof(value), NULL) == 9);
        REQUIRE(wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)"second") == -EBUSY);
        REQUIRE(WMT_write(NULL, "ok", 2, NULL) == 2);
        REQUIRE(pthread_join(writer, NULL) == 0);
        REQUIRE(producer_result == 0 && event_calls == 1);
    } else if (scenario == 101) {
        for (int i = 0; i < 2; i++) REQUIRE(pthread_create(&readers[i], NULL, reader, NULL) == 0);
        REQUIRE(pthread_join(writer, NULL) == 0);
        for (int i = 0; i < 2; i++) REQUIRE(pthread_join(readers[i], NULL) == 0);
        REQUIRE(deliveries == transfers && read_copies == transfers);
    } else if (scenario == 102) {
        REQUIRE(pthread_create(&readers[0], NULL, reader, NULL) == 0);
        pthread_mutex_lock(&copy_lock);
        while (!copy_entered) pthread_cond_wait(&copy_cv, &copy_lock);
        REQUIRE(pthread_create(&resetter, NULL, canceller, NULL) == 0);
        while (!cancel_started) sched_yield();
        copy_release = true;
        pthread_cond_broadcast(&copy_cv);
        pthread_mutex_unlock(&copy_lock);
        REQUIRE(pthread_join(readers[0], NULL) == 0);
        REQUIRE(pthread_join(resetter, NULL) == 0);
        REQUIRE(pthread_join(writer, NULL) == 0);
        REQUIRE(reader_result == 9 && producer_result == -ECANCELED);
        REQUIRE(WMT_read(NULL, received, sizeof(received), NULL) == 0);
        REQUIRE(WMT_write(NULL, "ok", 2, NULL) < 0);
    } else return 2;
    REQUIRE(errors == 0);
    puts("PASS");
    return 0;
}
