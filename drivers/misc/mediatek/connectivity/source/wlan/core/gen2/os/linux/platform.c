/*
* Copyright (C) 2016 MediaTek Inc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See http://www.gnu.org/licenses/gpl-2.0.html for more details.
*/

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/
#include <linux/version.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/fs.h>

#include <linux/uaccess.h>

#include "gl_os.h"

#ifndef CONFIG_X86
#if defined(CONFIG_HAS_EARLY_SUSPEND)
#include <linux/earlysuspend.h>
#endif
#endif

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/
#if CFG_TC10_FEATURE
#define WIFI_NVRAM_FILE_NAME   "/vendor/firmware/WIFI"
#else
#define WIFI_NVRAM_FILE_NAME   "/mnt/vendor/nvdata/APCFG/APRDEB/WIFI"
#endif
#define WIFI_NVRAM_CUSTOM_NAME "/mnt/vendor/nvdata/APCFG/APRDEB/WIFI_CUSTOM"

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

/*******************************************************************************
*                   F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/

static int netdev_event(struct notifier_block *nb, unsigned long notification, void *ptr)
{
	struct in_ifaddr *ifa = ptr;
	struct net_device *prDev;
	P_GLUE_INFO_T prGlueInfo;
	UINT_8 ip[4] = { 0 };
	BOOLEAN fgAddress, fgIsP2p;
	WLAN_STATUS rStatus;

	if (!fgIsUnderSuspend || !ifa || !ifa->ifa_dev || !ifa->ifa_dev->dev)
		return NOTIFY_DONE;
	prDev = ifa->ifa_dev->dev;
	fgIsP2p = strncmp(prDev->name, "p2p", 3) == 0;
	if (!fgIsP2p && strncmp(prDev->name, "wlan", 4) != 0)
		return NOTIFY_DONE;
	prGlueInfo = *((P_GLUE_INFO_T *)netdev_priv(prDev));
	if (!prGlueInfo || !prGlueInfo->prAdapter)
		return NOTIFY_DONE;

	fgAddress = kalGetIPv4Address(prDev, ip);
	rStatus = kalSetIPv4Address(prGlueInfo, fgAddress ? ip : NULL, fgIsP2p);
	if (rStatus != WLAN_STATUS_SUCCESS)
		DBGLOG(REQ, ERROR, "update IPv4 filter fail 0x%x\n", rStatus);
	return NOTIFY_DONE;
}

/* #if CFG_SUPPORT_HOTSPOT_2_0 */
#if 0
static int net6dev_event(struct notifier_block *nb, unsigned long notification, void *ptr)
{
	struct inet6_ifaddr *ifa = (struct inet6_ifaddr *)ptr;
	struct net_device *prDev = ifa->idev->dev;
	P_GLUE_INFO_T prGlueInfo = NULL;

	if (prDev == NULL) {
		DBGLOG(REQ, INFO, "net6dev_event: device is empty.\n");
		return NOTIFY_DONE;
	}

	if ((strncmp(prDev->name, "p2p", 3) != 0) && (strncmp(prDev->name, "wlan", 4) != 0)) {
		DBGLOG(REQ, INFO, "net6dev_event: xxx\n");
		return NOTIFY_DONE;
	}

	if (strncmp(prDev->name, "p2p", 3) == 0) {
		/* because we store the address of prGlueInfo in p2p's private date of net device */
		/* *((P_GLUE_INFO_T *) netdev_priv(prGlueInfo->prP2PInfo->prDevHandler)) = prGlueInfo; */
		prGlueInfo = *((P_GLUE_INFO_T *) netdev_priv(prDev));
	} else {		/* wlan0 */
		prGlueInfo = (P_GLUE_INFO_T) netdev_priv(prDev);
	}

	if (prGlueInfo == NULL) {
		DBGLOG(REQ, INFO, "netdev_event: prGlueInfo is empty.\n");
		return NOTIFY_DONE;
	}
	/* printk(KERN_INFO "[net6dev_event] IPV6_DAD is unlock now!!\n"); */
	prGlueInfo->fgIs6Dad = FALSE;

	return NOTIFY_DONE;
}
#endif

static struct notifier_block inetaddr_notifier = {
	.notifier_call = netdev_event,
};

#if 0				/* CFG_SUPPORT_HOTSPOT_2_0 */
static struct notifier_block inet6addr_notifier = {
	.notifier_call = net6dev_event,
};
#endif

void wlanRegisterNotifier(void)
{
	register_inetaddr_notifier(&inetaddr_notifier);

#if CFG_SUPPORT_HOTSPOT_2_0
	/* register_inet6addr_notifier(&inet6addr_notifier); */
#endif
}

/* EXPORT_SYMBOL(wlanRegisterNotifier); */

