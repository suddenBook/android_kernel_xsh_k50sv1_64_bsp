/* Kernel scheduling, allocation and user-copy adapters; production is inserted. */
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "line %d: %s (case %d, phase %d)\n", __LINE__, #value, scenario, phase); \
    abort(); \
} } while (0)
static int scenario, phase;
typedef void VOID;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef uint8_t UINT8;
typedef uint8_t *PUINT8;
typedef unsigned long ULONG;
typedef long LONG;
typedef size_t SIZE_T;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef bool MTK_WCN_BOOL;
typedef uint32_t compat_uptr_t;
#define __user
#define GFP_KERNEL 0
#define WMT_STAT_CMD 6
#define MAX_PATCH_NUM 10
#define WMTDRV_TYPE_ANT 5
#define WMTDRV_TYPE_WMT 4
#define WMT_INIT_DONE 2
#define WMT_DEV_INIT_TO_MS 1000
#define MTK_WCN_BOOL_TRUE true
#define MTK_WCN_BOOL_FALSE false
#define WMTCHIN_CHIPID 0
#define WMTCHIN_HWVER 1
#define WMTCHIN_FWVER 2
#define WMTCHIN_IPVER 3
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
#define WMT_LOUD_FUNC(...) ((void)0)
#define BIT(n) (1UL << (n))
#define BUILD_BUG_ON(c) _Static_assert(!(c), #c)
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define cpu_to_le16 htole16
#define cpu_to_le32 htole32
#define cpu_to_le64 htole64
#define le16_to_cpu le16toh
#define le32_to_cpu le32toh
#define le64_to_cpu le64toh
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define osal_snprintf(b, n, ...) snprintf((char *)(b), (n), __VA_ARGS__)
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define DEFINE_SPINLOCK(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define spin_lock_irqsave(m, f) do { (f) = 0; CHECK(!pthread_mutex_lock(m)); } while (0)
#define spin_unlock_irqrestore(m, f) do { (void)(f); CHECK(!pthread_mutex_unlock(m)); } while (0)
static _Atomic unsigned long clock_ticks = 100;
#define jiffies atomic_load(&clock_ticks)
#define msecs_to_jiffies(t) ((unsigned long)(t))
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define compat_ptr(a) ((void *)(uintptr_t)(a))
struct file { void *private_data; };
struct inode { int unused; };
typedef int poll_table;
struct completion { atomic_int done; };
typedef struct { struct completion comp; unsigned int timeoutValue; } OSAL_SIGNAL, *P_OSAL_SIGNAL;
typedef struct { int waitQueue; } OSAL_EVENT, *P_OSAL_EVENT;
/* SOURCE_UAPI */
/* SOURCE_TYPES */
typedef struct {
    unsigned long state;
    UINT8 cCmd[NAME_MAX + 1];
    INT32 cmdResult;
    OSAL_SIGNAL cmdResp;
    OSAL_EVENT cmdReq;
    UINT32 patchNum, ip_ver, fw_ver;
    P_WMT_PATCH_INFO pWmtPatchInfo;
    struct wmt_rom_patch_info *pWmtRomPatchInfo[WMTDRV_TYPE_ANT];
} DEV_WMT, *P_DEV_WMT;
typedef struct { SIZE_T au4CtrlData[8]; } WMT_CTRL_DATA, *P_WMT_CTRL_DATA;
static DEV_WMT gDevWmt;
static int gWmtInitStatus = WMT_INIT_DONE, gWmtInitWq, gWmtClose;
static atomic_int gWmtRefCnt;
#define wait_event_timeout(q, c, t) ((void)(q), (c) ? (long)(t) : 0L)
#define atomic_inc_return(a) (atomic_fetch_add((a), 1) + 1)
#define atomic_dec_return(a) (atomic_fetch_sub((a), 1) - 1)
static int launcher_kill, dbg_mode_result, collector_result, collector_calls;
static UINT32 wmt_lib_get_icinfo(int kind) { return kind ? 0x8a00 : 0x6755; }
static int wmt_detect_get_chip_type(void) { return 1; }
static void wmt_lib_set_stp_wmt_last_close(UINT32 value) { launcher_kill = value; }
static int wmt_plat_set_dbg_mode(ULONG value) { (void)value; return dbg_mode_result; }
static int wmt_dbg_fwinfor_from_emi(int a, int b, int c)
{ CHECK(a == 0 && b == 1 && c == 0); collector_calls++; return collector_result; }
/* SOURCE_STATE */
/* SOURCE_IOCTLS */
/* SOURCE_PROTOTYPES */

static atomic_int copy_from_calls, copy_to_calls, event_calls;
static atomic_int fail_from, fail_to, copy_from_action, copy_to_action;
static atomic_int allocation_calls, fail_allocation, allocation_cancel_at;
static pthread_mutex_t allocation_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *pointer; size_t size; bool live; } allocations[4096];
static size_t allocation_records, live_allocations;
static pthread_mutex_t *deadline_lock;
static atomic_int deadline_lock_once;
static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t signal_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_entered, gate_released;
static atomic_int consumer_copy_pause;
static atomic_int watch_cache_lock, cache_lock_entered;
static void on_wait(void);

