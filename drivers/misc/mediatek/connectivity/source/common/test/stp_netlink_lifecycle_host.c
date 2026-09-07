/* Complete production netlink functions are inserted by the runner. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

typedef int INT32;
typedef uint32_t UINT32;
typedef uint8_t UINT8;
typedef char *PINT8;
typedef void *PVOID;
typedef void VOID;
typedef void *P_WCN_CORE_DUMP_T;
struct mutex { pthread_mutex_t native; bool initialized; };
typedef struct { struct mutex lock; } OSAL_SLEEPABLE_LOCK;
#define __MUTEX_INITIALIZER(name) { .native = PTHREAD_MUTEX_INITIALIZER, .initialized = true }
#define DEFINE_MUTEX(name) struct mutex name = __MUTEX_INITIALIZER(name)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define GFP_KERNEL 0
#define NLA_NUL_STRING 1
#define GENL_ID_GENERATE 0
#define STP_DBG_FAMILY_NAME "STP_DBG"
#define STP_DBG_ATTR_MAX 1
#define STP_DBG_PR_ERR(...) ((void)0)
#define STP_DBG_PR_INFO(...) ((void)0)
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

struct sk_buff { int unused; };
struct nlattr { char text[2]; };
struct nla_policy { int type; };
struct genl_info { struct nlattr *attrs[2]; pid_t snd_portid; };
struct genl_ops {
    int cmd, flags;
    struct nla_policy *policy;
    INT32 (*doit)(struct sk_buff *, struct genl_info *);
    void *dumpit;
};
struct genl_family {
    int id, hdrsize, version, maxattr;
    const char *name;
    struct genl_ops *ops;
    unsigned int n_ops;
};
static INT32 stp_dbg_nl_bind(struct sk_buff *, struct genl_info *);
static INT32 stp_dbg_nl_reset(struct sk_buff *skb, struct genl_info *info) { return 0; }

/* SOURCE_STATE */

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t family_gate = PTHREAD_RWLOCK_INITIALIZER;
static bool registered, pause_send, send_entered, release_send, unicast_running;
static bool close_lock_attempt, close_done, unregister_entered, unregister_during_send;
static bool bind_admitted, release_bind;
static struct mutex *close_wait_lock;
static unsigned int registrations, unregisters, late_lock_uses, sends, dump_calls;
static bool table_destroyed, fail_header, fail_attribute, fail_allocation;
static int core_result, unicast_result;
static pid_t refused_port, delivered[8];
static unsigned int delivered_count;
static _Thread_local bool closing_thread;
static const int init_net;

static void mutex_lock(struct mutex *lock)
{
    REQUIRE(lock->initialized);
    if (closing_thread) {
        REQUIRE(pthread_mutex_lock(&gate) == 0);
        if (!close_lock_attempt) {
            close_wait_lock = lock;
            close_lock_attempt = true;
            REQUIRE(pthread_cond_broadcast(&changed) == 0);
        }
        REQUIRE(pthread_mutex_unlock(&gate) == 0);
    }
    REQUIRE(pthread_mutex_lock(&lock->native) == 0);
}

static void mutex_unlock(struct mutex *lock) { REQUIRE(pthread_mutex_unlock(&lock->native) == 0); }

static int osal_sleepable_lock_init(OSAL_SLEEPABLE_LOCK *lock)
{
    if (!lock->lock.initialized) {
        REQUIRE(pthread_mutex_init(&lock->lock.native, NULL) == 0);
        lock->lock.initialized = true;
    }
    table_destroyed = false;
    return 0;
}

static int osal_sleepable_lock_deinit(OSAL_SLEEPABLE_LOCK *lock)
{
    /* Keep backing storage valid to observe the old post-destroy counter write. */
    table_destroyed = true;
    return 0;
}

static int osal_lock_sleepable_lock(OSAL_SLEEPABLE_LOCK *lock)
{
    if (table_destroyed)
        late_lock_uses++;
    mutex_lock(&lock->lock);
    return 0;
}

static void osal_unlock_sleepable_lock(OSAL_SLEEPABLE_LOCK *lock) { mutex_unlock(&lock->lock); }

static int genl_register_family(struct genl_family *family)
{
    int result = 0;

    REQUIRE(pthread_rwlock_wrlock(&family_gate) == 0);
    if (registered)
        result = -EEXIST;
    else {
        registered = true;
        registrations++;
    }
    REQUIRE(pthread_rwlock_unlock(&family_gate) == 0);
    return result;
}

static int genl_unregister_family(struct genl_family *family)
{
    REQUIRE(pthread_mutex_lock(&gate) == 0);
    unregister_entered = true;
    unregister_during_send |= unicast_running;
    REQUIRE(pthread_cond_broadcast(&changed) == 0);
    REQUIRE(pthread_mutex_unlock(&gate) == 0);
    /* Generic netlink waits for admitted callbacks, but not outgoing unicast. */
    REQUIRE(pthread_rwlock_wrlock(&family_gate) == 0);
    REQUIRE(registered);
    registered = false;
    family->n_ops = 0; /* This kernel's genetlink.c:445 does exactly this. */
    unregisters++;
    REQUIRE(pthread_rwlock_unlock(&family_gate) == 0);
    return 0;
}

