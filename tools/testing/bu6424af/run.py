#!/usr/bin/env python3
"""Exercise the actual MAINAF selector and BU6424AF driver with fake I2C."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
lens = root / "drivers/misc/mediatek/lens/main"
driver = (lens / "common/bu6424af/BU6424AF.c").read_text()
driver = re.sub(r'^#include .*$', '', driver, flags=re.M)
main = (lens / "main_lens.c").read_text()
table = main[main.index("static struct stAF_DrvList g_stAF_DrvList"):]
table = table[:table.index("};") + 2]
selector = main[main.index("static long AF_SetMotorName("):]
selector = selector[:selector.index("\n}") + 2]
config = (root / "arch/arm64/configs/k50sv1_64_bsp_source.fragment").read_text()
defines = "\n".join("#define " + m + " 1" for m in
                    re.findall(r'^(CONFIG_MTK_LENS_\w+)=y$', config, re.M))
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int spinlock_t;
struct file { int unused; };
struct inode { int unused; };
struct i2c_client { unsigned addr; };
#define __user
#define pr_debug(...) ((void)0)
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
#define spin_lock(p) ((void)(p))
#define spin_unlock(p) ((void)(p))
#include "lens_info.h"
static int send_result = 2, recv_result = 2, writes, copy_error;
static unsigned char sent[2];
static int i2c_master_send(struct i2c_client *c, const char *data, int size) {
    (void)c;
    if (size != 2) return -EINVAL;
    memcpy(sent, data, 2); writes++;
    return send_result;
}
static int i2c_master_recv(struct i2c_client *c, char *data, int size) {
    (void)c;
    if (size != 2) return -EINVAL;
    if (recv_result > 0) data[0] = 0x02;
    if (recv_result > 1) data[1] = 0x34;
    return recv_result;
}
static int copy_to_user(void *to, const void *from, size_t n) {
    if (copy_error) return 1;
    memcpy(to, from, n); return 0;
}
#define copy_from_user copy_to_user
'''
selection_prefix = r'''
#define g_pstAF_I2Cclient selector_client
static struct i2c_client chip;
static struct i2c_client *selector_client = &chip;
static struct stAF_DrvList *g_pstAF_CurDrv;
static spinlock_t g_AF_SpinLock;
static int g_s4AF_Opened = 1;
static int DW9761AF_SetI2Cclient(struct i2c_client *c, spinlock_t *l, int *o) {
    (void)c; (void)l; (void)o; return -ENODEV;
}
static long DW9761AF_Ioctl(struct file *f, unsigned int c, unsigned long p) {
    (void)f; (void)c; (void)p; return -ENOTTY;
}
static int DW9761AF_Release(struct inode *i, struct file *f) {
    (void)i; (void)f; return 0;
}
'''
tests = r'''
static int failures, checks;
#define CHECK(name, expr) do { checks++; if (!(expr)) { \
    failures++; printf("FAIL %s\n", name); } } while (0)
int main(void) {
    struct stAF_MotorName name = { "BU6424AF" };
    struct stAF_MotorInfo info;
    int before;
    CHECK("HAL IMX145 lens selection", AF_SetMotorName(&name) == 1);
    BU6424AF_SetI2Cclient(&chip, &g_AF_SpinLock, &g_s4AF_Opened);
    CHECK("position command", BU6424AF_Ioctl(NULL, AFIOC_T_MOVETO, 0x2ab) == 0);
    CHECK("BU6424 wire protocol", chip.addr == 0x0c && sent[0] == 0xc2 && sent[1] == 0xab);
    CHECK("motor state", getAFInfo(&info) == 0 && info.u4CurrentPosition == 0x2ab);
    send_result = 1;
    CHECK("short write fails", moveAF(400) == -EIO);
    CHECK("failed write preserves position", getAFInfo(&info) == 0 && info.u4CurrentPosition == 0x2ab);
    send_result = -ENXIO;
    for (int i = 0; i < 4; i++) CHECK("bus errno preserved", moveAF(400) == -ENXIO);
    send_result = 2;
    CHECK("transient failure recovers", moveAF(400) == 0);
    g_s4AF_Opened = 1; recv_result = 1; before = writes;
    CHECK("short initial read fails", moveAF(200) == -EIO);
    CHECK("failed read preserves unopened state", g_s4AF_Opened == 1 && writes == before);
    recv_result = -ENXIO;
    CHECK("read errno preserved", moveAF(200) == -ENXIO);
    recv_result = 2;
    CHECK("read retry recovers", moveAF(200) == 0);
    copy_error = 1;
    CHECK("bad user info pointer fails", getAFInfo(&info) == -EFAULT);
    copy_error = 0;
    memset(&name, 'X', sizeof(name));
    CHECK("unterminated lens name rejected", AF_SetMotorName(&name) == -EINVAL);
    CHECK("invalid lens range rejected", setAFMacro(1024) == -EINVAL && setAFInf(1024) == -EINVAL);
    CHECK("shutdown", BU6424AF_Release(NULL, NULL) == 0 && sent[0] == 0 && sent[1] == 0);
    g_s4AF_Opened = 2; send_result = 1;
    CHECK("short shutdown fails", BU6424AF_Release(NULL, NULL) == -EIO);
    CHECK("failed shutdown releases ownership", g_s4AF_Opened == 0);
    g_s4AF_Opened = 2; send_result = -ENXIO;
    CHECK("shutdown bus errno preserved", BU6424AF_Release(NULL, NULL) == -ENXIO);
    CHECK("bus failure releases ownership", g_s4AF_Opened == 0);
    printf("BU6424AF: %d/%d passed\n", checks - failures, checks);
    return failures != 0;
}
'''
with tempfile.TemporaryDirectory(prefix="bu6424af-") as tmp:
    source = Path(tmp) / "test.c"
    source.write_text(prefix + defines + "\n" + driver + selection_prefix + table + selector + tests)
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(lens / "inc"), str(source), "-o", str(exe)], check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