static void deadline_advance(void) { atomic_fetch_add(&clock_ticks, 6001); }
static struct timespec deadline_seconds(int seconds)
{
    struct timespec deadline;
    CHECK(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += seconds;
    return deadline;
}
static void pause_at_gate(void)
{
    struct timespec deadline = deadline_seconds(5);
    CHECK(!pthread_mutex_lock(&gate_lock));
    gate_entered++;
    CHECK(!pthread_cond_broadcast(&gate_cv));
    while (!gate_released)
        CHECK(!pthread_cond_timedwait(&gate_cv, &gate_lock, &deadline));
    CHECK(!pthread_mutex_unlock(&gate_lock));
}
static void wait_for_gate(void)
{
    struct timespec deadline = deadline_seconds(5);
    CHECK(!pthread_mutex_lock(&gate_lock));
    while (!gate_entered)
        CHECK(!pthread_cond_timedwait(&gate_cv, &gate_lock, &deadline));
    CHECK(!pthread_mutex_unlock(&gate_lock));
}
static void release_gate(void)
{
    CHECK(!pthread_mutex_lock(&gate_lock));
    gate_released = 1;
    CHECK(!pthread_cond_broadcast(&gate_cv));
    CHECK(!pthread_mutex_unlock(&gate_lock));
}
static void test_mutex_lock(pthread_mutex_t *mutex)
{
    if (mutex == deadline_lock && atomic_exchange(&deadline_lock_once, 0))
        deadline_advance();
    if (mutex == &g_patch_info_lock && atomic_exchange(&watch_cache_lock, 0)) {
        CHECK(!pthread_mutex_lock(&gate_lock));
        atomic_store(&cache_lock_entered, 1);
        CHECK(!pthread_cond_broadcast(&gate_cv));
        CHECK(!pthread_mutex_unlock(&gate_lock));
    }
    CHECK(!pthread_mutex_lock(mutex));
}
#define mutex_lock test_mutex_lock
#define mutex_unlock(m) CHECK(!pthread_mutex_unlock(m))
static int osal_test_bit(unsigned int bit, const unsigned long *state)
{ return (__atomic_load_n(state, __ATOMIC_SEQ_CST) >> bit) & 1; }
static void osal_set_bit(unsigned int bit, unsigned long *state)
{ __atomic_fetch_or(state, BIT(bit), __ATOMIC_SEQ_CST); }
static void osal_clear_bit(unsigned int bit, unsigned long *state)
{ __atomic_fetch_and(state, ~BIT(bit), __ATOMIC_SEQ_CST); }
static void *test_allocate(size_t count, size_t size)
{
    int call = atomic_fetch_add(&allocation_calls, 1) + 1;
    if (atomic_load(&fail_allocation) == call) return NULL;
    void *memory = calloc(count, size);
    CHECK(memory != NULL);
    CHECK(!pthread_mutex_lock(&allocation_lock));
    CHECK(allocation_records < sizeof(allocations) / sizeof(allocations[0]));
    allocations[allocation_records++] = (typeof(allocations[0])){memory, count * size, true};
    live_allocations++;
    CHECK(!pthread_mutex_unlock(&allocation_lock));
    if (atomic_load(&allocation_cancel_at) == call) wmt_lib_cancel_cmd();
    return memory;
}
static void kfree(void *memory)
{
    if (!memory) return;
    CHECK(!IS_ERR(memory));
    CHECK(!pthread_mutex_lock(&allocation_lock));
    size_t i = allocation_records;
    while (i && (allocations[i - 1].pointer != memory || !allocations[i - 1].live)) i--;
    CHECK(i != 0);
    allocations[i - 1].live = false;
    CHECK(live_allocations > 0);
    live_allocations--;
    CHECK(!pthread_mutex_unlock(&allocation_lock));
    free(memory);
}
#define kcalloc(n, s, f) test_allocate((n), (s))
#define kzalloc(n, f) test_allocate(1, (n))
#define kmalloc_track_caller(n, f) test_allocate(1, (n))
static void allocation_fail_after(int ordinal)
{ atomic_store(&fail_allocation, atomic_load(&allocation_calls) + ordinal); }
static unsigned long copy_from_user(void *dest, const void *source, size_t count)
{
    atomic_fetch_add(&copy_from_calls, 1);
    if (!source) return count;
    if (atomic_load(&fail_from)) {
        memcpy(dest, source, count / 2);
        return count - count / 2;
    }
    memcpy(dest, source, count);
    int action = atomic_exchange(&copy_from_action, 0);
    if (action == 1) deadline_advance();
    else if (action == 2) wmt_lib_cancel_cmd();
    else if (action == 3) memset((void *)source, 0xff, count);
    else if (action == 4) pause_at_gate();
    return 0;
}
static unsigned long copy_to_user(void *dest, const void *source, size_t count)
{
    atomic_fetch_add(&copy_to_calls, 1);
    if (!dest) return count;
    if (atomic_load(&fail_to)) {
        memcpy(dest, source, count / 2);
        return count - count / 2;
    }
    memcpy(dest, source, count);
    if (atomic_exchange(&copy_to_action, 0)) deadline_advance();
    return 0;
}
static void *test_data_copy(void *dest, const void *source, size_t count)
{
    if (atomic_exchange(&consumer_copy_pause, 0)) pause_at_gate();
    return memcpy(dest, source, count);
}
#define osal_memcpy test_data_copy
static int osal_signal_init(P_OSAL_SIGNAL signal)
{
    CHECK(!osal_test_bit(WMT_STAT_CMD, &gDevWmt.state));
    atomic_store(&signal->comp.done, 0);
    return 0;
}
static void osal_raise_signal(P_OSAL_SIGNAL signal)
{
    CHECK(!pthread_mutex_lock(&signal_lock));
    atomic_fetch_add(&signal->comp.done, 1);
    CHECK(!pthread_cond_broadcast(&signal_cv));
    CHECK(!pthread_mutex_unlock(&signal_lock));
}
static int osal_trigger_event(P_OSAL_EVENT event)
{
    CHECK(event == &gDevWmt.cmdReq);
    CHECK(!pthread_mutex_lock(&event_lock));
    atomic_fetch_add(&event_calls, 1);
    CHECK(!pthread_cond_broadcast(&event_cv));
    CHECK(!pthread_mutex_unlock(&event_lock));
    return 0;
}
static unsigned long wait_for_completion_timeout(struct completion *completion, unsigned long timeout)
{
    if (scenario < 100) {
        on_wait();
        if (!atomic_load(&completion->done)) {
            atomic_fetch_add(&clock_ticks, timeout);
            return 0;
        }
    }
    struct timespec deadline = deadline_seconds(5);
    CHECK(!pthread_mutex_lock(&signal_lock));
    while (!atomic_load(&completion->done))
        CHECK(!pthread_cond_timedwait(&signal_cv, &signal_lock, &deadline));
    atomic_fetch_sub(&completion->done, 1);
    CHECK(!pthread_mutex_unlock(&signal_lock));
    return 1;
}
static void poll_wait(struct file *file, int *queue, poll_table *wait)
{ CHECK(queue == &gDevWmt.cmdReq.waitQueue); (void)file; (void)wait; }

/* SOURCE_FUNCTIONS */

struct packet {
    struct wmt_cmd2_header header;
    unsigned char body[WMT_CMD2_WRITE_MAX - sizeof(struct wmt_cmd2_header) + 1];
};
static struct file open_a, open_b;
static struct inode inode;
static struct packet saved_request;
static int expected_result, checks;
static const char *current_command = "baud_115200";
static u64 session_a;

static struct wmt_cmd2_session session_argument(u32 action, u64 id)
{
    return (struct wmt_cmd2_session){
        .version = cpu_to_le32(WMT_CMD2_VERSION), .action = cpu_to_le32(action),
        .session_id = cpu_to_le64(id)};
}
static LONG session_call(struct file *file, struct wmt_cmd2_session *session, bool compat)
{
    return compat ? WMT_compat_ioctl(file, WMT_IOCTL_CMD2_SESSION, (ULONG)session) :
                    WMT_unlocked_ioctl(file, WMT_IOCTL_CMD2_SESSION, (ULONG)session);
}
static u64 bind_file(struct file *file)
{
    struct wmt_cmd2_session session = session_argument(WMT_CMD2_BIND, 0);
    CHECK(session_call(file, &session, false) == 0);
    CHECK(le32_to_cpu(session.max_read_bytes) == WMT_CMD2_READ_MAX);
    CHECK(le32_to_cpu(session.max_write_bytes) == WMT_CMD2_WRITE_MAX);
    CHECK(!session.flags && !session.reserved);
    CHECK(le64_to_cpu(session.session_id) != 0);
    return le64_to_cpu(session.session_id);
}
static LONG unbind_file(struct file *file, u64 session_id)
{
    struct wmt_cmd2_session session = session_argument(WMT_CMD2_UNBIND, session_id);
    return session_call(file, &session, false);
}
static ssize_t write_packet(struct file *file, struct packet *packet)
{
    size_t length = sizeof(packet->header) + le32_to_cpu(packet->header.payload_len);
    return WMT_write(file, (char *)packet, length, NULL);
}
static struct packet receive_command(struct file *file, const char *command)
{
    struct packet packet = {0};
    ssize_t result = WMT_read(file, (char *)&packet, WMT_CMD2_READ_MAX, NULL);
    CHECK(result == (ssize_t)(sizeof(packet.header) + strlen(command)));
    CHECK(le32_to_cpu(packet.header.magic) == WMT_CMD2_MAGIC);
    CHECK(le16_to_cpu(packet.header.version) == WMT_CMD2_VERSION);
    CHECK(le16_to_cpu(packet.header.kind) == WMT_CMD2_COMMAND);
    CHECK(le64_to_cpu(packet.header.session_id) != 0);
    CHECK(le64_to_cpu(packet.header.transaction_id) != 0);
    CHECK(le32_to_cpu(packet.header.payload_len) == strlen(command));
    CHECK(packet.header.result == 0);
    CHECK(!memcmp(packet.body, command, strlen(command)));
    return packet;
}
static struct packet status_reply(struct packet request, int result)
{
    request.header.kind = cpu_to_le16(WMT_CMD2_STATUS);
    request.header.payload_len = 0;
    request.header.result = cpu_to_le32((u32)result);
    return request;
}
static struct packet list_reply(struct packet request, bool normal, unsigned count, const char *prefix)
{
    struct wmt_cmd2_list *list = (void *)request.body;
    CHECK(count <= WMT_CMD2_PATCH_MAX);
    memset(request.body, 0, sizeof(request.body));
    request.header.kind = cpu_to_le16(normal ? WMT_CMD2_PATCH_LIST : WMT_CMD2_ROM_LIST);
    request.header.result = 0;
    request.header.payload_len = cpu_to_le32(sizeof(*list) + count * sizeof(list->record[0]));
    list->count = cpu_to_le32(count);
    for (unsigned i = 0; i < count; i++) {
        list->record[i].index = cpu_to_le32(normal ? count - i : i);
        memset(list->record[i].address, (normal ? count - i : i) + 0x10, 4);
        snprintf((char *)list->record[i].name, sizeof(list->record[i].name), "%s%u.bin",
                 prefix, normal ? count - i : i);
    }
    return request;
}
static void expect_write(struct file *file, struct packet *packet)
{
    CHECK(write_packet(file, packet) ==
          (ssize_t)(sizeof(packet->header) + le32_to_cpu(packet->header.payload_len)));
    checks++;
}
struct cache_snapshot {
    UINT32 count, rom_mask;
    WMT_PATCH_INFO patch[WMT_CMD2_PATCH_MAX];
    struct wmt_rom_patch_info rom[WMT_CMD2_ROM_MAX];
};
static struct cache_snapshot snapshot_cache(void)
{
    struct cache_snapshot snapshot = {0};
    mutex_lock(&g_patch_info_lock);
    snapshot.count = pAtchNum;
    if (pAtchNum) memcpy(snapshot.patch, pPatchInfo, pAtchNum * sizeof(*pPatchInfo));
    mutex_unlock(&g_patch_info_lock);
    mutex_lock(&g_rom_patch_info_lock);
    for (unsigned i = 0; i < WMT_CMD2_ROM_MAX; i++) {
        if (gDevWmt.pWmtRomPatchInfo[i]) {
            snapshot.rom_mask |= BIT(i);
            snapshot.rom[i] = *gDevWmt.pWmtRomPatchInfo[i];
        }
    }
    mutex_unlock(&g_rom_patch_info_lock);
    return snapshot;
}
static void expect_cache_unchanged(struct cache_snapshot *before)
{
    struct cache_snapshot after = snapshot_cache();
    CHECK(!memcmp(before, &after, sizeof(after)));
}
static void reject_write(struct file *file, struct packet *packet, int error)
{
    struct cache_snapshot before = snapshot_cache();
    CHECK(write_packet(file, packet) == error);
    CHECK(g_wmt_cmd_in_progress && !g_wmt_cmd_responded);
    expect_cache_unchanged(&before);
    checks++;
}
static void check_patch(unsigned count, const char *prefix)
{
    WMT_CTRL_DATA control = {0};
    unsigned char name[256], address[4];
    CHECK(wmt_ctrl_get_patch_num(&control) == 0 && control.au4CtrlData[0] == count);
    CHECK((wmt_lib_get_patch_info() != NULL) == (count != 0));
    for (unsigned i = 1; i <= count; i++) {
        char expected[256];
        control.au4CtrlData[0] = i;
        control.au4CtrlData[1] = (SIZE_T)name;
        control.au4CtrlData[2] = (SIZE_T)address;
        CHECK(wmt_ctrl_get_patch_info(&control) == 0);
        snprintf(expected, sizeof(expected), "%s%u.bin", prefix, i);
        CHECK(!strcmp((char *)name, expected));
        for (int j = 0; j < 4; j++) CHECK(address[j] == i + 0x10);
    }
}
static void check_rom(unsigned type, const char *prefix)
{
    unsigned char name[256], address[4];
    char expected[256];
    int result = wmt_lib_get_rom_patch_info(type, name, address);
    if (!prefix) { CHECK(result == -ENOENT); return; }
    CHECK(result == 0);
    snprintf(expected, sizeof(expected), "%s%u.bin", prefix, type);
    CHECK(!strcmp((char *)name, expected));
    for (int i = 0; i < 4; i++) CHECK(address[i] == type + 0x10);
}

static void legacy_setters_rejected(void)
{
    const UINT32 native[] = {WMT_IOCTL_SET_PATCH_NAME, WMT_IOCTL_SET_PATCH_NUM,
        WMT_IOCTL_SET_PATCH_INFO, WMT_IOCTL_SET_ROM_PATCH_INFO};
    const UINT32 compat[] = {COMPAT_WMT_IOCTL_SET_PATCH_NAME, WMT_IOCTL_SET_PATCH_NUM,
        COMPAT_WMT_IOCTL_SET_PATCH_INFO, COMPAT_WMT_IOCTL_SET_ROM_PATCH_INFO};
    struct file *files[] = {&open_a, &open_b};
    int copies = atomic_load(&copy_from_calls);
    for (unsigned f = 0; f < 2; f++) {
        for (unsigned i = 0; i < 4; i++) {
            CHECK(WMT_unlocked_ioctl(files[f], native[i], 0) == -EOPNOTSUPP);
            CHECK(WMT_compat_ioctl(files[f], compat[i], 0) == -EOPNOTSUPP);
            CHECK(WMT_compat_ioctl(files[f], native[i], 0) == -EOPNOTSUPP);
            checks += 3;
        }
    }
    CHECK(atomic_load(&copy_from_calls) == copies);
}
static void malformed_frames(struct packet good)
{
    for (int variant = 0; variant < 10; variant++) {
        struct packet bad = good;
        switch (variant) {
        case 0: bad.header.magic ^= cpu_to_le32(1); break;
        case 1: bad.header.version = cpu_to_le16(1); break;
        case 2: bad.header.kind = cpu_to_le16(55); break;
        case 3: bad.header.kind = cpu_to_le16(WMT_CMD2_COMMAND); break;
        case 4: bad.header.session_id = 0; break;
        case 5: bad.header.transaction_id = 0; break;
        case 6: bad.header.result = cpu_to_le32(1); break;
        case 7: bad.header.payload_len = cpu_to_le32(1); break;
        case 8: bad.header.kind = cpu_to_le16(WMT_CMD2_PATCH_LIST); break;
        case 9: bad.header.kind = cpu_to_le16(WMT_CMD2_ROM_LIST); break;
        }
        reject_write(&open_a, &bad, -EINVAL);
    }
    struct packet bad = good;
    bad.header.payload_len = cpu_to_le32(100);
    CHECK(WMT_write(&open_a, (char *)&bad, sizeof(bad.header), NULL) == -EINVAL);
    CHECK(WMT_write(&open_a, (char *)&good, sizeof(good.header) + 1, NULL) == -EINVAL);
}
static void malformed_lists(struct packet good, bool normal)
{
    for (int variant = 0; variant < (normal ? 11 : 10); variant++) {
        struct packet bad = good;
        struct wmt_cmd2_list *list = (void *)bad.body;
        switch (variant) {
        case 0: list->count = cpu_to_le32(normal ? 11 : 6); break;
        case 1: list->record[1].index = list->record[0].index; break;
        case 2: list->record[0].index = cpu_to_le32(normal ? 0 : 5); break;
        case 3: list->record[0].index = cpu_to_le32(normal ? 3 : UINT32_MAX); break;
        case 4: list->record[0].name[0] = 0; break;
        case 5: memset(list->record[0].name, 'x', sizeof(list->record[0].name)); break;
        case 6: list->reserved = cpu_to_le32(1); break;
        case 7: bad.header.payload_len = cpu_to_le32(le32_to_cpu(bad.header.payload_len) - 1); break;
        case 8: bad.header.payload_len = cpu_to_le32(le32_to_cpu(bad.header.payload_len) + 1); break;
        case 9: bad.header.result = cpu_to_le32((u32)-EIO); break;
        case 10: list->count = 0; bad.header.payload_len = cpu_to_le32(sizeof(*list)); break;
        }
        reject_write(&open_a, &bad, -EINVAL);
    }
}
static void on_wait(void)
{
    struct file *owner = &open_a;
    char buffer[WMT_CMD2_READ_MAX];
    if ((scenario == 11 || scenario == 14) && phase) owner = &open_b;
    if (scenario == 66 && !phase) {
        CHECK(!g_wmt_cmd_owner && !g_wmt_cmd_session_id);
        expected_result = -ETIMEDOUT;
        return;
    }
    if (scenario == 67) {
        CHECK(WMT_poll(owner, NULL) == (POLLIN | POLLRDNORM));
        deadline_advance();
        CHECK(WMT_read(owner, buffer, sizeof(buffer), NULL) == -EAGAIN);
        CHECK(((struct wmt_cmd_file *)owner->private_data)->session_id == session_a);
        expected_result = -ETIMEDOUT;
        return;
    }

    if ((scenario == 4 || scenario == 54 || scenario == 56) && !phase) {
        CHECK(WMT_read(&open_a, buffer, sizeof(buffer), NULL) == -ENOTCONN);
        CHECK(g_wmt_cmd_session_id == 0 && g_wmt_cmd_transaction_id == 0);
        if (scenario == 56) {
            struct wmt_cmd2_session session = session_argument(WMT_CMD2_BIND, 0);
            atomic_store(&fail_to, 1);
            CHECK(session_call(&open_a, &session, false) == -EFAULT);
            atomic_store(&fail_to, 0);
            CHECK(g_wmt_cmd_owner == NULL && g_wmt_cmd_request_owner == NULL);
            CHECK(!g_wmt_cmd_responded);
        }
        if (scenario == 54) atomic_store(&copy_to_action, 1);
        session_a = bind_file(&open_a);
        if (scenario == 54) {
            CHECK(WMT_read(&open_a, buffer, sizeof(buffer), NULL) == -EAGAIN);
            expected_result = -ETIMEDOUT;
            return;
        }
    }
    if (scenario == 16) {
        CHECK(WMT_read(owner, buffer, 1, NULL) == -EMSGSIZE);
        atomic_store(&fail_to, 1);
        CHECK(WMT_read(owner, buffer, sizeof(buffer), NULL) == -EFAULT);
        atomic_store(&fail_to, 0);
        CHECK(osal_test_bit(WMT_STAT_CMD, &gDevWmt.state));
        CHECK(!g_wmt_cmd_delivered);
    }
    if (scenario == 21) {
        CHECK(WMT_read(owner, buffer, 0, NULL) == 0);
        CHECK(WMT_write(owner, buffer, 0, NULL) == 0);
        CHECK(!g_wmt_cmd_delivered && !g_wmt_cmd_responded);
    }
    if (scenario == 27) {
        struct packet early = {0};
        early.header.magic = cpu_to_le32(WMT_CMD2_MAGIC);
        early.header.version = cpu_to_le16(WMT_CMD2_VERSION);
        early.header.kind = cpu_to_le16(WMT_CMD2_STATUS);
        early.header.session_id = cpu_to_le64(g_wmt_cmd_session_id);
        early.header.transaction_id = cpu_to_le64(g_wmt_cmd_transaction_id);
        reject_write(owner, &early, -ESTALE);
    }
    if (scenario == 35) {
        atomic_store(&copy_to_action, 1);
        CHECK(WMT_read(owner, buffer, sizeof(buffer), NULL) == -ETIMEDOUT);
        CHECK(!g_wmt_cmd_delivered && g_wmt_cmd_responded);
        expected_result = -ETIMEDOUT;
        return;
    }
    if (scenario == 60) {
        CHECK(unbind_file(owner, session_a) == 0);
        CHECK(WMT_read(owner, buffer, sizeof(buffer), NULL) == -ENOTCONN);
        expected_result = -ECONNRESET;
        return;
    }
    if (scenario == 57) {
        CHECK(WMT_poll(owner, NULL) == (POLLIN | POLLRDNORM));
        CHECK(WMT_poll(&open_b, NULL) == (POLLERR | POLLHUP));
    }
    struct packet request = receive_command(owner, current_command);
    struct packet reply = status_reply(request, 0);
    if (scenario == 57) {
        CHECK(WMT_poll(owner, NULL) == (POLLOUT | POLLWRNORM));
        CHECK(WMT_read(owner, buffer, sizeof(buffer), NULL) == -EAGAIN);
    }
    if (scenario >= 11 && scenario <= 15) {
        if (!phase) {
            saved_request = request;
            if (scenario == 13) {
                reply = list_reply(request, true, 2, "old");
                expect_write(owner, &reply);
            } else if (scenario == 14 || scenario == 15) {
                wmt_lib_cancel_cmd(); expected_result = -ECANCELED;
            } else expected_result = -ETIMEDOUT;
            return;
        }
        struct packet stale = list_reply(saved_request, true, 2, "stale");
        reject_write(&open_a, &stale, -ESTALE);
        reply = status_reply(request, -EOPNOTSUPP);
        expected_result = -EOPNOTSUPP;
        expect_write(owner, &reply);
        return;
    }
    if (scenario == 9 || scenario == 10 || scenario == 59) {
        if (!phase) {
            saved_request = request;
            if (scenario == 59) {
                wmt_lib_cmd_shutdown(); expected_result = -ESHUTDOWN;
            } else {
                CHECK(unbind_file(owner, session_a) == 0); expected_result = -ECONNRESET;
            }
            return;
        }
        if (scenario == 10) {
            CHECK(unbind_file(owner, le64_to_cpu(saved_request.header.session_id)) == -ESTALE);
            CHECK(!g_wmt_cmd_responded);
        }
        struct packet stale = status_reply(saved_request, 0);
        reject_write(owner, &stale, -ESTALE);
    }
    if (scenario == 8) {
        CHECK(WMT_close(&inode, owner) == 0); expected_result = -ECONNRESET; return;
    }
    if (scenario == 33) {
        wmt_lib_cmd_shutdown();
        CHECK(WMT_poll(owner, NULL) == (POLLERR | POLLHUP));
        CHECK(write_packet(owner, &reply) == -ESTALE);
        expected_result = -ESHUTDOWN; return;
    }
    if (scenario == 7) { CHECK(WMT_close(&inode, &open_b) == 0); CHECK(!g_wmt_cmd_responded); }
    if (scenario == 68) {
        atomic_store(&fail_to, 1);
        CHECK(unbind_file(owner, session_a) == -EFAULT);
        atomic_store(&fail_to, 0);
        CHECK(g_wmt_cmd_owner == owner->private_data && !g_wmt_cmd_responded);
    }
    if (scenario == 1 || scenario == 2) {
        reply = status_reply(request, scenario == 1 ? -EIO : -EOPNOTSUPP);
        expected_result = scenario == 1 ? -EIO : -EOPNOTSUPP;
    }
    if (scenario == 17) {
        atomic_store(&fail_from, 1);
        reject_write(owner, &reply, -EFAULT);
        atomic_store(&fail_from, 0);
    }
    if (scenario == 22) {
        CHECK(WMT_write(owner, (char *)&reply, 1, NULL) == -EMSGSIZE);
        CHECK(WMT_write(owner, (char *)&reply, 31, NULL) == -EMSGSIZE);
        CHECK(WMT_write(owner, (char *)&reply, WMT_CMD2_WRITE_MAX + 1, NULL) == -EMSGSIZE);
    }
    if (scenario == 23) malformed_frames(reply);
    if (scenario == 24 || scenario == 25 || scenario == 26) {
        struct packet wrong = reply;
        if (scenario == 24) wrong.header.session_id ^= cpu_to_le64(0x100);
        if (scenario == 25) wrong.header.transaction_id ^= cpu_to_le64(0x100);
        reject_write(scenario == 26 ? &open_b : owner, &wrong, -ESTALE);
    }
    if (scenario == 29) CHECK(wmt_lib_send_cmd((PUINT8)"nested") == -EBUSY);
    if (scenario == 53) legacy_setters_rejected();
    if (!strcmp(current_command, "srh_patch")) {
        if (scenario == 3) reject_write(owner, &reply, -EINVAL);
        reply = list_reply(request, true, scenario == 39 ? 10 : 2,
                           scenario == 42 && !phase ? "old" : "new");
    } else if (!strcmp(current_command, "srh_rom_patch")) {
        unsigned count = scenario == 43 || scenario == 47 ? 5 : 2;
        if (scenario == 44 || (scenario == 52 && phase == 0)) count = 0;
        if (scenario == 46 && phase) count = 5;
        if (scenario == 47 && !phase) count = 2;
        if (scenario == 52) count = phase == 0 ? 0 : phase == 1 ? 4 : 5;
        reply = list_reply(request, false, count,
                           (scenario == 46 || scenario == 47) && !phase ? "old" : "new");
    }
    if (scenario == 40) malformed_lists(reply, true);
    if (scenario == 45) malformed_lists(reply, false);
    if (scenario == 41 || (scenario == 42 && phase)) {
        allocation_fail_after(scenario == 41 ? 1 : 2);
        reject_write(owner, &reply, -ENOMEM);
        atomic_store(&fail_allocation, 0);
    }
    if (scenario == 47 && phase) {
        for (int i = 1; i <= WMT_CMD2_ROM_MAX; i++) {
            allocation_fail_after(1 + i);
            reject_write(owner, &reply, -ENOMEM);
            atomic_store(&fail_allocation, 0);
        }
    }
    if (scenario == 36 || scenario == 37 || scenario == 38 || scenario == 62) {
        struct cache_snapshot before = snapshot_cache();
        if (scenario == 36) atomic_store(&copy_from_action, 1);
        if (scenario == 37) atomic_store(&allocation_cancel_at, atomic_load(&allocation_calls) + 2);
        if (scenario == 38 || scenario == 62) {
            deadline_lock = scenario == 38 ? &g_patch_info_lock : &g_rom_patch_info_lock;
            atomic_store(&deadline_lock_once, 1);
        }
        CHECK(write_packet(owner, &reply) == ((scenario == 38 || scenario == 62) ? -ETIMEDOUT : -ESTALE));
        expect_cache_unchanged(&before);
        expected_result = scenario == 37 ? -ECANCELED : -ETIMEDOUT;
        return;
    }
    if (scenario == 48 || scenario == 50) {
        if (!phase) { saved_request = request; expected_result = -ETIMEDOUT; return; }
        struct packet stale = list_reply(saved_request, scenario == 48, scenario == 48 ? 2 : 5, "stale");
        reject_write(owner, &stale, -ESTALE);
        if (scenario == 50) reply = list_reply(request, false, 0, "new");
    }
    if (scenario == 55) {
        int copies = atomic_load(&copy_from_calls);
        size_t length = sizeof(reply.header) + le32_to_cpu(reply.header.payload_len);
        atomic_store(&copy_from_action, 3);
        CHECK(WMT_write(owner, (char *)&reply, length, NULL) == (ssize_t)length);
        CHECK(atomic_load(&copy_from_calls) == copies + 1);
        CHECK(reply.header.magic == UINT32_MAX);
        return;
    }
    if ((scenario == 63 || scenario == 64) && !phase) {
        /* Negative list frames cannot publish, and status errors carry no list. */
        struct packet bad = reply;
        bad.header.result = cpu_to_le32((u32)-ENOENT);
        reject_write(owner, &bad, -EINVAL);
        reply = status_reply(request, -ENOENT);
        expected_result = -ENOENT;
    }
    expect_write(owner, &reply);
    if (scenario == 48 && phase) {
        struct packet stale = list_reply(saved_request, true, 2, "stale");
        CHECK(write_packet(owner, &stale) == -ESTALE);
        check_patch(2, "new");
    }
    if (scenario == 28) {
        CHECK(write_packet(owner, &reply) == -ESTALE);
        CHECK(wmt_lib_send_cmd((PUINT8)"nested") == -EBUSY);
    }
    if (scenario == 49) { wmt_lib_cancel_cmd(); CHECK(gDevWmt.cmdResult == 0); }
}

struct producer_args { const char *command; int result; };
struct reader_args { struct packet packet; ssize_t result; };
struct writer_args { struct packet packet; _Atomic ssize_t result; };
struct getter_args { unsigned char name[256], address[4]; int result; };
static void *producer_thread(void *argument)
{
    struct producer_args *args = argument;
    args->result = wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)args->command);
    return NULL;
}
static void *reader_thread(void *argument)
{
    struct reader_args *args = argument;
    args->result = WMT_read(&open_a, (char *)&args->packet, WMT_CMD2_READ_MAX, NULL);
    return NULL;
}
static void *writer_thread(void *argument)
{
    struct writer_args *args = argument;
    atomic_store(&args->result, write_packet(&open_a, &args->packet));
    return NULL;
}
static void *getter_thread(void *argument)
{
    struct getter_args *args = argument;
    args->result = wmt_dev_get_patch_info(1, args->name, args->address);
    return NULL;
}
static pthread_t start_producer(struct producer_args *args)
{
    struct timespec deadline = deadline_seconds(5);
    pthread_t thread;
    int before = atomic_load(&event_calls);
    CHECK(!pthread_create(&thread, NULL, producer_thread, args));
    CHECK(!pthread_mutex_lock(&event_lock));
    while (atomic_load(&event_calls) == before)
        CHECK(!pthread_cond_timedwait(&event_cv, &event_lock, &deadline));
    CHECK(!pthread_mutex_unlock(&event_lock));
    return thread;
}
static void join_thread(pthread_t thread) { CHECK(!pthread_join(thread, NULL)); }
static void run_thread_case(void)
{
    struct producer_args first = {scenario == 104 ? "srh_patch" : "first", -999};
    pthread_t producer = start_producer(&first);
    if (scenario == 100) {
        struct reader_args readers[2] = {0};
        pthread_t reader_threads[2];
        CHECK(!pthread_create(&reader_threads[0], NULL, reader_thread, &readers[0]));
        CHECK(!pthread_create(&reader_threads[1], NULL, reader_thread, &readers[1]));
        join_thread(reader_threads[0]); join_thread(reader_threads[1]);
        int winner = readers[0].result > 0 ? 0 : 1;
        CHECK(readers[winner].result == (ssize_t)(32 + strlen(first.command)));
        CHECK(readers[1 - winner].result == -EAGAIN);
        struct packet reply = status_reply(readers[winner].packet, 0);
        expect_write(&open_a, &reply);
        join_thread(producer); CHECK(first.result == 0); return;
    }
    struct packet request = receive_command(&open_a, first.command);
    struct packet reply = status_reply(request, 0);
    if (scenario == 102) {
        expect_write(&open_a, &reply);
        wmt_lib_cancel_cmd();
        join_thread(producer); CHECK(first.result == 0); return;
    }
    if (scenario == 101 || scenario == 103) {
        struct writer_args writer = {.packet = reply, .result = -999};
        pthread_t writer_id;
        atomic_store(&copy_from_action, 4);
        CHECK(!pthread_create(&writer_id, NULL, writer_thread, &writer));
        wait_for_gate();
        wmt_lib_cancel_cmd();
        join_thread(producer); CHECK(first.result == -ECANCELED);
        struct producer_args second = {"second", -999};
        pthread_t second_id = 0;
        struct packet next = {0};
        if (scenario == 101) {
            second_id = start_producer(&second);
            next = receive_command(&open_a, second.command);
            CHECK(le64_to_cpu(next.header.session_id) == le64_to_cpu(request.header.session_id));
            CHECK(le64_to_cpu(next.header.transaction_id) > le64_to_cpu(request.header.transaction_id));
        }
        release_gate();
        join_thread(writer_id); CHECK(atomic_load(&writer.result) == -ESTALE);
        if (scenario == 101) {
            next = status_reply(next, -EIO);
            expect_write(&open_a, &next);
            join_thread(second_id); CHECK(second.result == -EIO);
        }
        return;
    }
    CHECK(scenario == 104);
    reply = list_reply(request, true, 2, "old");
    expect_write(&open_a, &reply);
    join_thread(producer); CHECK(first.result == 0);
    struct getter_args getter = {0};
    pthread_t getter_id;
    atomic_store(&consumer_copy_pause, 1);
    CHECK(!pthread_create(&getter_id, NULL, getter_thread, &getter));
    wait_for_gate();
    struct producer_args second = {"srh_patch", -999};
    pthread_t second_id = start_producer(&second);
    request = receive_command(&open_a, second.command);
    struct writer_args writer = {.packet = list_reply(request, true, 2, "new"), .result = -999};
    pthread_t writer_id;
    atomic_store(&watch_cache_lock, 1);
    CHECK(!pthread_create(&writer_id, NULL, writer_thread, &writer));
    struct timespec deadline = deadline_seconds(5);
    CHECK(!pthread_mutex_lock(&gate_lock));
    while (!atomic_load(&cache_lock_entered))
        CHECK(!pthread_cond_timedwait(&gate_cv, &gate_lock, &deadline));
    CHECK(!pthread_mutex_unlock(&gate_lock));
    CHECK(atomic_load(&writer.result) == -999);
    release_gate();
    join_thread(getter_id); join_thread(writer_id); join_thread(second_id);
    CHECK(getter.result == 0 && !strcmp((char *)getter.name, "old1.bin"));
    for (int i = 0; i < 4; i++) CHECK(getter.address[i] == 0x11);
    CHECK(atomic_load(&writer.result) == (ssize_t)(32 + 8 + 2 * 264));
    CHECK(second.result == 0);
    check_patch(2, "new");
}
static void run_one(void)
{
    expected_result = 0;
    int result = wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)current_command);
    CHECK(result == expected_result);
    CHECK(!g_wmt_cmd_in_progress && !g_wmt_cmd_delivered && !g_wmt_cmd_responded);
    CHECK(!g_wmt_cmd_request_owner && !g_wmt_cmd_session_id && !g_wmt_cmd_transaction_id);
    checks++;
}
static void check_session_arguments(void)
{
    for (int variant = 0; variant < 7; variant++) {
        struct wmt_cmd2_session session = session_argument(WMT_CMD2_BIND, 0);
        switch (variant) {
        case 0: session.version = cpu_to_le32(1); break;
        case 1: session.action = cpu_to_le32(3); break;
        case 2: session.session_id = cpu_to_le64(1); break;
        case 3: session.max_read_bytes = cpu_to_le32(1); break;
        case 4: session.max_write_bytes = cpu_to_le32(1); break;
        case 5: session.flags = cpu_to_le32(1); break;
        case 6: session.reserved = cpu_to_le32(1); break;
        }
        CHECK(session_call(&open_a, &session, false) == -EINVAL);
        CHECK(g_wmt_cmd_owner == open_a.private_data);
        CHECK(((struct wmt_cmd_file *)open_a.private_data)->session_id == session_a);
    }
    CHECK(unbind_file(&open_a, 0) == -ESTALE);
    struct wmt_cmd2_session session = session_argument(WMT_CMD2_BIND, 0);
    CHECK(session_call(&open_a, &session, true) == 0);
    CHECK(le64_to_cpu(session.session_id) == session_a);
}
static void cleanup(void)
{
    if (open_a.private_data) CHECK(WMT_close(&inode, &open_a) == 0);
    if (open_b.private_data) CHECK(WMT_close(&inode, &open_b) == 0);
    CHECK(atomic_load(&gWmtRefCnt) == 0);
    wmt_lib_cmd_shutdown();
    wmt_dev_patch_info_free();
    wmt_lib_rom_patch_info_free();
    CHECK(live_allocations == 0);
    CHECK(!pAtchNum && !pPatchInfo && !g_patch_info_ready);
    CHECK(!gDevWmt.patchNum && !gDevWmt.pWmtPatchInfo);
    for (int i = 0; i < WMT_CMD2_ROM_MAX; i++) CHECK(!gDevWmt.pWmtRomPatchInfo[i]);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    scenario = atoi(argv[1]);
    int original_case = scenario;
    CHECK(sizeof(struct wmt_cmd2_header) == 32 && sizeof(struct wmt_cmd2_session) == 32);
    CHECK(sizeof(struct wmt_cmd2_record) == 264);
    CHECK(WMT_IOCTL_CMD2_SESSION == 0xc020a040U);
    CHECK(offsetof(struct wmt_cmd2_header, transaction_id) == 16);
    CHECK(offsetof(struct wmt_cmd2_session, max_read_bytes) == 16);
    if (scenario == 34) {
        wmt_lib_cmd_shutdown(); wmt_lib_cmd_shutdown();
        CHECK(atomic_load(&event_calls) == 0);
        CHECK(wmt_lib_send_cmd((PUINT8)"early") == -ESHUTDOWN);
    }
    wmt_lib_cmd_start();
    CHECK(WMT_open(&inode, &open_a) == 0);
    CHECK(WMT_open(&inode, &open_b) == 0);
    CHECK(atomic_load(&gWmtRefCnt) == 2);
    if (scenario == 53) legacy_setters_rejected();
    if (scenario == 18 || scenario == 19) {
        struct wmt_cmd2_session session = session_argument(WMT_CMD2_BIND, 0);
        atomic_store(scenario == 18 ? &fail_from : &fail_to, 1);
        CHECK(session_call(&open_a, &session, false) == -EFAULT);
        atomic_store(&fail_from, 0); atomic_store(&fail_to, 0);
        CHECK(!g_wmt_cmd_owner && !((struct wmt_cmd_file *)open_a.private_data)->session_id);
    }
    if (scenario != 4 && scenario != 54 && scenario != 56 && scenario != 66)
        session_a = bind_file(&open_a);
    if (scenario == 5) {
        u64 counter = wmt_cmd_session_next;
        CHECK(bind_file(&open_a) == session_a && wmt_cmd_session_next == counter);
    }
    if (scenario == 6) {
        struct wmt_cmd2_session session = session_argument(WMT_CMD2_BIND, 0);
        CHECK(session_call(&open_b, &session, false) == -EBUSY);
        CHECK(WMT_unlocked_ioctl(&open_b, WMT_IOCTL_GET_CHIP_INFO, 0) == 0x6755);
        CHECK(WMT_unlocked_ioctl(&open_b, WMT_IOCTL_SET_LAUNCHER_KILL, 1) == 0 && launcher_kill);
        CHECK(WMT_unlocked_ioctl(&open_b, WMT_IOCTL_SET_LAUNCHER_KILL, 0) == 0 && !launcher_kill);
    }
    if (scenario == 20) {
        atomic_store(&fail_to, 1);
        CHECK(unbind_file(&open_a, session_a) == -EFAULT);
        atomic_store(&fail_to, 0);
        CHECK(g_wmt_cmd_owner == open_a.private_data);
        CHECK(bind_file(&open_a) == session_a);
    }
    if (scenario == 30) {
        struct file extra = {0};
        allocation_fail_after(1);
        CHECK(WMT_open(&inode, &extra) == -ENOMEM && !extra.private_data);
        CHECK(atomic_load(&gWmtRefCnt) == 2);
        atomic_store(&fail_allocation, 0);
        CHECK(WMT_open(&inode, &extra) == 0);
        CHECK(WMT_close(&inode, &extra) == 0);
    }
    if (scenario == 31) {
        CHECK(unbind_file(&open_a, session_a) == 0);
        u64 counter = wmt_cmd_session_next;
        wmt_cmd_session_next = UINT64_MAX;
        struct wmt_cmd2_session session = session_argument(WMT_CMD2_BIND, 0);
        CHECK(session_call(&open_a, &session, false) == -EOVERFLOW);
        CHECK(!g_wmt_cmd_owner && !((struct wmt_cmd_file *)open_a.private_data)->session_id);
        CHECK(wmt_cmd_session_next == UINT64_MAX);
        wmt_cmd_session_next = counter; /* Boundary injection is test-only. */
        session_a = bind_file(&open_a);
    }
    if (scenario == 32) {
        ((struct wmt_cmd_file *)open_a.private_data)->next_transaction = UINT64_MAX;
        CHECK(wmt_lib_send_cmd((PUINT8)"overflow") == -EOVERFLOW);
        CHECK(!g_wmt_cmd_in_progress);
        CHECK(unbind_file(&open_a, session_a) == 0);
        session_a = bind_file(&open_a);
    }
    if (scenario == 58) check_session_arguments();
    char maximum_command[WMT_CMD2_COMMAND_MAX + 1];
    if (scenario == 65) {
        char unterminated[WMT_CMD2_COMMAND_MAX + 1];
        memset(unterminated, 'x', sizeof(unterminated));
        CHECK(wmt_lib_send_cmd(NULL) == -EINVAL);
        CHECK(wmt_lib_send_cmd((PUINT8)"") == -EINVAL);
        CHECK(wmt_lib_send_cmd((PUINT8)unterminated) == -EINVAL);
        memset(maximum_command, 'x', sizeof(maximum_command));
        maximum_command[WMT_CMD2_COMMAND_MAX] = 0;
        current_command = maximum_command;
    }
    if (scenario == 61) {
        collector_result = -EINTR;
        CHECK(WMT_unlocked_ioctl(&open_b, WMT_IOCTL_FW_DBGLOG_CTRL, 1) == -EINTR);
        CHECK(collector_calls == 1);
        dbg_mode_result = -EBUSY;
        CHECK(WMT_unlocked_ioctl(&open_b, WMT_IOCTL_FW_DBGLOG_CTRL, 1) == -EBUSY);
        CHECK(collector_calls == 1);
        dbg_mode_result = 0; collector_result = 0;
        CHECK(WMT_unlocked_ioctl(&open_b, WMT_IOCTL_FW_DBGLOG_CTRL, 1) == 0);
        CHECK(collector_calls == 2);
    }
    if (scenario == 2) current_command = "not_a_supported_command";
    if (scenario == 3 || (scenario >= 11 && scenario <= 15) ||
        (scenario >= 36 && scenario <= 42) || scenario == 48 || scenario == 49 ||
        scenario == 51 || scenario == 55 || scenario == 63) current_command = "srh_patch";
    if ((scenario >= 43 && scenario <= 47) || scenario == 50 || scenario == 52 || scenario == 62 || scenario == 64)
        current_command = "srh_rom_patch";
    if (scenario >= 100) run_thread_case();
    else if (scenario == 52) {
        unsigned char name[256] = {0}, address[4] = {0};
        WMT_CTRL_DATA control = {.au4CtrlData = {4, (SIZE_T)name, (SIZE_T)address, 0x6755, 0x8a00}};
        CHECK(wmt_ctrl_get_rom_patch_info(&control) == 1);
        phase = 1; control.au4CtrlData[0] = 1;
        CHECK(wmt_ctrl_get_rom_patch_info(&control) == 0 && !strcmp((char *)name, "new1.bin"));
        phase = 2; control.au4CtrlData[0] = 4;
        CHECK(wmt_ctrl_get_rom_patch_info(&control) == 0 && !strcmp((char *)name, "new4.bin"));
        int events = atomic_load(&event_calls);
        phase = 3;
        CHECK(wmt_ctrl_get_rom_patch_info(&control) == 0 && atomic_load(&event_calls) == events);
        CHECK(gDevWmt.ip_ver == 0x6755 && gDevWmt.fw_ver == 0x8a00);
    } else {
        run_one();
        if (scenario == 42 || scenario == 47 || scenario == 63 || scenario == 64 || scenario == 66) {
            if (scenario == 63) check_patch(0, "new");
            if (scenario == 64) for (unsigned i = 0; i < 5; i++) check_rom(i, NULL);
            if (scenario == 66) {
                session_a = bind_file(&open_a);
                char buffer[WMT_CMD2_READ_MAX];
                CHECK(WMT_read(&open_a, buffer, sizeof(buffer), NULL) == -EAGAIN);
            }
            phase = 1; run_one();
        }
        if (scenario == 9 || scenario == 10 || (scenario >= 11 && scenario <= 15) ||
            scenario == 48 || scenario == 50 || scenario == 59) {
            if (scenario == 9 || scenario == 10 || scenario == 59) {
                if (scenario == 59) wmt_lib_cmd_start();
                session_a = bind_file(&open_a);
                CHECK(session_a > le64_to_cpu(saved_request.header.session_id));
            }
            if (scenario == 11 || scenario == 14) {
                CHECK(unbind_file(&open_a, session_a) == 0);
                CHECK(bind_file(&open_b) > session_a);
            }
            if (scenario >= 11 && scenario <= 15) current_command = "update_patch_version";
            struct packet stale = status_reply(saved_request, 0);
            CHECK(write_packet(&open_a, &stale) == -ESTALE);
            phase = 1;
            run_one();
        }
        if (scenario == 46) {
            check_rom(0, "old"); check_rom(1, "old"); check_rom(4, NULL);
            phase = 1; run_one(); phase = 2; run_one();
            for (unsigned i = 0; i < 5; i++) check_rom(i, i < 2 ? "old" : "new");
        }
    }
    if (scenario == 39) check_patch(10, "new");
    if (scenario == 48 || scenario == 49 || scenario == 51 || scenario == 55) check_patch(2, "new");
    if (scenario == 43) for (unsigned i = 0; i < 5; i++) check_rom(i, "new");
    if (scenario == 47) for (unsigned i = 0; i < 5; i++) check_rom(i, i < 2 ? "old" : "new");
    if (scenario == 44 || scenario == 50) for (unsigned i = 0; i < 5; i++) check_rom(i, NULL);
    if (scenario == 51) {
        unsigned char name[256], address[4];
        CHECK(wmt_dev_get_patch_info(0, name, address) == -EINVAL);
        CHECK(wmt_dev_get_patch_info(3, name, address) == -EINVAL);
        CHECK(wmt_dev_get_patch_info(SIZE_MAX, name, address) == -EINVAL);
        CHECK(wmt_dev_get_patch_info(1, NULL, address) == -EINVAL);
        CHECK(wmt_ctrl_get_patch_num(NULL) == -EINVAL && wmt_ctrl_get_patch_info(NULL) == -EINVAL);
        CHECK(wmt_dev_get_patch_info(1, name, address) == 0);
        memset(name, 'x', sizeof(name));
        check_patch(2, "new");
    }
    cleanup();
    /* Every scenario ends with a fresh successful start/bind/request/cleanup. */
    scenario = 0; phase = 0; current_command = "retry";
    atomic_store(&fail_allocation, 0); atomic_store(&allocation_cancel_at, 0);
    atomic_store(&copy_from_action, 0); atomic_store(&copy_to_action, 0);
    deadline_lock = NULL; atomic_store(&deadline_lock_once, 0);
    u64 prior_session = wmt_cmd_session_next;
    wmt_lib_cmd_start();
    CHECK(WMT_open(&inode, &open_a) == 0);
    CHECK(bind_file(&open_a) > prior_session);
    run_one();
    cleanup();
    printf("{\"case\":%d,\"checks\":%d,\"live_allocations\":%zu,\"sessions_issued\":%llu}\n",
           original_case, checks, live_allocations, (unsigned long long)wmt_cmd_session_next);
    return 0;
}