static void *nla_data(struct nlattr *attribute) { return attribute->text; }
static int stp_dbg_core_dump_nl(void *dump, char *message, int length) { dump_calls++; return core_result; }
static struct sk_buff *genlmsg_new(int size, int flags) { return fail_allocation ? NULL : malloc(sizeof(struct sk_buff)); }
static void nlmsg_free(struct sk_buff *skb) { free(skb); }
static void *genlmsg_put(struct sk_buff *skb, int port, unsigned int sequence,
                         struct genl_family *family, int flags, int command)
{
    return fail_header ? NULL : skb;
}
static int nla_put(struct sk_buff *skb, int attribute, int length, char *message)
{
    return fail_attribute ? -EMSGSIZE : 0;
}
static void genlmsg_end(struct sk_buff *skb, void *header) { REQUIRE(header == skb); }

static int genlmsg_unicast(const int *network, struct sk_buff *skb, pid_t port)
{
    int result = port == refused_port ? -ECONNREFUSED : unicast_result;

    REQUIRE(port >= 101 && port <= 104);
    REQUIRE(pthread_mutex_lock(&gate) == 0);
    sends++;
    send_entered = unicast_running = true;
    REQUIRE(pthread_cond_broadcast(&changed) == 0);
    while (pause_send && !release_send)
        REQUIRE(pthread_cond_wait(&changed, &gate) == 0);
    if (!result) {
        REQUIRE(delivered_count < ARRAY_SIZE(delivered));
        delivered[delivered_count++] = port;
    }
    unicast_running = false;
    REQUIRE(pthread_mutex_unlock(&gate) == 0);
    free(skb); /* Unicast consumes the skb even when it returns an error. */
    return result;
}

/* SOURCE_FUNCTIONS */

static int bind_peer_id(pid_t port)
{
    struct genl_info info = {.snd_portid = port};
    int result;

    REQUIRE(pthread_rwlock_rdlock(&family_gate) == 0);
    REQUIRE(registered && stp_dbg_gnl_family.n_ops == ARRAY_SIZE(stp_dbg_gnl_ops_array));
    result = stp_dbg_nl_bind(NULL, &info);
    REQUIRE(pthread_rwlock_unlock(&family_gate) == 0);
    return result;
}

static int bind_peer(void) { return bind_peer_id(101); }

static void closed_table(void)
{
    unsigned int i;

    fprintf(stdout, "closed count=%d sends=%u late_lock_uses=%u\n", num_bind_process, sends, late_lock_uses);
    REQUIRE(!registered && num_bind_process == 0);
    for (i = 0; i < MAX_BIND_PROCESS; i++)
        REQUIRE(bind_pid[i] == 0);
}

static void *send_thread(void *unused)
{
    int result = stp_dbg_nl_send("x", 2, 1);

    REQUIRE(result == (unicast_result ? -1 : 0));
    return NULL;
}

static void *close_thread(void *unused)
{
    closing_thread = true;
    stp_dbg_nl_deinit();
    REQUIRE(pthread_mutex_lock(&gate) == 0);
    close_done = true;
    REQUIRE(pthread_cond_broadcast(&changed) == 0);
    REQUIRE(pthread_mutex_unlock(&gate) == 0);
    return NULL;
}

static void pending_send_close(int result)
{
    pthread_t sender, closer;
    bool closed_early;

    stp_dbg_nl_init();
    REQUIRE(bind_peer() == 0 && num_bind_process == 1);
    pause_send = true;
    unicast_result = result;
    REQUIRE(pthread_create(&sender, NULL, send_thread, NULL) == 0);
    REQUIRE(pthread_mutex_lock(&gate) == 0);
    while (!send_entered)
        REQUIRE(pthread_cond_wait(&changed, &gate) == 0);
    REQUIRE(pthread_mutex_unlock(&gate) == 0);
    REQUIRE(pthread_create(&closer, NULL, close_thread, NULL) == 0);
    REQUIRE(pthread_mutex_lock(&gate) == 0);
    while (!close_lock_attempt && !close_done)
        REQUIRE(pthread_cond_wait(&changed, &gate) == 0);
    closed_early = close_done;
    if (close_lock_attempt) {
        /* The close is at a lock still held by the paused production sender. */
        REQUIRE(pthread_mutex_trylock(&close_wait_lock->native) == EBUSY);
        REQUIRE(!unregister_entered);
    }
    release_send = true;
    REQUIRE(pthread_cond_broadcast(&changed) == 0);
    REQUIRE(pthread_mutex_unlock(&gate) == 0);
    REQUIRE(pthread_join(sender, NULL) == 0);
    REQUIRE(pthread_join(closer, NULL) == 0);
    fprintf(stdout, "close_before_send_finished=%d unregister_during_send=%d\n", closed_early, unregister_during_send);
    closed_table();
    REQUIRE(!closed_early && !unregister_during_send);
}

