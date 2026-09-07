#!/usr/bin/env python3
"""Exercise actual STA/P2P suspend and address-notifier producers under sanitizers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('scratch', type=Path)
parser.add_argument('--root', type=Path)
args = parser.parse_args()
root = args.root or Path(__file__).resolve().parents[3]
wlan = root / 'drivers/misc/mediatek/connectivity/source/wlan/core/gen2'

def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

init = (wlan / 'os/linux/gl_init.c').read_text()
p2p = (wlan / 'os/linux/gl_p2p_init.c').read_text()
platform = (wlan / 'os/linux/platform.c').read_text()
kal = (wlan / 'os/linux/gl_kal.c').read_text()
oid = (wlan / 'include/wlan_oid.h').read_text()
structs = oid[oid.index('typedef struct _PARAM_NETWORK_ADDRESS_IP {'):oid.index('\n#if CFG_SLT_SUPPORT', oid.index('typedef struct _PARAM_NETWORK_ADDRESS_IP {'))]
helpers = ''
for signature in ('BOOLEAN kalGetIPv4Address(', 'WLAN_STATUS kalSetIPv4Address('):
    if signature in kal:
        helpers += function(kal, signature)
functions = function(init, 'static void wlanNotifyFwSuspend(')
functions += function(init, 'void wlanHandleSystemSuspend(')
functions += function(init, 'void wlanHandleSystemResume(')
functions += function(p2p, 'void p2pHandleSystemSuspend(')
functions += function(p2p, 'void p2pHandleSystemResume(')
functions += function(platform, 'static int netdev_event(')
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT_8, *PUINT_8;
typedef uint16_t UINT_16;
typedef uint32_t UINT_32, WLAN_STATUS;
typedef uintptr_t ULONG;
typedef bool BOOLEAN;
typedef void *PVOID;
typedef int (*PFN_OID_HANDLER_FUNC)(void);
#define TRUE true
#define FALSE false
#define WLAN_STATUS_SUCCESS 0
#define WLAN_STATUS_FAILURE 1
#define WLAN_STATUS_NOT_SUPPORTED 2
#define PARAM_PROTOCOL_ID_TCP_IP 2
#define PARAM_PACKET_FILTER_P2P_MASK 0xf0
#define CFG_ENABLE_WIFI_DIRECT_CFG_80211 1
#define CFG_ENABLE_WIFI_DIRECT 1
#define CFG_SUPPORT_DROP_MC_PACKET 1
#define CFG_MAX_WLAN_DEVICES 1
#define NOTIFY_DONE 0
#define __aligned(n) __attribute__((aligned(n)))
#define OFFSET_OF(t, f) offsetof(t, f)
#define ASSERT(x) ((void)0)
#define IN
#define OUT
static void log_ignore(const char *fmt, ...) { (void)fmt; }
#define DBGLOG(m,c,...) log_ignore(__VA_ARGS__)
#define kalMemZero(p,n) memset(p,0,n)
struct net_device;
struct in_ifaddr {
    void *hash[2]; struct in_ifaddr *ifa_next; struct in_device *ifa_dev;
    void *rcu_head[2]; UINT_32 ifa_local, ifa_address;
};
struct in_device { struct net_device *dev; int refcnt, dead; struct in_ifaddr *ifa_list; };
struct list_head { struct list_head *next, *prev; };
struct inet6_dev { struct net_device *dev; struct list_head addr_list; unsigned char remaining[512]; };
struct adapter { UINT_32 u4OsPacketFilter; };
struct p2p_info { struct net_device *prDevHandler; };
typedef struct glue { struct adapter *prAdapter; struct p2p_info *prP2PInfo; } GLUE_INFO_T, *P_GLUE_INFO_T;
struct net_device { struct in_device *ip_ptr; struct inet6_dev *ip6_ptr; GLUE_INFO_T *private; const char *name; };
struct notifier_block { int unused; };
typedef struct { int eConnectionState, eCurrentOPMode, fgIsNetActive; } EVENT_AIS_BSS_INFO_T;
static struct { struct net_device *prDev; } arWlanDevInfo[CFG_MAX_WLAN_DEVICES];
static UINT_32 u4WlanDevNum = 1;
static BOOLEAN fgIsUnderSuspend;
static struct glue *exported_glue;
static unsigned char g_aucBufIpAddr[32] __attribute__((unused));
static int rcu_depth, check_rcu, ipv6_reads, sets, queries, notifications, last_suspend;
static unsigned last_count;
static bool last_p2p;
static UINT_32 last_ip;
static const void *ifa_source;
static WLAN_STATUS injected_status;
static void rcu_read_lock(void) { assert(!rcu_depth); ++rcu_depth; }
static void rcu_read_unlock(void) { assert(rcu_depth == 1); --rcu_depth; }
static struct in_device *__in_dev_get_rcu(struct net_device *dev) { assert(rcu_depth); return dev->ip_ptr; }
#define rcu_dereference(p) (assert(rcu_depth), (p))
static void *netdev_priv(struct net_device *dev) { assert(dev); return &dev->private; }
static bool wlanExportGlueInfo(struct glue **glue) { *glue = exported_glue; return *glue != NULL; }
static void kalMemCopy(void *dst, const void *src, size_t n) {
    if (src == ifa_source && check_rcu) assert(rcu_depth == 1);
    if (n == 16) ++ipv6_reads;
    memcpy(dst, src, n);
}
static int wlanoidSetNetworkAddress(void) { return 0; }
static int wlanoidSetP2pSetNetworkAddress(void) { return 0; }
static int wlanoidNotifyFwSuspend(void) { return 0; }
static int wlanoidQueryBSSInfo(void) { return 0; }
static int wlanoidSetCurrentPacketFilter(void) { return 0; }
'''
mocks = r'''
static WLAN_STATUS kalIoctl(struct glue *glue, PFN_OID_HANDLER_FUNC oid, void *buffer,
        UINT_32 length, bool read, bool wait, bool cmd, bool p2p, UINT_32 *written) {
    assert(glue && glue->prAdapter && !rcu_depth && cmd);
    (void)read; (void)wait; *written = 0;
    if (oid == wlanoidNotifyFwSuspend) {
        assert(length == sizeof(BOOLEAN) && !p2p);
        ++notifications; last_suspend = *(BOOLEAN *)buffer;
    } else if (oid == wlanoidQueryBSSInfo) ++queries;
    else if (oid != wlanoidSetCurrentPacketFilter) {
        assert(oid == wlanoidSetNetworkAddress || oid == wlanoidSetP2pSetNetworkAddress);
        PARAM_NETWORK_ADDRESS_LIST *list = buffer;
        assert(length >= sizeof(*list) && length <= 32);
        assert(list->u4AddressCount <= 1 && list->u2AddressType == PARAM_PROTOCOL_ID_TCP_IP);
        ++sets; last_count = list->u4AddressCount; last_p2p = p2p;
        assert(p2p == (oid == wlanoidSetP2pSetNetworkAddress));
        if (last_count) {
            size_t needed = offsetof(PARAM_NETWORK_ADDRESS_LIST, arAddress)
                + offsetof(PARAM_NETWORK_ADDRESS, aucAddress) + sizeof(PARAM_NETWORK_ADDRESS_IP);
            assert(length == needed);
            assert(list->arAddress[0].u2AddressLength == sizeof(PARAM_NETWORK_ADDRESS_IP));
            memcpy(&last_ip, list->arAddress[0].aucAddress + offsetof(PARAM_NETWORK_ADDRESS_IP, in_addr), 4);
        }
    }
    return injected_status;
}
'''
tests = r'''
int main(int argc, char **argv) {
    assert(argc == 3);
    const char *operation = argv[1], *mode = argv[2];
    struct adapter adapter = {0x3f};
    struct glue glue = {.prAdapter = &adapter};
    struct net_device dev = {.private = &glue, .name = "wlan0"};
    struct p2p_info p2p = {.prDevHandler = &dev}; glue.prP2PInfo = &p2p;
    struct in_ifaddr ifa = {.ifa_local = 0x010200c0};
    struct in_device in = {.dev = &dev, .ifa_list = &ifa}; ifa.ifa_dev = &in;
    ifa_source = &ifa.ifa_local; dev.ip_ptr = &in;
    struct inet6_dev *in6 = calloc(1, sizeof(*in6)); assert(in6);
    in6->dev = &dev; in6->addr_list.next = in6->addr_list.prev = &in6->addr_list;
    dev.ip6_ptr = in6; arWlanDevInfo[0].prDev = &dev; exported_glue = &glue;
    bool active = true, have_address = true;
    if (!strcmp(mode, "ipv4-only")) dev.ip6_ptr = NULL;
    else if (!strcmp(mode, "no-ip")) { dev.ip_ptr = NULL; have_address = false; }
    else if (!strcmp(mode, "no-address")) { in.ifa_list = NULL; have_address = false; }
    else if (!strcmp(mode, "zero-address")) { ifa.ifa_local = 0; have_address = false; }
    else if (!strcmp(mode, "rcu")) check_rcu = true;
    else if (!strcmp(mode, "io-error")) injected_status = WLAN_STATUS_FAILURE;
    else if (!strcmp(mode, "no-adapter")) { glue.prAdapter = NULL; active = false; }
    else if (!strcmp(mode, "no-glue")) { dev.private = NULL; exported_glue = NULL; active = false; }
    else if (!strcmp(mode, "no-device")) { arWlanDevInfo[0].prDev = NULL; p2p.prDevHandler = NULL; active = false; }
    else assert(!strcmp(mode, "dual-stack"));
    bool suspend = strstr(operation, "suspend") != NULL;
    bool is_p2p = !strncmp(operation, "p2p-", 4);
    bool notifier = !strcmp(operation, "notifier");
    if (!strcmp(operation, "sta-suspend")) wlanHandleSystemSuspend();
    else if (!strcmp(operation, "sta-resume")) wlanHandleSystemResume();
    else if (!strcmp(operation, "p2p-suspend")) p2pHandleSystemSuspend();
    else if (!strcmp(operation, "p2p-resume")) p2pHandleSystemResume();
    else {
        assert(notifier); fgIsUnderSuspend = true;
        if (!strcmp(mode, "no-device")) in.dev = NULL;
        netdev_event(NULL, 1, &ifa); suspend = true;
    }
    assert(!rcu_depth && !ipv6_reads);
    assert(sets == (int)active);
    if (active) {
        assert(last_count == (unsigned)(suspend && have_address));
        assert(last_p2p == is_p2p);
        if (last_count) assert(last_ip == ifa.ifa_local);
    }
    assert(notifications == (active && !is_p2p && !notifier));
    if (notifications) assert(last_suspend == (int)suspend);
    assert(queries == (active && !is_p2p && !suspend));
    free(in6);
    printf("PASS %s/%s\n", operation, mode);
}
'''
args.scratch.mkdir(parents=True, exist_ok=True)
operations = ('sta-suspend', 'sta-resume', 'p2p-suspend', 'p2p-resume', 'notifier')
modes = ('dual-stack', 'ipv4-only', 'no-ip', 'no-address', 'zero-address', 'rcu',
         'io-error', 'no-adapter', 'no-glue', 'no-device')
with tempfile.TemporaryDirectory(prefix='suspend-address-', dir=args.scratch) as directory:
    unit = Path(directory) / 'test.c'
    unit.write_text(prefix + structs + mocks + helpers + functions + tests)
    binary = Path(directory) / 'test'
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-pointer-bool-conversion',
                    '-DCONFIG_IPV6=1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(unit), '-o', str(binary)], check=True)
    failed = []
    for operation in operations:
        for mode in modes:
            result = subprocess.run([str(binary), operation, mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if result.returncode:
                failed.append(f'{operation}/{mode}')
                print(f'FAIL {operation}/{mode}: {result.stdout.splitlines()[0] if result.stdout else result.returncode}')
            else:
                print(result.stdout, end='')
    print(f'{len(operations)*len(modes)-len(failed)}/50 cases pass; failures={failed}')
    raise SystemExit(bool(failed))
