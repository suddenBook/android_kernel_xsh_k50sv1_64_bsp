/* Kernel-service fixture for probe-unwind.py; driver functions are extracted. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef void VOID, *PVOID;
typedef bool BOOLEAN;
typedef int INT_32, WLAN_STATUS;
typedef unsigned int UINT_32;
typedef unsigned short UINT_16;
typedef unsigned long ULONG;
typedef unsigned char u8;
typedef char CHAR;
#define TRUE true
#define FALSE false
#define WLAN_STATUS_SUCCESS 0
#define WLAN_STATUS_FAILURE 1
#define CFG_SUPPORT_THERMO_THROTTLING 1
#define WLAN_INCLUDE_PROC 1
#define FW_CFG_SUPPORT 1
#define CFG_INIT_POWER_SAVE_PROF 0
#define CFG_FW_START_ADDRESS 0
#define CFG_FW_LOAD_ADDRESS 0
#define WIFI_MAC_ADDRESS_FILE "mac"
#define CONN_MCU_CPUPCR 0
#define MCU_REG_READL(...) 0
#define PHY_MEM_TYPE 0
#define GLUE_FLAG_HALT_BIT 0
#define IEEE80211_BAND_5GHZ 1
#define PARAM_MAC_ADDR_LEN 6
#define PARAM_MEDIA_STATE_DISCONNECTED 0
#define NIC_INF_NAME "wlan%d"
#define NET_NAME_PREDICTABLE 0
#define CFG_MAX_TXQ_NUM 4
#define FULL_SCAN_MAX_CHANNEL_NUM 8
#define SPIN_LOCK_NUM 2
#define RST_FLAG_DO_CORE_DUMP 1
#define ENUM_WIFI_LOG_LEVEL_OFF 0
#define ENUM_WIFI_LOG_LEVEL_VERSION_V1 1
#define ENUM_WIFI_LOG_MODULE_FW 1
#define ASSERT assert
#define DBGLOG(...) ((void)0)
#define kalMemZero(p, n) memset(p, 0, n)
#define kalMemSet memset
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define __user
#define KUIDT_INIT(n) (n)
#define KGIDT_INIT(n) (n)
#define PROC_UID_SHELL 2000
#define PROC_GID_WIFI 1010
#define PROC_ROOT_NAME "wlan"
#define PROC_DBG_LEVEL_NAME "dbgLevel"
#define PROC_NEED_TX_DONE "txdone"
#define PROC_AUTO_PERF_CFG "perf"
#define PROC_COUNTRY "country"
#define PROC_WLAN_THERMO "thermo"
#define PROC_CMD_DEBUG_NAME "command"
#define PROC_SET_CAM "cam"
#define PROC_CFG_NAME "cfg"
#define kalSprintf sprintf
#define kalStrnCpy strncpy
#define kalStrLen strlen
#define pr_info(...) ((void)0)

struct proc_dir_entry { const char *name; int session; };
struct file { int unused; };
struct task_struct { int running; };
struct completion { int rx; };
struct wait_queue { int rx; };
struct wake_lock { int initialized; };
struct lock_class_key { int unused; };
struct device { int unused; };
struct glue;
struct wireless_dev;
struct net_device {
    struct glue *private;
    int tx_queue_len, ifindex;
    const void *netdev_ops;
    struct wireless_dev *ieee80211_ptr;
    unsigned char dev_addr[6], perm_addr[6];
};
typedef struct {
    unsigned int u4StartAddress, u4LoadAddress, u4PowerMode;
    bool fgEnArpFilter;
    unsigned char aucMacAddr[6];
} REG_INFO_T, *P_REG_INFO_T;
typedef struct { int unused; } GL_HIF_INFO_T;
typedef struct adapter {
    struct glue *prGlueInfo;
    bool fgEnable5GBand;
    struct wake_lock rTxThreadWakeLock;
    struct { struct { unsigned short u2CountryCode; } rConnSettings; } rWifiVar;
} ADAPTER_T, *P_ADAPTER_T;
typedef struct glue {
    P_ADAPTER_T prAdapter;
    struct net_device *prDevHandler;
    struct proc_dir_entry *pProcRoot;
    struct task_struct *main_thread, *rx_thread;
    struct completion rScanComp, rHaltComp, rPendComp, rRxHaltComp;
    struct wait_queue waitq, waitq_rx;
    struct wake_lock rAhbIsrWakeLock, rTimeoutWakeLock;
    unsigned long ulFlag;
    unsigned int u4ReadyFlag, u4FWRoamingEnable, rBuildVarint;
    int i4DevIdx, rFtIeForTx, eParamMediaStateIndicated, ucTrScanType;
    bool fgIsMacAddrOverride, fgIsRegistered;
    void *prScanRequest, *puScanChannel, *puFullScan2PartialChannel;
    int u4LastFullScanTime, rSpinLock[SPIN_LOCK_NUM], ioctl_sem, rCmdQueue, rTxQueue;
    unsigned char ucChannelNum[FULL_SCAN_MAX_CHANNEL_NUM];
    REG_INFO_T rRegInfo;
    GL_HIF_INFO_T rHifInfo;
} GLUE_INFO_T, *P_GLUE_INFO_T;
struct wiphy { GLUE_INFO_T private; void *bands[2]; };
struct wireless_dev { struct wiphy *wiphy; struct net_device *netdev; };
typedef struct { struct net_device *prDev; } WLANDEV_INFO_T, *P_WLANDEV_INFO_T;
typedef struct {
    struct proc_dir_entry *prEntryDbgLevel, *prEntryTxDoneCfg, *prEntryAutoPerCfg;
    struct proc_dir_entry *prEntryCountry, *prEntryWlanThermo, *prEntryCmdDbg;
    struct proc_dir_entry *prEntrySetCAM, *prEntryCfg;
} PROC_CFG_ENTRY;

static const char *failure;
static struct wiphy wiphy;
static struct wireless_dev wdev = { .wiphy = &wiphy };
static struct wireless_dev *gprWdev = &wdev;
static struct net_device *gPrDev;
static WLANDEV_INFO_T arWlanDevInfo[1];
static unsigned int u4WlanDevNum, u4FwLogLevel;
static bool fgIsBusAccessFailed, fgIsWorkMcEverInit;
static int workq, sched_workq, mtk_band_5ghz, wlan_netdev_ops;
static struct lock_class_key rSpinKey[SPIN_LOCK_NUM];
static struct task_struct tx_task, rx_task;
static bool halted = true, halt_locked;
static int bus, netdevs, adapters, started, irq, notifier, timer, hif, firmware;
static int session_entries, proc_allocations, perf_initialized, loaded;
static int callbacks, blocking_waits, interruptible_waits;
static void *debug_storage;
static struct proc_dir_entry proc_net = { .name = "net" };
static struct { struct proc_dir_entry *proc_net; } init_net = { &proc_net };
static struct proc_dir_entry *gprProcRoot, *entries[8];
static PROC_CFG_ENTRY *gprProcCfgEntry;
static P_GLUE_INFO_T g_prGlueInfo_proc, gprGlueInfo;
static int dbglevel_ops, proc_txdone_ops, auto_perf_ops, country_ops;
static int proc_fops, proc_CmdDebug_ops, proc_set_cam_ops, cfg_ops;
static char aucProcBuf[1024];

static bool fail(const char *name) { return failure && !strcmp(failure, name); }
static void debug_callback(void) {
    assert(debug_storage && adapters);
    ++callbacks;
    ((unsigned char *)debug_storage)[0]++;
    wiphy.private.prAdapter->fgEnable5GBand = false;
}
static void adapter_callback(void) {
    assert(adapters);
    ++callbacks;
    (void)wiphy.private.prAdapter->rWifiVar.rConnSettings.u2CountryCode;
}
static void assert_session_clean(void) {
    assert(!tx_task.running && !rx_task.running && !irq && !notifier);
    assert(!u4WlanDevNum && !session_entries && !started);
    assert(!netdevs && !adapters && !timer && !hif && !firmware && !bus);
    assert(!workq && !sched_workq && !halt_locked);
    assert(!gPrDev && !wiphy.private.prAdapter);
    assert(!g_prGlueInfo_proc && !gprGlueInfo);
    assert(!debug_storage);
}
static bool wlanDebugInit(void) {
    assert(!debug_storage);
    if (fail("debug")) return false;
    debug_storage = calloc(1, 64); assert(debug_storage); return true;
}
static void wlanDebugUninit(void) {
    /* All callback sources must be drained before these shared buffers die. */
    assert(!tx_task.running && !rx_task.running && !irq && !notifier);
    assert(!u4WlanDevNum && !session_entries && !started && !adapters);
    assert(!workq && !sched_workq);
    free(debug_storage); debug_storage = NULL;
}
static void *wiphy_priv(struct wiphy *value) { return &value->private; }
static void *netdev_priv(struct net_device *value) { return &value->private; }
static struct net_device *alloc_netdev_mq(size_t size, const char *name,
                                        int mode, void (*setup)(void), int queues) {
    if (fail("netdev")) return NULL;
    assert(!netdevs++);
    struct net_device *result = calloc(1, sizeof(*result)); assert(result); return result;
}
static void free_netdev(struct net_device *value) {
    assert(netdevs == 1 && !u4WlanDevNum && !irq && !tx_task.running && !rx_task.running);
    --netdevs; free(value);
}
static void ether_setup(void) {}
#define SET_NETDEV_DEV(...) ((void)0)
#define netif_carrier_off(...) ((void)0)
#define netif_tx_stop_all_queues(...) ((void)0)
#define spin_lock_init(...) ((void)0)
#define lockdep_set_class(...) ((void)0)
#define sema_init(...) ((void)0)
#define QUEUE_INITIALIZE(...) ((void)0)
#define init_completion(c) ((c)->rx = ((c) == &wiphy.private.rRxHaltComp))
#define init_waitqueue_head(q) ((q)->rx = ((q) == &wiphy.private.waitq_rx))
static void kalTimeoutHandler(void) {}
static void kalOsTimerInitialize(P_GLUE_INFO_T glue, void (*fn)(void)) { assert(!timer++); }
static void kalCancelTimer(P_GLUE_INFO_T glue) { assert(timer == 1); --timer; }
static void glSetHifInfo(P_GLUE_INFO_T glue, ULONG data) { assert(!hif++); }
static void glClearHifInfo(P_GLUE_INFO_T glue) { assert(hif == 1); --hif; }
static P_ADAPTER_T wlanAdapterCreate(P_GLUE_INFO_T glue) {
    if (fail("adapter")) return NULL;
    assert(!adapters++);
    P_ADAPTER_T result = calloc(1, sizeof(*result)); assert(result);
    result->prGlueInfo = glue; return result;
}
static void wlanAdapterDestroy(P_ADAPTER_T adapter) {
    assert(adapter && adapters == 1 && halted && halt_locked);
    assert(!started && !irq && !tx_task.running && !rx_task.running);
    assert(!notifier && !u4WlanDevNum && !session_entries && !workq && !sched_workq);
    assert(!adapter->rTxThreadWakeLock.initialized);
    assert(!wiphy.private.rAhbIsrWakeLock.initialized);
#if CFG_SUPPORT_MULTITHREAD
    assert(!wiphy.private.rTimeoutWakeLock.initialized);
#endif
    debug_callback(); --adapters; free(adapter);
}
static void wake_lock_init(struct wake_lock *lock) { assert(!lock->initialized); lock->initialized = 1; }
static void wake_lock_destroy(struct wake_lock *lock) { assert(lock->initialized); lock->initialized = 0; }
#define KAL_WAKE_LOCK_INIT(a, lock, name) wake_lock_init(lock)
#define KAL_WAKE_LOCK_DESTROY(a, lock) wake_lock_destroy(lock)
#define KAL_WAKE_LOCK_ACTIVE(a, lock) ((lock)->initialized)
static bool glBusInit(void *data) {
    if (fail("bus")) return false;
    assert(!bus++); return true;
}
static void glBusRelease(struct net_device *dev) { assert(bus == 1); --bus; }
static int glBusSetIrq(struct net_device *dev, void *isr, P_GLUE_INFO_T glue) {
    if (fail("irq")) return -EIO;
    assert(!irq++); debug_callback(); return 0;
}
static void glBusFreeIrq(struct net_device *dev, P_GLUE_INFO_T glue) {
    assert(irq == 1); debug_callback(); --irq;
}
static void glLoadNvram(P_GLUE_INFO_T glue, P_REG_INFO_T reg) {}
static bool hasFile(const char *name) { return true; }
static int readMac(const char *name, char *data, size_t size) { return fail("mac"); }
static int khwaddr_aton(const char *data, unsigned char *mac) { return 0; }
static void *kalFirmwareImageMapping(P_GLUE_INFO_T glue, void **buffer, UINT_32 *size) {
    if (fail("firmware")) return NULL;
    assert(!firmware++); *buffer = &firmware; *size = sizeof(firmware); return *buffer;
}
static void kalFirmwareImageUnmapping(P_GLUE_INFO_T glue, void *handle, void *buffer) {
    assert(firmware == 1); --firmware;
}
static void HifRegDump(P_ADAPTER_T adapter) { debug_callback(); }
static int wlanAdapterStart(P_ADAPTER_T adapter, P_REG_INFO_T reg, void *fw, UINT_32 size) {
    debug_callback();
    if (fail("start")) return WLAN_STATUS_FAILURE;
    assert(!started++); wake_lock_init(&adapter->rTxThreadWakeLock); return 0;
}
static void wlanAdapterStop(P_ADAPTER_T adapter) {
    assert(started == 1 && !tx_task.running && !rx_task.running && !irq);
    debug_callback(); --started;
}
static int tx_thread(void *data) { return 0; }
static int rx_thread(void *data) { return 0; }
static struct task_struct *kthread_run(int (*fn)(void *), void *data, const char *name) {
    bool rx = fn == rx_thread;
    if (fail(rx ? "rx" : "tx")) return ERR_PTR(-EAGAIN);
    struct task_struct *task = rx ? &rx_task : &tx_task;
    assert(!task->running); task->running = 1; return task;
}
static void kalChangeSchedParams(P_GLUE_INFO_T glue, bool initial) {
    assert(glue->main_thread && !IS_ERR(glue->main_thread));
#if CFG_SUPPORT_MULTITHREAD
    assert(glue->rx_thread && !IS_ERR(glue->rx_thread));
#endif
}
static void kalSetHalted(bool value) { halted = value; }
static bool kalIsHalted(void) { return halted; }
static int kalHaltTryLock(void) {
    if (halt_locked) return -1;
    halt_locked = true; return 0;
}
static int kalHaltLock(unsigned int timeout) {
    assert(!timeout && !halt_locked && halted);
    assert(!session_entries && !notifier && !u4WlanDevNum && !workq && !sched_workq);
    halt_locked = true; return 0;
}
static void kalHaltUnlock(void) { assert(halt_locked); halt_locked = false; }
static void set_bit(int bit, unsigned long *flags) { *flags |= 1UL << bit; }
static void wake_up_interruptible(struct wait_queue *queue) {
    assert(wiphy.private.ulFlag & (1UL << GLUE_FLAG_HALT_BIT));
    assert(queue->rx ? rx_task.running : tx_task.running);
}
static void wait_for_completion(struct completion *completion) {
    struct task_struct *task = completion->rx ? &rx_task : &tx_task;
    assert(task->running); debug_callback(); task->running = 0; ++blocking_waits;
}
static int wait_for_completion_interruptible(struct completion *completion) {
    ++interruptible_waits; return -EINTR;
}
static void cancel_delayed_work_sync(int *work) {
    if (*work) { assert(!halt_locked); debug_callback(); *work = 0; }
}
static int work_busy(int *work) { return 0; /* Pending work need not be running. */ }
static int wlanNetRegister(struct wireless_dev *wireless) {
    /* ndo_init can queue work even if register_netdev then fails. */
    fgIsWorkMcEverInit = true; workq = sched_workq = 1;
    if (fail("register")) return -1;
    assert(!u4WlanDevNum++); wiphy.private.fgIsRegistered = true; return 0;
}
static void wlanNetUnregister(struct wireless_dev *wireless) {
    assert(u4WlanDevNum == 1); debug_callback(); --u4WlanDevNum;
    wiphy.private.fgIsRegistered = false;
}
static void wlanRegisterNotifier(void) { assert(!notifier++); }
static void wlanUnregisterNotifier(void) { assert(notifier == 1); adapter_callback(); --notifier; }
static int wlanoidQueryCurrentAddr;
static int kalIoctl(P_GLUE_INFO_T glue, int oid, void *data, int size,
                   bool read, bool wait, bool command, bool p2p, UINT_32 *length) {
    assert(!halted && started && tx_task.running); debug_callback(); return 0;
}
static void ether_addr_copy(unsigned char *dst, const unsigned char *src) { memcpy(dst, src, 6); }
static int wlanFwArrayCfg(P_ADAPTER_T adapter) { return 0; }
static void kalPerMonInit(P_GLUE_INFO_T glue) { ++perf_initialized; }
static void update_driver_loaded_status(bool value) { loaded = value; }
static void wlanProcessInfoFile(P_ADAPTER_T adapter) {}
static void wlanDbgSetLogLevelImpl(P_ADAPTER_T adapter, int version, int module, unsigned int level) {
    assert(adapters); debug_callback();
}
static void reset_trigger(P_ADAPTER_T adapter, int flags) {
    /* The real reset path must not receive a freed local adapter pointer. */
    assert(!adapter || adapters);
}
#define GL_RESET_TRIGGER reset_trigger