static void *admitted_bind_thread(void *unused)
{
    struct genl_info info = {.snd_portid = 101};

    REQUIRE(pthread_rwlock_rdlock(&family_gate) == 0);
    REQUIRE(registered);
    REQUIRE(pthread_mutex_lock(&gate) == 0);
    bind_admitted = true;
    REQUIRE(pthread_cond_broadcast(&changed) == 0);
    while (!release_bind)
        REQUIRE(pthread_cond_wait(&changed, &gate) == 0);
    REQUIRE(pthread_mutex_unlock(&gate) == 0);
    REQUIRE(stp_dbg_nl_bind(NULL, &info) == 0);
    REQUIRE(pthread_rwlock_unlock(&family_gate) == 0);
    return NULL;
}

static void admitted_bind_close(void)
{
    pthread_t binder, closer;

    stp_dbg_nl_init();
    REQUIRE(pthread_create(&binder, NULL, admitted_bind_thread, NULL) == 0);
    REQUIRE(pthread_mutex_lock(&gate) == 0);
    while (!bind_admitted)
        REQUIRE(pthread_cond_wait(&changed, &gate) == 0);
    REQUIRE(pthread_mutex_unlock(&gate) == 0);
    REQUIRE(pthread_create(&closer, NULL, close_thread, NULL) == 0);
    REQUIRE(pthread_mutex_lock(&gate) == 0);
    while (!unregister_entered)
        REQUIRE(pthread_cond_wait(&changed, &gate) == 0);
    REQUIRE(!close_done);
    release_bind = true;
    REQUIRE(pthread_cond_broadcast(&changed) == 0);
    REQUIRE(pthread_mutex_unlock(&gate) == 0);
    REQUIRE(pthread_join(binder, NULL) == 0);
    REQUIRE(pthread_join(closer, NULL) == 0);
    closed_table();
}

static void first_peer_refused(void)
{
    bool seen[3] = {false};
    unsigned int i;

    stp_dbg_nl_init();
    for (i = 101; i <= 104; i++)
        REQUIRE(bind_peer_id(i) == 0);
    refused_port = 101;
    REQUIRE(stp_dbg_nl_send("x", 2, 1) == 0 && delivered_count == 3);
    fprintf(stdout, "after refusal count=%d peers=[%d,%d,%d,%d]\n",
            num_bind_process, bind_pid[0], bind_pid[1], bind_pid[2], bind_pid[3]);
    REQUIRE(num_bind_process == 3 && bind_pid[3] == 0);
    for (i = 0; i < 3; i++) {
        REQUIRE(bind_pid[i] >= 102 && bind_pid[i] <= 104);
        REQUIRE(!seen[bind_pid[i] - 102]);
        seen[bind_pid[i] - 102] = true;
    }
    delivered_count = 0;
    REQUIRE(stp_dbg_nl_send("x", 2, 1) == 0 && delivered_count == 3);
    for (i = 0; i < delivered_count; i++) {
        REQUIRE(delivered[i] >= 102 && delivered[i] <= 104);
        REQUIRE(seen[delivered[i] - 102]);
        seen[delivered[i] - 102] = false;
    }
    stp_dbg_nl_deinit();
    closed_table();
}

int main(int argc, char **argv)
{
    unsigned int i;

    REQUIRE(argc == 2);
    switch (atoi(argv[1])) {
    case 0:
        for (i = 0; i < 2; i++) {
            stp_dbg_nl_init();
            fprintf(stdout, "registration=%u n_ops=%u\n", registrations, stp_dbg_gnl_family.n_ops);
            REQUIRE(stp_dbg_gnl_family.n_ops == ARRAY_SIZE(stp_dbg_gnl_ops_array));
            REQUIRE(bind_peer() == 0);
            REQUIRE(stp_dbg_nl_send("x", 2, 1) == 0);
            stp_dbg_nl_deinit();
            closed_table();
        }
        REQUIRE(registrations == 2 && unregisters == 2);
        break;
    case 1:
        pending_send_close(0);
        break;
    case 2:
        admitted_bind_close();
        break;
    case 3:
        pending_send_close(-ECONNREFUSED);
        break;
    case 4:
        stp_dbg_nl_init();
        REQUIRE(stp_dbg_nl_send("x", 2, 1) == 0 && dump_calls == 0);
        REQUIRE(bind_peer() == 0);
        for (i = 0; i < 5; i++) {
            core_result = i == 0 ? -EIO : (i == 1 ? 32 : 0);
            fail_header = i == 2;
            fail_attribute = i == 3;
            fail_allocation = i == 4;
            REQUIRE(stp_dbg_nl_send("x", 2, 1) ==
                    (i == 0 ? -EIO : (i == 1 ? 32 : (i == 3 ? -EMSGSIZE : -1))));
            /* Exercise both lifecycle and table locking after every early exit. */
            stp_dbg_nl_init();
            REQUIRE(bind_peer() == 0);
        }
        stp_dbg_nl_deinit();
        closed_table();
        break;
    case 5:
        first_peer_refused();
        break;
    default:
        REQUIRE(false);
    }
    return 0;
}
