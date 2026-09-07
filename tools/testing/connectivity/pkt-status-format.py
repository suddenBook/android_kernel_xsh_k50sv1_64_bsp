#!/usr/bin/env python3
"""Exercise the driver's actual packet-status formatter at buffer boundaries."""

import argparse
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = "drivers/misc/mediatek/connectivity/source/wlan/core/gen2/common/debug.c"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--revision", help="read the formatter from this Git revision")
args = parser.parse_args()
source = (subprocess.check_output(["git", "show", f"{args.revision}:{SOURCE}"], cwd=ROOT).decode()
          if args.revision else (ROOT / SOURCE).read_text())
start = source.index("VOID wlanPktStatusDebugDumpInfo(")
formatter = source[start:source.index("\n#if CFG_SUPPORT_EMI_DEBUG", start)]
start = source.index("typedef struct _PKT_STATUS_ENTRY")
types = source[start:source.index("#define TC_RELEASE_TRACE_BUF_MAX_NUM", start)]
constants = "\n".join(re.findall(r"^#define PKT_STATUS_(?:BUF_MAX_NUM|MSG_GROUP_RANGE|MSG_LENGTH).*", source, re.M))
harness = r'''
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef void *P_ADAPTER_T;
typedef uint8_t UINT_8;
typedef uint16_t UINT_16;
typedef uint32_t UINT_32;
typedef uint64_t UINT_64;
#define FALSE 0
#define kalMemCopy memcpy
#define kalMemSet memset
#define kalMemZero(p, n) memset(p, 0, n)
__TYPES__
__CONSTANTS__
static PKT_STATUS_RECORD grPktStaRec;
static UINT_16 gau2PktSeq[PKT_STATUS_BUF_MAX_NUM];
static UINT_32 u4PktSeqCount;
static char messages[100000];
static size_t messages_len;

static int host_format(void *dst, size_t size, const char *fmt, va_list ap) {
    if (size > PKT_STATUS_MSG_LENGTH) {
        fprintf(stderr, "invalid formatter size: %zu\n", size);
        abort();
    }
    return vsnprintf(dst, size, fmt, ap);
}
static int kalSnprintf(void *dst, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int result = host_format(dst, size, fmt, ap);
    va_end(ap);
    return result;
}
static int scnprintf(void *dst, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int result = host_format(dst, size, fmt, ap);
    va_end(ap);
    return result < (int)size ? result : (size ? (int)size - 1 : 0);
}
static void host_log(const char *level, const char *fmt, ...) {
    if (strcmp(level, "INFO")) return;
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(messages + messages_len, sizeof(messages) - messages_len, fmt, ap);
    va_end(ap);
    assert(len >= 0 && (size_t)len < sizeof(messages) - messages_len);
    messages_len += len;
}
#define DBGLOG(module, level, ...) host_log(#level, __VA_ARGS__)
__FORMATTER__

int main(void) {
    static PKT_STATUS_ENTRY tx[PKT_STATUS_BUF_MAX_NUM], rx[PKT_STATUS_BUF_MAX_NUM];
    static const unsigned counts[] = {0, 1, 179, 180, 220, 449, 450};
    grPktStaRec.pTxPkt = tx;
    grPktStaRec.pRxPkt = rx;
    for (unsigned i = 0; i < PKT_STATUS_BUF_MAX_NUM; ++i) {
        tx[i].u1Type = rx[i].u1Type = 255;
        tx[i].u2IpId = rx[i].u2IpId = 65535;
        tx[i].status = rx[i].status = 255;
        tx[i].u4pktXmitTime = (uint64_t)i * 3000000;
        tx[i].u4ProcessTimeDiff = INT_MAX;
    }
    for (unsigned trial = 0; trial < sizeof(counts) / sizeof(counts[0]); ++trial) {
        unsigned n = counts[trial];
        messages[0] = 0;
        messages_len = 0;
        grPktStaRec.u4TxIndex = grPktStaRec.u4RxIndex = 449;
        u4PktSeqCount = n;
        for (unsigned i = 0; i < n; ++i) gau2PktSeq[i] = 65535;
        wlanPktStatusDebugDumpInfo(NULL);
        assert(grPktStaRec.u4TxIndex == 0 && grPktStaRec.u4RxIndex == 0 && u4PktSeqCount == 0);
        char *seq = strstr(messages, "RX Seq count:");
        assert(seq);
        unsigned found = 0;
        for (char *p = seq; (p = strstr(p, "ffff,")); p += 5) ++found;
        assert(found == n);
        unsigned entries = 0;
        for (char *p = messages; (p = strstr(p, "255,ffff,ff,")); ++p) ++entries;
        assert(entries == 449);
        entries = 0;
        for (char *p = messages; (p = strstr(p, "255,ffff,ff ")); ++p) ++entries;
        assert(entries == 449);
        for (unsigned i = 0; i < PKT_STATUS_BUF_MAX_NUM; ++i) assert(gau2PktSeq[i] == 0);
        printf("PASS sequence_count=%u, complete TX/RX records, bounded output\n", n);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="pkt-status-", dir=pathlib.Path(__file__).parent) as temp:
    directory = pathlib.Path(temp)
    test = directory / "test.c"
    test.write_text(harness.replace("__TYPES__", types).replace("__CONSTANTS__", constants)
                   .replace("__FORMATTER__", formatter))
    executable = directory / "test"
    subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", str(test), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