static void *kalMemAlloc(size_t size, int type) {
    assert(size == sizeof(PROC_CFG_ENTRY) && type == PHY_MEM_TYPE);
    if (fail("proc-alloc")) return NULL;
    assert(!proc_allocations++);
    void *result = malloc(size); assert(result); return result;
}
static void kalMemFree(void *data, int type, size_t size) {
    if (!data) return;
    assert(size == sizeof(PROC_CFG_ENTRY) && type == PHY_MEM_TYPE && proc_allocations == 1);
    --proc_allocations; free(data);
}
static struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent) {
    assert(parent == &proc_net && !gprProcRoot);
    if (fail("proc-root")) return NULL;
    struct proc_dir_entry *entry = malloc(sizeof(*entry)); assert(entry);
    *entry = (struct proc_dir_entry){ name, 0 }; return entry;
}
static struct proc_dir_entry *proc_create(const char *name, int mode,
                                         struct proc_dir_entry *root, void *ops) {
    assert(root && root == gprProcRoot);
    bool session = ops == &proc_fops || ops == &proc_CmdDebug_ops ||
                   ops == &proc_set_cam_ops || ops == &cfg_ops;
    const char *fault = session ? name : ops == &dbglevel_ops ? "proc-debug" :
        ops == &proc_txdone_ops ? "proc-txdone" : ops == &auto_perf_ops ? "proc-perf" : "proc-country";
    if (fail(fault)) return NULL;
    for (int i = 0; i < 8; ++i) assert(!entries[i] || strcmp(entries[i]->name, name));
    for (int i = 0; i < 8; ++i) {
        if (entries[i]) continue;
        entries[i] = malloc(sizeof(*entries[i])); assert(entries[i]);
        *entries[i] = (struct proc_dir_entry){ name, session };
        if (session) {
            ++session_entries;
            assert(ops == &cfg_ops ? gprGlueInfo == &wiphy.private : g_prGlueInfo_proc == &wiphy.private);
            debug_callback(); /* proc_create publishes the entry immediately. */
        }
        return entries[i];
    }
    assert(!"proc fixture exhausted"); return NULL;
}
static void proc_set_user(struct proc_dir_entry *entry, int uid, int gid) { assert(entry); }
static void remove_proc_entry(const char *name, struct proc_dir_entry *root) {
    assert(root == gprProcRoot);
    for (int i = 0; i < 8; ++i) {
        if (!entries[i] || strcmp(entries[i]->name, name)) continue;
        if (entries[i]->session) {
            debug_callback(); /* Existing callback finishes before removal returns. */
            --session_entries;
        }
        free(entries[i]); entries[i] = NULL; return;
    }
    assert(!"remove of absent proc entry");
}
static void remove_proc_subtree(const char *name, struct proc_dir_entry *parent) {
    assert(parent == &proc_net && gprProcRoot && !strcmp(name, PROC_ROOT_NAME));
    for (int i = 0; i < 8; ++i) if (entries[i]) remove_proc_entry(entries[i]->name, gprProcRoot);
    free(gprProcRoot);
}
static int copy_to_user(char *dst, const char *src, size_t length) { memcpy(dst, src, length); return 0; }

