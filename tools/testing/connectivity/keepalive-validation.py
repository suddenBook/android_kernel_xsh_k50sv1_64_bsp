#!/usr/bin/env python3
"""Check actual keep-alive handlers and this kernel's attribute parser on the host."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("scratch", type=Path)
parser.add_argument("--source", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
vendor = root / "drivers/misc/mediatek/connectivity/source/wlan/core/gen2/os/linux"
source = (args.source or vendor / "gl_vendor.c").read_text()
header = (vendor / "include/gl_vendor.h").read_text()
netlink = (root / "include/net/netlink.h").read_text()
nlattr = (root / "lib/nlattr.c").read_text()


def function(text, name):
    match = re.search(r"^(?:static )?(?:inline )?[\w *]+\b" + name
                      + r"\([^;]*?\)\s*\n\{", text, re.M)
    if not match:
        raise ValueError("function not found: " + name)
    begin = text.index("{", match.start())
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end] + "\n"


def declaration(text, begin, terminator="};"):
    start = text.index(begin)
    return text[start:text.index(terminator, start) + len(terminator)] + "\n"


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8, UINT_8, BOOLEAN;
typedef uint16_t u16, UINT_16;
typedef uint32_t u32, UINT_32, WLAN_STATUS;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32, INT_32;
typedef int64_t s64;
typedef UINT_8 mac_addr[6];
struct nlattr { u16 nla_len, nla_type; };
#define NLA_HDRLEN 4
#define NLA_ALIGN(len) (((len) + 3) & ~3)
#define NLA_TYPE_MASK 0x3fff
#define NLA_TYPE_MAX (__NLA_TYPE_MAX - 1)
#define BUG_ON(x) assert(!(x))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define unlikely(x) (x)
#define pr_warn_ratelimited(...) ((void)0)
#define nla_for_each_attr(pos, head, len, rem) \
    for (pos = head, rem = len; nla_ok(pos, rem); pos = nla_next(pos, &(rem)))
'''
parts = [prefix,
         declaration(netlink, "enum {\n\tNLA_UNSPEC"),
         declaration(netlink, "struct nla_policy {"),
         *[function(netlink, name) for name in
           ("nla_type", "nla_data", "nla_len", "nla_ok", "nla_next",
            "nla_get_u8", "nla_get_u16", "nla_get_u32")],
         declaration(nlattr, "static const u16 nla_attr_minlen"),
         function(nlattr, "validate_nla"), function(nlattr, "nla_parse")]
if "NLA_PARSE_NESTED" in source[source.index("int mtk_cfg80211_vendor_packet_keep_alive_start"):]:
    parts.append(function(netlink, "nla_parse_nested"))
enum_start = header.rfind("typedef enum", 0, header.index("MKEEP_ALIVE_ATTRIBUTE_ID"))
parts.append(header[enum_start:header.index("} WIFI_MKEEP_ALIVE_ATTRIBUTE;", enum_start)
                    + len("} WIFI_MKEEP_ALIVE_ATTRIBUTE;")] + "\n")
parts.append(declaration(header, "typedef struct _PARAM_PACKET_KEEPALIVE_T", "*P_PARAM_PACKET_KEEPALIVE_T;"))
parts.append(declaration(source, "static struct nla_policy nla_parse_offloading_policy"))
parts.append(r'''
#define NLA_PARSE nla_parse
#define NLA_PARSE_NESTED nla_parse_nested
#define WLAN_STATUS_SUCCESS 0
#define TRUE 1
#define FALSE 0
#define ASSERT(x) assert(x)
#define DBGLOG(...) ((void)0)
#define VIR_MEM_TYPE 0
#define kalMemZero(ptr, count) memset(ptr, 0, count)
#define kalMemCopy memcpy
struct wiphy { int private; };
struct wireless_dev { int unused; };
typedef void *P_GLUE_INFO_T;
static void *wiphy_priv(struct wiphy *wiphy) { return wiphy; }
static int allocations, ioctls, fail_allocation, ioctl_result;
static PARAM_PACKET_KEEPALIVE_T submitted;
static int wlanoidPacketKeepAlive;
static void *allocate(size_t bytes) {
    if (fail_allocation) return NULL;
    void *memory = malloc(bytes);
    assert(memory); ++allocations;
    return memory;
}
static void release(void *memory) { assert(memory && allocations > 0); --allocations; free(memory); }
#define kalMemAlloc(bytes, type) allocate(bytes)
#define kalMemFree(ptr, type, bytes) release(ptr)
static WLAN_STATUS kalIoctl(P_GLUE_INFO_T glue, int oid, void *request, size_t size,
                            int a, int b, int c, int d, UINT_32 *length) {
    (void)oid; (void)a; (void)b; (void)c; (void)d; (void)length;
    assert(glue && request && size == sizeof(submitted));
    ++ioctls; memcpy(&submitted, request, sizeof(submitted));
    return ioctl_result;
}
''')
if "static int mtk_cfg80211_vendor_parse_offload_attrs" in source:
    parts.append(function(source, "mtk_cfg80211_vendor_parse_offload_attrs"))
parts.extend(function(source, name) for name in
             ("mtk_cfg80211_vendor_packet_keep_alive_start", "mtk_cfg80211_vendor_packet_keep_alive_stop"))
parts.append(r'''
static struct wiphy phy;
static struct wireless_dev dev;
static _Alignas(8) unsigned char message[1024];
static size_t bytes;
static void begin(void) { memset(message, 0, sizeof(message)); bytes = NLA_HDRLEN; }
static void add(unsigned int type, const void *payload, size_t size) {
    struct nlattr *attr = (struct nlattr *)(message + bytes);
    attr->nla_type = type; attr->nla_len = NLA_HDRLEN + size;
    memcpy(nla_data(attr), payload, size);
    bytes += NLA_ALIGN(attr->nla_len);
    assert(bytes <= sizeof(message));
    ((struct nlattr *)message)->nla_len = bytes;
}
static void build(unsigned int omitted, uint16_t declared, size_t actual,
                  size_t source_mac_length, size_t destination_mac_length) {
    uint8_t index = 7, payload[300], mac[6] = {2, 3, 4, 5, 6, 7};
    uint32_t period = 12345;
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = i;
    begin();
    if (omitted != 1) add(1, &index, sizeof(index));
    if (omitted != 2) add(2, &declared, sizeof(declared));
    if (omitted != 3) add(3, payload, actual);
    if (omitted != 4) add(4, mac, source_mac_length);
    if (omitted != 5) add(5, mac, destination_mac_length);
    if (omitted != 6) add(6, &period, sizeof(period));
}
static int start(void) {
    return mtk_cfg80211_vendor_packet_keep_alive_start(&phy, &dev, message + NLA_HDRLEN, bytes - NLA_HDRLEN);
}
static int stop(void) {
    return mtk_cfg80211_vendor_packet_keep_alive_stop(&phy, &dev, message + NLA_HDRLEN, bytes - NLA_HDRLEN);
}
static void invalid(void) { ioctls = 0; assert(start() == -EINVAL); assert(!ioctls && !allocations); }
int main(void) {
    const unsigned int legal_lengths[] = {0, 1, 40, 255, 256};
    for (unsigned int i = 0; i < sizeof(legal_lengths) / sizeof(legal_lengths[0]); ++i) {
        unsigned int length = legal_lengths[i];
        build(0, length, length, 6, 6); ioctls = 0;
        assert(start() == 0 && ioctls == 1 && !allocations);
        assert(submitted.enable && submitted.index == 7 && submitted.u2IpPktLen == length);
        assert(submitted.u4PeriodMsec == 12345);
        for (unsigned int j = 0; j < length; ++j) assert(submitted.pIpPkt[j] == (uint8_t)j);
        for (unsigned int j = 0; j < 6; ++j) {
            assert(submitted.ucSrcMacAddr[j] == j + 2 && submitted.ucDstMacAddr[j] == j + 2);
        }
    }
    puts("PASS legal payload lengths and complete firmware command fields");
    build(0, 257, 257, 6, 6); invalid();
    build(0, 65535, 256, 6, 6); invalid();
    build(0, 40, 39, 6, 6); invalid();
    build(0, 40, 40, 5, 6); invalid();
    build(0, 40, 40, 6, 5); invalid();
    for (unsigned int field = 1; field <= 6; ++field) { build(field, 40, 40, 6, 6); invalid(); }
    puts("PASS oversized, undersized and missing payload attributes");
    build(0, 40, 40, 6, 6); message[bytes++] = 0; invalid();
    begin(); uint8_t index = 7; add(1, &index, 1); ioctls = 0;
    assert(stop() == 0 && ioctls == 1 && !allocations && !submitted.enable && submitted.index == 7);
    begin(); add(2, &index, 0); ioctls = 0;
    assert(stop() == -EINVAL && !ioctls && !allocations);
    begin(); add(1, &index, 0);
    assert(stop() == -EINVAL && !ioctls && !allocations);
    assert(mtk_cfg80211_vendor_packet_keep_alive_start(&phy, &dev, NULL, 0) == -EINVAL);
    assert(mtk_cfg80211_vendor_packet_keep_alive_stop(&phy, &dev, message, -1) == -EINVAL);
    puts("PASS complete attribute framing and stop requires a valid index");
    build(0, 40, 40, 6, 6); fail_allocation = 1; ioctls = 0;
    assert(start() == -ENOMEM && !ioctls && !allocations);
    fail_allocation = 0; ioctl_result = 42;
    assert(start() == 42 && ioctls == 1 && !allocations);
    puts("PASS allocation and firmware-call failure release ownership");
}
''')
args.scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="keepalive-", dir=args.scratch) as temporary:
    directory = Path(temporary)
    unit = directory / "test.c"
    unit.write_text("\n".join(parts))
    executable = directory / "test"
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(unit), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