void wlanUnregisterNotifier(void)
{
	unregister_inetaddr_notifier(&inetaddr_notifier);

#if CFG_SUPPORT_HOTSPOT_2_0
	/* unregister_inetaddr_notifier(&inet6addr_notifier); */
#endif
}

/* EXPORT_SYMBOL(wlanUnregisterNotifier); */

#if 0
/*----------------------------------------------------------------------------*/
/*!
* \brief Utility function for reading data from files on NVRAM-FS
*
* \param[in]
*           filename
*           len
*           offset
* \param[out]
*           buf
* \return
*           actual length of data being read
*/
/*----------------------------------------------------------------------------*/
/* (Wayne) TODO: merge these two functions into one. */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0))
static int nvram_read(char *filename, char *buf, ssize_t len, int offset)
{
#if CFG_SUPPORT_NVRAM
	struct file *fd;
	int retLen = -1;
	loff_t pos;
	char __user *p;

	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);

	fd = filp_open(filename, O_RDONLY, 0644);

	if (IS_ERR(fd)) {
		DBGLOG(INIT, INFO, "[MT6620][nvram_read] : failed to open!!\n");
		return -1;
	}

	do {
		if (fd->f_op == NULL) {
			DBGLOG(INIT, INFO, "[MT6620][nvram_read] : f_op is NULL!!\n");
			break;
		}

		if (fd->f_pos != offset) {
			if (fd->f_op->llseek) {
				if (fd->f_op->llseek(fd, offset, 0) != offset) {
					DBGLOG(INIT, INFO, "[MT6620][nvram_read] : failed to seek!!\n");
					break;
				}
			} else {
				fd->f_pos = offset;
			}
		}

		p = (__force char __user *)buf;
		pos = (loff_t)offset;
		retLen = __vfs_read(fd, p, len, &pos);
		if (retLen < 0)
			DBGLOG(INIT, ERROR, "[MT6620][nvram_read] : read failed!! Error code: %d\n", retLen);

	} while (FALSE);

	filp_close(fd, NULL);

	set_fs(old_fs);

	return retLen;

#else /* !CFG_SUPPORT_NVRAM */

	return -EIO;

#endif
}
#else
static int nvram_read(char *filename, char *buf, ssize_t len, int offset)
{
#if CFG_SUPPORT_NVRAM
	struct file *fd;
	int retLen = -1;

	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);

	fd = filp_open(filename, O_RDONLY, 0644);

	if (IS_ERR(fd)) {
		DBGLOG(INIT, INFO, "[MT6620][nvram_read] : failed to open!!\n");
		set_fs(old_fs);
		return -1;
	}

	do {
		if ((fd->f_op == NULL) || (fd->f_op->read == NULL)) {
			DBGLOG(INIT, INFO, "[MT6620][nvram_read] : file can not be read!!\n");
			break;
		}

		if (fd->f_pos != offset) {
			if (fd->f_op->llseek) {
				if (fd->f_op->llseek(fd, offset, 0) != offset) {
					DBGLOG(INIT, INFO, "[MT6620][nvram_read] : failed to seek!!\n");
					break;
				}
			} else {
				fd->f_pos = offset;
			}
		}

		retLen = fd->f_op->read(fd, buf, len, &fd->f_pos);

	} while (FALSE);

	filp_close(fd, NULL);

	set_fs(old_fs);

	return retLen;

#else /* !CFG_SUPPORT_NVRAM */

	return -EIO;

#endif
}
#endif
/*----------------------------------------------------------------------------*/
/*!
* \brief Utility function for writing data to files on NVRAM-FS
*
* \param[in]
*           filename
*           buf
*           len
*           offset
* \return
*           actual length of data being written
*/
/*----------------------------------------------------------------------------*/
/* (Wayne) TODO: merge these two functions into one. */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0))
static int nvram_write(char *filename, char *buf, ssize_t len, int offset)
{
#if CFG_SUPPORT_NVRAM
	struct file *fd;
	int retLen = -1;
	loff_t pos;
	char __user *p;

	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);

	fd = filp_open(filename, O_WRONLY | O_CREAT, 0644);

	if (IS_ERR(fd)) {
		DBGLOG(INIT, INFO, "[MT6620][nvram_write] : failed to open!!\n");
		return -1;
	}

	do {
		if (fd->f_op == NULL) {
			DBGLOG(INIT, INFO, "[MT6620][nvram_write] : f_op is NULL!!\n");
			break;
		}
		/* End of if */
		if (fd->f_pos != offset) {
			if (fd->f_op->llseek) {
				if (fd->f_op->llseek(fd, offset, 0) != offset) {
					DBGLOG(INIT, INFO, "[MT6620][nvram_write] : failed to seek!!\n");
					break;
				}
			} else {
				fd->f_pos = offset;
			}
		}

		p = (__force char __user *)buf;
		pos = (loff_t)offset;

		retLen = __vfs_write(fd, p, len, &pos);
		if (retLen < 0)
			DBGLOG(INIT, ERROR, "[MT6620][nvram_write] : write failed!! Error code: %d\n", retLen);

	} while (FALSE);

	filp_close(fd, NULL);

	set_fs(old_fs);

	return retLen;