#include "probe-under-test.c"

static void assert_module_clean(void) {
    assert(!gprProcRoot && !gprProcCfgEntry && !proc_allocations && !session_entries);
    assert(!gprGlueInfo && !g_prGlueInfo_proc);
    for (int i = 0; i < 8; ++i) assert(!entries[i]);
}
static void assert_module_ready(void) {
    assert(gprProcRoot && gprProcCfgEntry && proc_allocations == 1);
    int count = 0;
    for (int i = 0; i < 8; ++i) if (entries[i]) { assert(!entries[i]->session); ++count; }
    assert(count == 4);
}
static void stop_success(void) {
#if HAVE_PROBE_CLEANUP
    assert(session_entries == 3 + CFG_SUPPORT_SET_CAM_BY_PROC && loaded);
    assert(tx_task.running && rx_task.running == CFG_SUPPORT_MULTITHREAD);
    assert(procCreateFsEntry(&wiphy.private) == -EBUSY);
    assert(cfgCreateProcEntry(&wiphy.private) == -EBUSY);
    wlanProbeCleanup(&wdev, WLAN_PROBE_CFG_CREATED);
    wlanDebugUninit(); assert_session_clean();
    loaded = perf_initialized = 0;
#else
    /* Earlier code has no shared unwind helper. Regression runs select faults. */
    exit(0);
#endif
}
static void retry_cycles(void) {
    failure = NULL;
    for (int i = 0; i < 20; ++i) {
        assert(wlanProbe(&wdev) == 0);
        stop_success(); assert_module_ready();
    }
}
int main(int argc, char **argv) {
    assert(argc == 2); failure = argv[1];
    if (!strncmp(failure, "proc-", 5)) {
        if (fail("proc-net")) init_net.proc_net = NULL;
        int result = procInitFs();
        assert_module_clean();
        assert(result == (fail("proc-net") || fail("proc-root") ? -ENOENT : -ENOMEM));
        init_net.proc_net = &proc_net; failure = NULL;
        assert(procInitFs() == 0); assert_module_ready();
        procUninitProcFs(); assert_module_clean();
        procUninitProcFs(); assert_module_clean();
        return 0;
    }
    assert(procInitFs() == 0); assert_module_ready();
    if (fail("country-lock")) {
        /* A persistent country read may run while teardown owns the halt lock. */
        char output[128] = {0}; loff_t pos = 0;
        P_ADAPTER_T adapter = calloc(1, sizeof(*adapter)); assert(adapter);
        adapter->rWifiVar.rConnSettings.u2CountryCode = 0x5553;
        wiphy.private.prAdapter = adapter; g_prGlueInfo_proc = &wiphy.private;
        halted = false;
        assert(procCountryRead(NULL, output, sizeof(output), &pos) > 0);
        assert(strstr(output, "US") && !halt_locked);
        /* Poison the pointer so a read that ignores the busy lock is caught. */
        free(adapter); halt_locked = true; pos = 0; memset(output, 0, sizeof(output));
        assert(procCountryRead(NULL, output, sizeof(output), &pos) > 0);
        assert(!strstr(output, "US") && halt_locked);
        halt_locked = false; halted = true; pos = 0;
        assert(procCountryRead(NULL, output, sizeof(output), &pos) > 0);
        wiphy.private.prAdapter = NULL; g_prGlueInfo_proc = NULL;
    } else {
        if (fail("wdev")) gprWdev = NULL;
        int result = wlanProbe(&wdev);
        if (fail("success")) { assert(result == 0); stop_success(); }
        else {
            assert_session_clean();
            int expected = fail("debug") || fail("wdev") || fail("netdev") || fail("adapter") ||
                fail("thermo") || fail("command") || fail("cam") || fail("cfg") ? -ENOMEM :
                fail("tx") || fail("rx") ? -EAGAIN : fail("register") ? -ENXIO :
                fail("mac") ? -EINVAL : -EIO;
            assert(result == expected && !loaded && !perf_initialized);
            assert(!interruptible_waits);
            assert_module_ready();
            /* HIF calls wlanRemove on probe failure; its zero-device guard exits. */
            assert(!u4WlanDevNum);
        }
        gprWdev = &wdev;
        retry_cycles();
    }
    procUninitProcFs(); assert_module_clean();
    procRemoveProcfs(); cfgRemoveProcEntry(); procUninitProcFs(); assert_module_clean();
    return 0;
}
