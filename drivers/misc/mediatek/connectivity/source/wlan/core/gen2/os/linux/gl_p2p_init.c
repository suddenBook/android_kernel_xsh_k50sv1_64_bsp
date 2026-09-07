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

#include "precomp.h"

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

#define P2P_INF_NAME "p2p%d"
#define AP_INF_NAME  "ap%d"

#define RUNNING_P2P_MODE  0
#define RUNNING_AP_MODE   1

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
static PUCHAR ifname = P2P_INF_NAME;
static UINT_16 mode = RUNNING_P2P_MODE;

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

void p2pHandleSystemSuspend(void)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	struct net_device *prDev;
	UINT_8 ip[4] = { 0 };
	BOOLEAN fgAddress;
	WLAN_STATUS rStatus;

	if (!wlanExportGlueInfo(&prGlueInfo) || !prGlueInfo || !prGlueInfo->prAdapter ||
	    !prGlueInfo->prP2PInfo || !prGlueInfo->prP2PInfo->prDevHandler)
		return;
	prDev = prGlueInfo->prP2PInfo->prDevHandler;
	fgAddress = kalGetIPv4Address(prDev, ip);
	rStatus = kalSetIPv4Address(prGlueInfo, fgAddress ? ip : NULL, TRUE);
	DBGLOG(P2P, INFO, "suspend IPv4: %d.%d.%d.%d, rStatus: %u\n",
		ip[0], ip[1], ip[2], ip[3], rStatus);
}

void p2pHandleSystemResume(void)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;

	if (!wlanExportGlueInfo(&prGlueInfo) || !prGlueInfo || !prGlueInfo->prAdapter ||
	    !prGlueInfo->prP2PInfo || !prGlueInfo->prP2PInfo->prDevHandler)
		return;
	rStatus = kalSetIPv4Address(prGlueInfo, NULL, TRUE);
	DBGLOG(P2P, INFO, "resume clear IPv4 filter: %u\n", rStatus);
}

/*----------------------------------------------------------------------------*/
/*!
* \brief
*       run p2p init procedure, glue register p2p and set p2p registered flag
*
* \retval 1     Success
*/
/*----------------------------------------------------------------------------*/
BOOLEAN p2pLaunch(P_GLUE_INFO_T prGlueInfo)
{

	DBGLOG(P2P, TRACE, "p2pLaunch\n");

	if (prGlueInfo->prAdapter->fgIsP2PRegistered == TRUE) {
		DBGLOG(P2P, INFO, "p2p is already registered\n");
		return FALSE;
	}

	if (!glRegisterP2P(prGlueInfo, ifname, (BOOLEAN) mode)) {
		DBGLOG(P2P, ERROR, "Launch failed\n");
		return FALSE;
	}

	prGlueInfo->prAdapter->fgIsP2PRegistered = TRUE;
	DBGLOG(P2P, TRACE, "Launch success, fgIsP2PRegistered TRUE\n");
	return TRUE;
}

VOID p2pSetMode(IN BOOLEAN fgIsAPMode)
{
	if (fgIsAPMode) {
		mode = RUNNING_AP_MODE;
		ifname = AP_INF_NAME;
	} else {
		mode = RUNNING_P2P_MODE;
		ifname = P2P_INF_NAME;
	}

}				/* p2pSetMode */

/*----------------------------------------------------------------------------*/
/*!
* \brief
*       run p2p exit procedure, glue unregister p2p and set p2p registered flag
*
* \retval 1     Success
*/
/*----------------------------------------------------------------------------*/
BOOLEAN p2pRemove(P_GLUE_INFO_T prGlueInfo)
{
	if (prGlueInfo->prAdapter->fgIsP2PRegistered == FALSE) {
		DBGLOG(P2P, INFO, "p2p is not registered\n");
		return FALSE;
	}
	/* Check P2P FSM is stop or not. If not then stop now */
	if (IS_P2P_ACTIVE(prGlueInfo->prAdapter))
		p2pStopImmediate(prGlueInfo);

	prGlueInfo->prAdapter->fgIsP2PRegistered = FALSE;
	glUnregisterP2P(prGlueInfo);
	return TRUE;
}
