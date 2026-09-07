/* Host-only concurrency test, appended to the extracted initialization code. */
#define REQUIRE(value) do { if (!(value)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; \
} } while (0)

static pthread_barrier_t start;
static int results[2];
static ULONG arguments[2] = {0x23, 0x14};

static void *configure(void *argument)
{
    int index = (int)(uintptr_t)argument;
    pthread_barrier_wait(&start);
    results[index] = run_ioctl(WMT_IOCTL_SET_STP_MODE, arguments[index]);
    return NULL;
}

static void *unblank(void *unused)
{
    (void)unused;
    INT32 blank = FB_BLANK_UNBLANK;
    struct fb_event event = {.data = &blank};
    pthread_barrier_wait(&start);
    wmt_fb_notifier_callback(NULL, FB_EVENT_BLANK, &event);
    return NULL;
}

int main(void)
{
    REQUIRE(pthread_barrier_init(&start, NULL, 3) == 0);
    for (int round = 0; round < 100; round++) {
        /* Every previous thread has joined before the next simulated init. */
        hif_info = 0;
        queued = 0;
        tx_calls = 0;
        plat_calls = 0;
        scheduled = 0;
        g_late_pwr_on_for_blank = 0;
        g_es_lr_flag_for_blank = 0;
        memset(&gDevWmt.rWmtHifConf, 0, sizeof(WMT_HIF_CONF));
        pthread_t threads[3];
        REQUIRE(pthread_create(&threads[0], NULL, configure, (void *)(uintptr_t)0) == 0);
        REQUIRE(pthread_create(&threads[1], NULL, configure, (void *)(uintptr_t)1) == 0);
        REQUIRE(pthread_create(&threads[2], NULL, unblank, NULL) == 0);
        for (int i = 0; i < 3; i++) REQUIRE(pthread_join(threads[i], NULL) == 0);
        REQUIRE(results[0] == 0 && results[1] == 0);
        REQUIRE(hif_info == 1 && queued == 1 && tx_calls == 1 && pool_owned == 0);
        REQUIRE(scheduled == 1 && g_late_pwr_on_for_blank == 0);
        REQUIRE(memcmp(&queued_hif, &gDevWmt.rWmtHifConf, sizeof(queued_hif)) == 0);
    }
    REQUIRE(pthread_barrier_destroy(&start) == 0);
    puts("PASS: 100 rounds, two initializers and one framebuffer notification");
    return 0;
}
