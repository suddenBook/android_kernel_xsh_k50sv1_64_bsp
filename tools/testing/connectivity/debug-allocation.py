#!/usr/bin/env python3
"""Inject every allocation failure into the actual WLAN debug init/uninit pair."""
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
path = args.source or root / "drivers/misc/mediatek/connectivity/source/wlan/core/gen2/common/debug.c"
source = path.read_text()


def function(name):
    match = re.search(r"(?:VOID|BOOLEAN) " + name + r"\(VOID\)\n\{", source)
    start = match.start()
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


init = function("wlanDebugInit")
uninit = function("wlanDebugUninit")
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef bool BOOLEAN;
typedef uint16_t UINT_16;
#define TRUE true
#define FALSE false
#define PHY_MEM_TYPE 0
#define VIR_MEM_TYPE 1
#define CFG_SUPPORT_EMI_DEBUG 1
#define kalMemZero(p, n) memset(p, 0, n)
/* Only allocation and lifetime are exercised; field payloads are opaque here. */
typedef struct { uint64_t payload[4]; } TC_RES_RELEASE_ENTRY;
typedef TC_RES_RELEASE_ENTRY CMD_TRACE_ENTRY, COMMAND_ENTRY, PKT_INFO_ENTRY;
typedef TC_RES_RELEASE_ENTRY HIF_TX_DESC_T, BSS_TRACE_RECORD, PKT_STATUS_ENTRY;
struct COMMAND_DEBUG_INFO { uint64_t payload[4]; };
#define TC_RELEASE_TRACE_BUF_MAX_NUM 8
#define TXED_CMD_TRACE_BUF_MAX_NUM 8
#define TXED_COMMAND_BUF_MAX_NUM 8
#define PKT_INFO_BUF_MAX_NUM 8
#define NIC_TX_BUFF_COUNT_TC4 8
#define SCAN_TARGET_BSS_MAX_NUM 8
#define PKT_STATUS_BUF_MAX_NUM 450
static TC_RES_RELEASE_ENTRY *gprTcReleaseTraceBuffer;
static CMD_TRACE_ENTRY *gprCmdTraceEntry;
static COMMAND_ENTRY *gprCommandEntry;
static struct COMMAND_DEBUG_INFO *gprCommandDebugInfo;
static struct { PKT_INFO_ENTRY *pTxPkt, *pRxPkt; unsigned int u4TxIndex, u4RxIndex; } grPktRec;
static struct { PKT_STATUS_ENTRY *pTxPkt, *pRxPkt; unsigned int u4TxIndex, u4RxIndex; } grPktStaRec;
static struct { HIF_TX_DESC_T *pTxDescScanWriteBefore, *pTxDescScanWriteDone;
                unsigned int aucFreeBufCntScanWriteBefore, aucFreeBufCntScanWriteDone; } grScanHifDescRecord;
static struct { BSS_TRACE_RECORD *prBssTraceRecord; unsigned int u4BSSIDCount; } grScanTargetBssList;
static UINT_16 gau2PktSeq[PKT_STATUS_BUF_MAX_NUM];
static unsigned int u4PktSeqCount, gPrevIdxPagedtrace;
static void *allocated[11];
static size_t allocation_sizes[11];
static int allocation_types[11];
static int fail_at, allocation_calls, outstanding, log_initialized;
static void wlanDbgLogLevelInit(void) { assert(!log_initialized); log_initialized = 1; }
static void wlanDbgLogLevelUninit(void) { log_initialized = 0; }
static void *kalMemAlloc(size_t size, int type) {
    int index = allocation_calls++;
    assert(index < 11);
    if (index + 1 == fail_at) return NULL;
    allocated[index] = malloc(size); assert(allocated[index]);
    allocation_sizes[index] = size; allocation_types[index] = type;
    ++outstanding; return allocated[index];
}
static void kalMemFree(void *data, int type, size_t size) {
    if (!data) return;
    for (int i = 0; i < 11; ++i) {
        if (allocated[i] == data) {
            assert(allocation_sizes[i] == size && allocation_types[i] == type);
            allocated[i] = NULL; --outstanding; free(data); return;
        }
    }
    assert(!"free of stale or unowned debug buffer");
}
static void assert_clean(void) {
    assert(!outstanding && !log_initialized);
    assert(!gprTcReleaseTraceBuffer && !gprCmdTraceEntry && !gprCommandEntry && !gprCommandDebugInfo);
    assert(!grPktRec.pTxPkt && !grPktRec.pRxPkt && !grPktStaRec.pTxPkt && !grPktStaRec.pRxPkt);
    assert(!grScanHifDescRecord.pTxDescScanWriteBefore && !grScanHifDescRecord.pTxDescScanWriteDone);
    assert(!grScanTargetBssList.prBssTraceRecord);
}
VOID wlanDebugUninit(VOID);
'''
tests = r'''
int main(int argc, char **argv) {
    assert(argc == 2); fail_at = atoi(argv[1]);
    int result = invoke_init();
    if (fail_at) {
        assert(!result && allocation_calls == fail_at);
        assert_clean();
        /* A later probe must not inherit freed pointers from partial init. */
        fail_at = 0; allocation_calls = 0;
        assert(invoke_init());
    } else assert(result && outstanding == 11);
    wlanDebugUninit(); assert_clean();
    wlanDebugUninit(); assert_clean();
    for (int i = 0; i < 20; ++i) {
        allocation_calls = 0; assert(invoke_init());
        wlanDebugUninit(); assert_clean();
    }
    printf("PASS failure-index=%s plus retry, double cleanup and 20 cycles\n", argv[1]);
}
'''
invoke = "return wlanDebugInit();" if init.startswith("BOOLEAN") else "wlanDebugInit(); return TRUE;"
args.scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="wlan-debug-", dir=args.scratch) as temporary:
    unit = Path(temporary) / "test.c"
    unit.write_text(prefix + init + uninit + "\nstatic int invoke_init(void) { " + invoke + " }\n" + tests)
    binary = Path(temporary) / "test"
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(unit), "-o", str(binary)], check=True)
    failed = [n for n in range(12) if subprocess.run([str(binary), str(n)]).returncode]
    print(f"{12 - len(failed)}/12 cases pass; failures: {failed}", flush=True)
    raise SystemExit(bool(failed))
