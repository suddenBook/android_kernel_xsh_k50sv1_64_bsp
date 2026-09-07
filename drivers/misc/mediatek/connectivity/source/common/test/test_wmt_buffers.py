#!/usr/bin/env python3
"""Run source-derived WMT caller-buffer ownership checks without hardware."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


CASES = {
    0: "gps-completed-output-and-wire-request",
    1: "gps-timeout-keeps-original-input",
    2: "gps-timeout-isolates-reused-output",
    3: "gps-cancel-keeps-original-input",
    4: "gps-cancel-isolates-reused-output",
    5: "gps-late-completion-does-not-copy-output",
    6: "gps-completion-after-cancel-does-not-copy-output",
    7: "gps-tx-failure-keeps-output",
    8: "gps-short-tx-keeps-output",
    9: "gps-rx-failure-keeps-output",
    10: "gps-wrong-opcode-keeps-output",
    11: "gps-payload-allocation-failure",
    12: "gps-tx-allocation-failure",
    13: "gps-rx-allocation-failure",
    14: "gps-psm-failure-releases-payload",
    15: "gps-queue-failure-releases-payload",
    16: "gps-coredump-rejection-releases-payload",
    17: "loopback-completed-output",
    18: "adie-completed-output",
    19: "loopback-output-survives-second-caller",
    20: "adie-output-survives-second-caller",
    21: "loopback-timeout-keeps-original-input",
    22: "bgw-completed-input",
    23: "bgw-timeout-keeps-original-input",
    24: "ant-completed-input",
    25: "ant-timeout-keeps-original-input",
    26: "ant-cancel-keeps-original-input",
    27: "flash-completed-input",
    28: "flash-timeout-keeps-original-input",
    29: "flash-cancel-keeps-original-input",
    30: "idc-completed-wire-payload",
    31: "idc-timeout-keeps-original-payload",
    32: "idc-cancel-keeps-original-payload",
    33: "retained-submit-keeps-payload-until-release",
    34: "retained-timeout-keeps-consumer-payload",
    35: "retained-cancel-keeps-consumer-payload",
    36: "payload-allocation-starts-zeroed",
    37: "payload-rejects-second-allocation",
    38: "unsubmitted-payload-released-on-free",
    39: "async-payload-released-by-final-consumer",
    40: "loopback-copy-output-failure-releases-payload",
    41: "adie-copy-output-failure-releases-payload",
    42: "loopback-oversized-result-releases-payload",
    43: "adie-oversized-result-releases-payload",
    44: "loopback-allocation-failure",
    45: "adie-allocation-failure",
    46: "bgw-allocation-failure",
    47: "ant-allocation-failure",
    48: "flash-allocation-failure",
    49: "idc-allocation-failure",
    50: "gps-short-header-keeps-output",
    51: "gps-short-payload-keeps-output",
    52: "gps-announced-overrun-keeps-output",
    53: "gps-reported-overrun-keeps-output",
    54: "gps-empty-reply-copies-zero-bytes",
    55: "coex-completed-output",
    56: "coex-timeout-isolates-later-result",
    57: "coex-cancel-isolates-later-result",
    58: "coex-output-survives-second-caller",
    59: "coex-allocation-failure",
    60: "loopback-header-copy-failure",
    61: "loopback-payload-copy-failure-releases-allocation",
    62: "bgw-copy-failure-releases-allocation",
    63: "gps-null-length-pointer-rejected",
    64: "gps-tx-wire-length-overflow-rejected",
    65: "gps-rx-wire-length-overflow-rejected",
    66: "idc-null-message-rejected",
    67: "idc-short-header-rejected",
    68: "idc-empty-payload-rejected",
    69: "idc-oversized-payload-rejected",
    70: "flash-null-version-rejected",
    71: "payload-allocation-size-truncation-rejected",
}


def function(source, name, required=True):
    """Extract one complete, top-level production function, without rewriting it."""
    pattern = r"(?m)^[A-Za-z_][^;\n]*\b" + re.escape(name) + r"\([^;{}]*\)\n\{"
    match = re.search(pattern, source)
    if not match:
        if required:
            raise ValueError(f"Missing production function: {name}")
        return ""
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end] + "\n"


def declaration(source, kind, tag):
    return re.search(r"^typedef " + kind + r" " + tag + r" \{.*?^\}[^;]*;",
                     source, re.M | re.S)[0] + "\n"


def anonymous_enum(source, name):
    matches = re.findall(r"^typedef enum \{.*?^\}[^;]*;", source, re.M | re.S)
    return next(value + "\n" for value in matches
                if re.search(r"\}\s*" + re.escape(name) + r"\s*;", value))


def build_fixture(sources, mapping):
    osal, lib, exp = (sources[name] for name in ("osal", "lib", "exp"))
    owned = bool(function(lib, "wmt_lib_alloc_op_data", required=False))
    types = re.search(r"^#define OSAL_OP_DATA_SIZE[^\n]*", osal, re.M)[0] + "\n"
    types += "".join(declaration(osal, "struct", tag)
                     for tag in ("_OSAL_OP_DAT", "_OSAL_LXOP_"))
    types += "typedef OSAL_OP_DAT WMT_OP;\ntypedef P_OSAL_OP_DAT P_WMT_OP;\n"
    types += declaration(sources["core_h"], "enum", "_ENUM_WMT_OPID_T")
    types += "".join(declaration(sources["exp_h"], "enum", "_ENUM_WMT_" + tag + "_T")
                     for tag in ("FLASH_PATCH_CTRL", "FLASH_PATCH_SEQ", "FLASH_PATCH_TYPE",
                                 "FLASH_PATCH_STATUS", "ANT_RAM_CTRL", "ANT_RAM_SEQ", "ANT_RAM_STATUS"))
    types += declaration(sources["dbg_h"], "enum", "_ENUM_CMD_TYPE_T")
    types += declaration(sources["dbg_h"], "struct", "_COEX_BUF")
    types += anonymous_enum(sources["ipc_h"], "CCCI_IPC_MSG_ID_RANGE")
    types += anonymous_enum(sources["idc_h"], "WMT_IDC_TX_OPCODE")
    types += anonymous_enum(sources["idc_h"], "IPC_MSG_ID_CODE")
    types += declaration(sources["ipc_h"], "struct", "local_para")
    types += "typedef struct { UINT32 msg_id; struct local_para *local_para_ptr; } conn_md_ipc_ilm_t;\n"
    types += re.search(r"enum wmt_op_state \{.*?\};", lib, re.S)[0] + "\n"
    types += "".join(re.search(r"^#define " + name + r"[^\n]*", sources["lib_h"], re.M)[0] + "\n"
                     for name in ("WMT_IDC_MSG_BUFFER", "WMT_IDC_MSG_MAX_SIZE"))
    types += re.search(r"^#define LTE_MSG_ID_OFFSET[^\n]*", sources["idc_h"], re.M)[0] + "\n"
    types += "".join(re.search(r"^#define " + name + r"[^\n]*", sources["core_h"], re.M)[0] + "\n"
                     for name in ("WMT_LPBK_CMD_LEN", "WMT_LPBK_BUF_LEN"))
    functions = []
    for source_name, names in {
        "lib": ["wmt_lib_get_free_op", "wmt_lib_put_op_to_free_queue", "wmt_lib_alloc_op_data",
                "wmt_lib_put_op_ref", "wmt_lib_queue_op", "wmt_lib_submit_op_result", "wmt_lib_put_act_op_result",
                "wmt_lib_put_act_op", "wmt_lib_cancel_current_op", "wmt_lib_complete_op",
                "wmt_lib_gps_mcu_ctrl", "wmt_lib_handle_idc_msg"],
        "exp": ["mtk_wcn_wmt_ant_ram_ctrl", "mtk_wcn_wmt_flash_patch_ctrl"],
        "core": ["opfunc_gps_mcu_ctrl"],
        "dbg": ["wmt_dbg_cmd_test_api"],
    }.items():
        for name in names:
            optional = name in {"wmt_lib_alloc_op_data", "wmt_lib_put_op_ref", "wmt_lib_queue_op", "wmt_lib_submit_op_result"}
            value = function(sources[source_name], name, required=not optional)
            if value:
                functions.append(value)
    prototypes = "".join(value[:value.index("\n{")] + ";\n" for value in functions)
    branches = ""
    for name in ("WMT_IOCTL_LPBK_TEST", "WMT_IOCTL_ADIE_LPBK_TEST", "WMT_IOCTL_SEND_BGW_DS_CMD"):
        start = sources["dev"].index("\tcase " + name + ":")
        branches += sources["dev"][start:sources["dev"].index("\tcase ", start + 1)]
    ioctl = "static INT32 run_ioctl(UINT32 cmd, ULONG arg) { INT32 iRet = 0; switch (cmd) {\n" + branches
    ioctl += "\n default: return -EINVAL; } return iRet; }\n"
    host = Path(__file__).with_name("wmt_buffers_host.c").read_text()
    host = host.replace("/* SOURCE_TYPES */", types)
    host = host.replace("/* SOURCE_PROTOTYPES */", prototypes)
    host = host.replace("/* SOURCE_LOCKS */", "\n".join(re.findall(
        r"^static DEFINE_MUTEX\([^\n]*coex[^\n]*\);$", sources["dbg"], re.M)))
    host = host.replace("/* PRODUCTION */", "\n".join(functions) + ioctl)
    return (f"#define HAVE_PAYLOAD {int(owned)}\n#define HAVE_OP_POOL {int(bool(function(lib, 'wmt_lib_queue_op', required=False)))}\n#define CFG_WMT_LTE_ENABLE_MSGID_MAPPING {mapping}\n" + host,
            owned)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", help="Read an immutable git revision instead of worktree sources")
    parser.add_argument("--idc-mapping", type=int, choices=[0, 1], default=1)
    parser.add_argument("--case", action="append", help="Run only named cases (may be repeated)")
    args = parser.parse_args()
    common = "drivers/misc/mediatek/connectivity/source/common/common_main/"
    paths = {name: common + value for name, value in {
        "lib": "core/wmt_lib.c", "dev": "linux/wmt_dev.c", "core": "core/wmt_core.c",
        "exp": "core/wmt_exp.c", "osal": "linux/include/osal.h", "lib_h": "core/include/wmt_lib.h",
        "core_h": "core/include/wmt_core.h", "exp_h": "include/wmt_exp.h", "idc_h": "linux/include/wmt_idc.h",
        "dbg": "linux/wmt_dbg.c", "dbg_h": "linux/include/wmt_dbg.h",
    }.items()}
    paths["ipc_h"] = "drivers/misc/mediatek/eccci/port_ipc.h"
    source_bytes = {}
    for name, path in paths.items():
        source_bytes[name] = (subprocess.run(["git", "-C", str(args.kernel), "show", f"{args.revision}:{path}"],
                                            capture_output=True, check=True).stdout
                              if args.revision else (args.kernel / path).read_bytes())
    fixture, owned = build_fixture({name: value.decode() for name, value in source_bytes.items()}, args.idc_mapping)
    chosen = {number: name for number, name in CASES.items() if not args.case or name in args.case}
    if args.case and set(args.case) - set(chosen.values()):
        parser.error("Unknown case: " + ", ".join(sorted(set(args.case) - set(chosen.values()))))
    args.output.mkdir(parents=True, exist_ok=False)
    c_file, binary = args.output / "fixture.c", args.output / "fixture"
    c_file.write_text(fixture)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-Wno-unused-but-set-variable", "-Wno-sign-compare", "-Wno-pointer-sign", "-pthread",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie",
        str(c_file), "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / "compile.txt").write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stderr.decode(errors="replace"))
        raise SystemExit(compiled.returncode)
    rows = []
    for number, name in chosen.items():
        run = subprocess.run([str(binary), str(number)], capture_output=True, timeout=20,
                             env=dict(os.environ, ASAN_OPTIONS="detect_stack_use_after_return=1:detect_leaks=1:halt_on_error=1"))
        (args.output / (name + ".txt")).write_bytes(run.stdout + run.stderr)
        status = {0: "PASS", 77: "SKIP"}.get(run.returncode, "FAIL")
        rows.append(dict(case=name, exit_code=run.returncode, status=status))
        print(status + ": " + name, flush=True)
    result = dict(cases=rows, passed=sum(row["status"] == "PASS" for row in rows),
                  skipped=sum(row["status"] == "SKIP" for row in rows), total=len(rows),
                  source_revision=args.revision or "worktree", owned_payload_api=owned,
                  source_sha256={paths[name]: hashlib.sha256(value).hexdigest() for name, value in source_bytes.items()},
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  host_sha256=hashlib.sha256(Path(__file__).with_name("wmt_buffers_host.c").read_bytes()).hexdigest(),
                  fixture_sha256=hashlib.sha256(c_file.read_bytes()).hexdigest(), compiler=command,
                  sanitizer="address,undefined", idc_mapping=args.idc_mapping)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    raise SystemExit(any(row["status"] == "FAIL" for row in rows))


if __name__ == "__main__":
    main()