#else /* !CFG_SUPPORT_NVRAMS */

	return -EIO;

#endif
}
#else
static int nvram_write(char *filename, char *buf, ssize_t len, int offset)
{
#if CFG_SUPPORT_NVRAM
	struct file *fd;
	int retLen = -1;

	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);

	fd = filp_open(filename, O_WRONLY | O_CREAT, 0644);

	if (IS_ERR(fd)) {
		DBGLOG(INIT, INFO, "[MT6620][nvram_write] : failed to open!!\n");
		set_fs(old_fs);
		return -1;
	}

	do {
		if ((fd->f_op == NULL) || (fd->f_op->write == NULL)) {
			DBGLOG(INIT, INFO, "[MT6620][nvram_write] : file can not be write!!\n");
			break;
		}
		/* End of if */
		if (fd->f_pos != offset) {
			if (fd->f_op->llseek) {
				if (fd->f_op->llseek(fd, offset, 0) != offset) {
					DBGLOG(INIT, INFO, "[MT6620][nvram_write] : failed to seek!!\n");
					break;
				}
			} else {
				fd->f_pos = offset;
			}
		}

		retLen = fd->f_op->write(fd, buf, len, &fd->f_pos);

	} while (FALSE);

	filp_close(fd, NULL);

	set_fs(old_fs);

	return retLen;

#else /* !CFG_SUPPORT_NVRAMS */

	return -EIO;

#endif
}
#endif
#endif /* #if 0 */
/*----------------------------------------------------------------------------*/
/*!
* \brief API for reading data on NVRAM
*
* \param[in]
*           prGlueInfo
*           u4Offset
* \param[out]
*           pu2Data
* \return
*           TRUE
*           FALSE
*/
/*----------------------------------------------------------------------------*/
BOOLEAN kalCfgDataRead(IN P_GLUE_INFO_T prGlueInfo, IN UINT_32 u4Offset,
					IN UINT_32 u4Len, OUT PUINT_16 pu2Data)
{
	if (pu2Data == NULL)
		return FALSE;

	if (u4Offset + u4Len > CFG_FILE_WIFI_REC_SIZE)
		return FALSE;

	kalMemCopy(pu2Data, &g_aucNvram[u4Offset], u4Len);
	return TRUE;
#if 0
	if (nvram_read(WIFI_NVRAM_FILE_NAME,
		(char *)pu2Data, u4Len, u4Offset) != u4Len) {
		return FALSE;
	} else {
		return TRUE;
	}
#endif
}

/*----------------------------------------------------------------------------*/
/*!
* \brief API for reading data on NVRAM
*
* \param[in]
*           prGlueInfo
*           u4Offset
* \param[out]
*           pu2Data
* \return
*           TRUE
*           FALSE
*/
/*----------------------------------------------------------------------------*/
BOOLEAN kalCfgDataRead16(IN P_GLUE_INFO_T prGlueInfo, IN UINT_32 u4Offset, OUT PUINT_16 pu2Data)
{
	if (pu2Data == NULL)
		return FALSE;

	if (u4Offset + sizeof(unsigned short) > CFG_FILE_WIFI_REC_SIZE)
		return FALSE;

	kalMemCopy(pu2Data, &g_aucNvram[u4Offset], sizeof(unsigned short));
	return TRUE;
#if 0
	if (nvram_read(WIFI_NVRAM_FILE_NAME,
		(char *)pu2Data, sizeof(unsigned short), u4Offset) != sizeof(unsigned short)) {
		return FALSE;
	} else {
		return TRUE;
	}
#endif
}

/*----------------------------------------------------------------------------*/
/*!
* \brief API for writing data on NVRAM
*
* \param[in]
*           prGlueInfo
*           u4Offset
*           u2Data
* \return
*           TRUE
*           FALSE
*/
/*----------------------------------------------------------------------------*/
BOOLEAN kalCfgDataWrite16(IN P_GLUE_INFO_T prGlueInfo, UINT_32 u4Offset, UINT_16 u2Data)
{
	if (u4Offset + sizeof(unsigned short) > CFG_FILE_WIFI_REC_SIZE)
		return FALSE;

	kalMemCopy(&g_aucNvram[u4Offset], &u2Data, sizeof(unsigned short));
	return TRUE;
#if 0
	if (nvram_write(WIFI_NVRAM_FILE_NAME,
			(char *)&u2Data, sizeof(unsigned short), u4Offset) != sizeof(unsigned short)) {
		return FALSE;
	} else {
		return TRUE;
	}
#endif
}
