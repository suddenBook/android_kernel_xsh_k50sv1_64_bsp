/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Dongwoon DW9761 VCM variant. The k50sv1 IMX145 HAL selects BU6424AF;
 * this register-addressed implementation is not interchangeable with it
 * and is disabled in the board configuration.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/uaccess.h>

#include "lens_info.h"
#include "k50_dw9761af.h"

#define AF_DRVNAME K50_DW9761AF_DRVNAME
#define AF_I2C_SLAVE_ADDR K50_DW9761AF_I2C_WRITE

#define LOG_INF(format, args...) \
	pr_debug(AF_DRVNAME " [%s] " format, __func__, ##args)

static struct i2c_client *g_pstAF_I2Cclient;
static int *g_pAF_Opened;
static spinlock_t *g_pAF_SpinLock;

static unsigned long g_u4AF_INF;
static unsigned long g_u4AF_MACRO = K50_DW9761AF_MACRO;
static unsigned long g_u4TargetPosition;
static unsigned long g_u4CurrPosition;

static int s4AF_Write(u8 reg, u8 val)
{
	u8 buf[2];
	int ret;

	if (!g_pstAF_I2Cclient || !k50_dw9761af_addr_ok(AF_I2C_SLAVE_ADDR))
		return -EINVAL;

	g_pstAF_I2Cclient->addr = AF_I2C_SLAVE_ADDR >> 1;
	buf[0] = reg;
	buf[1] = val;
	ret = i2c_master_send(g_pstAF_I2Cclient, buf, 2);
	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int s4AF_Read(u8 reg, u8 *val)
{
	u8 addr = reg;
	int ret;
	struct i2c_msg msgs[2];

	if (!g_pstAF_I2Cclient || !val)
		return -EINVAL;

	g_pstAF_I2Cclient->addr = AF_I2C_SLAVE_ADDR >> 1;
	msgs[0].addr = g_pstAF_I2Cclient->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &addr;
	msgs[1].addr = g_pstAF_I2Cclient->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = val;
	ret = i2c_transfer(g_pstAF_I2Cclient->adapter, msgs, 2);
	return ret != 2 ? (ret < 0 ? ret : -EIO) : 0;
}

static int s4AF_WriteReg(u16 pos)
{
	u8 buf[3];
	int ret;

	if (!g_pstAF_I2Cclient || !k50_dw9761af_addr_ok(AF_I2C_SLAVE_ADDR))
		return -EINVAL;

	LOG_INF("------write pos register-----\n");
	g_pstAF_I2Cclient->addr = AF_I2C_SLAVE_ADDR >> 1;
	buf[0] = K50_DW9761AF_REG_POS;
	buf[1] = k50_dw9761af_pos_msb(pos);
	buf[2] = k50_dw9761af_pos_lsb(pos);
	ret = i2c_master_send(g_pstAF_I2Cclient, buf, 3);
	if (ret != 3) {
		LOG_INF("I2C send failed!!\n");
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

static int s4AF_ReadReg(unsigned short *result)
{
	u8 msb;
	u8 lsb;
	int ret;

	if (!result)
		return -EINVAL;
	ret = s4AF_Read(K50_DW9761AF_REG_POS, &msb);
	if (ret)
		return ret;
	ret = s4AF_Read(K50_DW9761AF_REG_POS + 1, &lsb);
	if (ret)
		return ret;
	*result = k50_dw9761af_pos_from_bytes(msb, lsb);
	return 0;
}

static int initAF(void)
{
	int ret;

	ret = s4AF_Write(K50_DW9761AF_REG_PD, K50_DW9761AF_PD_SHUTDOWN);
	if (ret)
		return ret;
	msleep(1);
	ret = s4AF_Write(K50_DW9761AF_REG_PD, K50_DW9761AF_PD_ACTIVE);
	if (ret)
		return ret;
	msleep(1);
	ret = s4AF_Write(K50_DW9761AF_REG_PD, K50_DW9761AF_PD_RING);
	if (ret)
		return ret;
	ret = s4AF_Write(K50_DW9761AF_REG_MODE, K50_DW9761AF_SAC);
	if (ret)
		return ret;
	ret = s4AF_Write(K50_DW9761AF_REG_FREQ, K50_DW9761AF_FREQ);
	if (ret)
		return ret;
	return s4AF_Write(K50_DW9761AF_REG_PRELOAD, K50_DW9761AF_PRELOAD);
}

static inline int getAFInfo(__user struct stAF_MotorInfo *pstMotorInfo)
{
	struct stAF_MotorInfo stMotorInfo;

	if (!g_pAF_Opened)
		return -ENODEV;
	stMotorInfo.u4MacroPosition = g_u4AF_MACRO;
	stMotorInfo.u4InfPosition = g_u4AF_INF;
	stMotorInfo.u4CurrentPosition = g_u4CurrPosition;
	stMotorInfo.bIsSupportSR = 1;
	stMotorInfo.bIsMotorMoving = 1;
	stMotorInfo.bIsMotorOpen = *g_pAF_Opened >= 1;
	if (copy_to_user(pstMotorInfo, &stMotorInfo,
			 sizeof(struct stAF_MotorInfo))) {
		LOG_INF("copy to user failed when getting motor information\n");
		return -EFAULT;
	}
	return 0;
}

static inline int moveAF(unsigned long a_u4Position)
{
	int ret = 0;

	if (!g_pAF_Opened || !g_pAF_SpinLock || !g_pstAF_I2Cclient)
		return -ENODEV;
	if ((a_u4Position > g_u4AF_MACRO) || (a_u4Position < g_u4AF_INF)) {
		LOG_INF("out of range\n");
		return -EINVAL;
	}

	if (*g_pAF_Opened == 1) {
		unsigned short init_pos;

		ret = initAF();
		if (ret)
			return ret;
		if (s4AF_ReadReg(&init_pos) == 0) {
			spin_lock(g_pAF_SpinLock);
			g_u4CurrPosition = init_pos;
			spin_unlock(g_pAF_SpinLock);
		} else {
			spin_lock(g_pAF_SpinLock);
			g_u4CurrPosition = 0;
			spin_unlock(g_pAF_SpinLock);
		}
		spin_lock(g_pAF_SpinLock);
		*g_pAF_Opened = 2;
		spin_unlock(g_pAF_SpinLock);
	}

	if (g_u4CurrPosition == a_u4Position)
		return 0;

	spin_lock(g_pAF_SpinLock);
	g_u4TargetPosition = a_u4Position;
	spin_unlock(g_pAF_SpinLock);
	LOG_INF("move [curr] %ld [target] %ld\n",
		g_u4CurrPosition, g_u4TargetPosition);

	if (s4AF_WriteReg((unsigned short)g_u4TargetPosition) == 0) {
		spin_lock(g_pAF_SpinLock);
		g_u4CurrPosition = g_u4TargetPosition;
		spin_unlock(g_pAF_SpinLock);
	} else {
		ret = -1;
	}
	return ret;
}

static inline int setAFInf(unsigned long a_u4Position)
{
	spin_lock(g_pAF_SpinLock);
	g_u4AF_INF = a_u4Position;
	spin_unlock(g_pAF_SpinLock);
	return 0;
}

static inline int setAFMacro(unsigned long a_u4Position)
{
	spin_lock(g_pAF_SpinLock);
	g_u4AF_MACRO = a_u4Position;
	spin_unlock(g_pAF_SpinLock);
	return 0;
}

long DW9761AF_Ioctl(struct file *a_pstFile, unsigned int a_u4Command,
	unsigned long a_u4Param)
{
	long i4RetValue = 0;

	(void)a_pstFile;
	switch (a_u4Command) {
	case AFIOC_G_MOTORINFO:
		i4RetValue = getAFInfo(
			(__user struct stAF_MotorInfo *)a_u4Param);
		break;
	case AFIOC_T_MOVETO:
		i4RetValue = moveAF(a_u4Param);
		break;
	case AFIOC_T_SETINFPOS:
		i4RetValue = setAFInf(a_u4Param);
		break;
	case AFIOC_T_SETMACROPOS:
		i4RetValue = setAFMacro(a_u4Param);
		break;
	default:
		LOG_INF("No CMD\n");
		i4RetValue = -EPERM;
		break;
	}
	return i4RetValue;
}

int DW9761AF_Release(struct inode *a_pstInode, struct file *a_pstFile)
{
	(void)a_pstInode;
	(void)a_pstFile;
	if (!g_pAF_Opened || !g_pAF_SpinLock)
		return -ENODEV;
	LOG_INF("Start\n");
	if (*g_pAF_Opened == 2) {
		LOG_INF("Wait\n");
		s4AF_WriteReg(200);
		msleep(20);
		s4AF_WriteReg(100);
		msleep(20);
	}
	if (*g_pAF_Opened) {
		LOG_INF("Free\n");
		spin_lock(g_pAF_SpinLock);
		*g_pAF_Opened = 0;
		spin_unlock(g_pAF_SpinLock);
	}
	LOG_INF("End\n");
	return 0;
}

int DW9761AF_SetI2Cclient(struct i2c_client *pstAF_I2Cclient,
	spinlock_t *pAF_SpinLock, int *pAF_Opened)
{
	if (!pstAF_I2Cclient || !pAF_SpinLock || !pAF_Opened)
		return -EINVAL;
	g_pstAF_I2Cclient = pstAF_I2Cclient;
	g_pAF_SpinLock = pAF_SpinLock;
	g_pAF_Opened = pAF_Opened;
	return 1;
}
