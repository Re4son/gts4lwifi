/*
 * Copyright (c) 2011-2017 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/*
 * DOC: csr_api_scan.c
 *
 * Implementation for the Common Scan interfaces.
 */

#include "ani_global.h"

#include "cds_mq.h"
#include "csr_inside_api.h"
#include "sme_inside.h"

#include "csr_support.h"

#include "host_diag_core_log.h"
#include "host_diag_core_event.h"

#include "cds_reg_service.h"
#include "wma_types.h"
#include "cds_utils.h"
#include "cfg_api.h"
#include "wma.h"

#include "cds_concurrency.h"
#include "wlan_hdd_main.h"
#include "pld_common.h"
#include "csr_internal.h"

#define MIN_CHN_TIME_TO_FIND_GO 100
#define MAX_CHN_TIME_TO_FIND_GO 100
#define DIRECT_SSID_LEN 7

/* Purpose of HIDDEN_TIMER
 * When we remove hidden ssid from the profile i.e., forget the SSID via GUI
 * that SSID shouldn't see in the profile For above requirement we used timer
 * limit, logic is explained below Timer value is initialsed to current time
 * when it receives corresponding probe response of hidden SSID (The probe
 * request is received regularly till SSID in the profile. Once it is removed
 * from profile probe request is not sent.) when we receive probe response for
 * broadcast probe request, during update SSID with saved SSID we will diff
 * current time with saved SSID time if it is greater than 1 min then we are
 * not updating with old one
 */

#define HIDDEN_TIMER (1*60*1000)
/* must be less than 100, represent the persentage of new RSSI */
#define CSR_SCAN_RESULT_RSSI_WEIGHT     80
#define CSR_PURGE_RSSI_THRESHOLD     -70

#define MAX_ACTIVE_SCAN_FOR_ONE_CHANNEL 140
#define MIN_ACTIVE_SCAN_FOR_ONE_CHANNEL 120

#define MAX_ACTIVE_SCAN_FOR_ONE_CHANNEL_FASTREASSOC 30
#define MIN_ACTIVE_SCAN_FOR_ONE_CHANNEL_FASTREASSOC 20

#define PCL_ADVANTAGE 20
#define PCL_RSSI_THRESHOLD -75

#define CSR_SCAN_IS_OVER_BSS_LIMIT(pMac)  \
	((pMac)->scan.nBssLimit <= (csr_ll_count(&(pMac)->scan.scanResultList)))

#define MIN_11D_AP_COUNT 3

static void csr_set_default_scan_timing(tpAniSirGlobal pMac,
					tSirScanType scanType,
					tCsrScanRequest *pScanRequest);
static QDF_STATUS csr_scan_channels(tpAniSirGlobal pMac, tSmeCmd *pCommand);
static void csr_set_cfg_valid_channel_list(tpAniSirGlobal pMac, uint8_t
					*pChannelList, uint8_t NumChannels);
static void csr_save_tx_power_to_cfg(tpAniSirGlobal pMac, tDblLinkList *pList,
			      uint32_t cfgId);
static void csr_set_cfg_country_code(tpAniSirGlobal pMac, uint8_t *countryCode);
static void csr_purge_channel_power(tpAniSirGlobal pMac, tDblLinkList
							*pChannelList);
static bool csr_scan_validate_scan_result(tpAniSirGlobal pMac,
					uint8_t *pChannels,
					  uint8_t numChn,
					  tSirBssDescription *pBssDesc,
					  tDot11fBeaconIEs **ppIes);
static bool csr_roam_is_valid_channel(tpAniSirGlobal pMac, uint8_t channel);
static void csr_purge_scan_result_by_age(void *pv);

#define CSR_IS_SOCIAL_CHANNEL(channel) \
	(((channel) == 1) || ((channel) == 6) || ((channel) == 11))

static void csr_release_scan_cmd_pending_list(tpAniSirGlobal pMac)
{
	tListElem *pEntry;
	tSmeCmd *pCommand;

	while ((pEntry =
			csr_ll_remove_head(&pMac->scan.scanCmdPendingList,
					   LL_ACCESS_LOCK)) != NULL) {
		pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
		if (eSmeCsrCommandMask & pCommand->command)
			csr_abort_command(pMac, pCommand, true);
		else
			sme_err("Received command: %d", pCommand->command);
	}
}

/* pResult is invalid calling this function. */
void csr_free_scan_result_entry(tpAniSirGlobal pMac, struct tag_csrscan_result
				*pResult)
{
	if (NULL != pResult->Result.pvIes)
		qdf_mem_free(pResult->Result.pvIes);

	qdf_mem_free(pResult);
}

static QDF_STATUS csr_ll_scan_purge_result(tpAniSirGlobal pMac,
					   tDblLinkList *pList)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tListElem *pEntry;
	struct tag_csrscan_result *pBssDesc;

	csr_ll_lock(pList);

	while ((pEntry = csr_ll_remove_head(pList, LL_ACCESS_NOLOCK)) != NULL) {
		pBssDesc = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		csr_free_scan_result_entry(pMac, pBssDesc);
	}

	csr_ll_unlock(pList);

	return status;
}

QDF_STATUS csr_scan_open(tpAniSirGlobal mac_ctx)
{
	csr_ll_open(mac_ctx->hHdd, &mac_ctx->scan.scanResultList);
	csr_ll_open(mac_ctx->hHdd, &mac_ctx->scan.tempScanResults);
	csr_ll_open(mac_ctx->hHdd, &mac_ctx->scan.channelPowerInfoList24);
	csr_ll_open(mac_ctx->hHdd, &mac_ctx->scan.channelPowerInfoList5G);
#ifdef WLAN_AP_STA_CONCURRENCY
	csr_ll_open(mac_ctx->hHdd, &mac_ctx->scan.scanCmdPendingList);
#endif
	mac_ctx->scan.fFullScanIssued = false;
	mac_ctx->scan.nBssLimit = CSR_MAX_BSS_SUPPORT;
	return QDF_STATUS_SUCCESS;
}

QDF_STATUS csr_scan_close(tpAniSirGlobal pMac)
{
	csr_ll_scan_purge_result(pMac, &pMac->scan.tempScanResults);
	csr_ll_scan_purge_result(pMac, &pMac->scan.scanResultList);
#ifdef WLAN_AP_STA_CONCURRENCY
	csr_release_scan_cmd_pending_list(pMac);
#endif
	csr_ll_close(&pMac->scan.scanResultList);
	csr_ll_close(&pMac->scan.tempScanResults);
#ifdef WLAN_AP_STA_CONCURRENCY
	csr_ll_close(&pMac->scan.scanCmdPendingList);
#endif
	csr_purge_channel_power(pMac, &pMac->scan.channelPowerInfoList24);
	csr_purge_channel_power(pMac, &pMac->scan.channelPowerInfoList5G);
	csr_ll_close(&pMac->scan.channelPowerInfoList24);
	csr_ll_close(&pMac->scan.channelPowerInfoList5G);
	csr_scan_disable(pMac);
	return QDF_STATUS_SUCCESS;
}

QDF_STATUS csr_scan_enable(tpAniSirGlobal pMac)
{

	pMac->scan.fScanEnable = true;

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS csr_scan_disable(tpAniSirGlobal pMac)
{
	pMac->scan.fScanEnable = false;

	return QDF_STATUS_SUCCESS;
}

/* Set scan timing parameters according to state of other driver sessions */
/* No validation of the parameters is performed. */
static void csr_set_default_scan_timing(tpAniSirGlobal pMac,
					tSirScanType scanType,
					tCsrScanRequest *pScanRequest)
{
#ifdef WLAN_AP_STA_CONCURRENCY
	if (csr_is_any_session_connected(pMac)) {
		/* Reset passive scan time as per ini parameter. */
		cfg_set_int(pMac, WNI_CFG_PASSIVE_MAXIMUM_CHANNEL_TIME,
			    pMac->roam.configParam.nPassiveMaxChnTimeConc);
		/* If multi-session, use the appropriate default scan times */
		if (scanType == eSIR_ACTIVE_SCAN) {
			pScanRequest->maxChnTime =
				pMac->roam.configParam.nActiveMaxChnTimeConc;
			pScanRequest->minChnTime =
				pMac->roam.configParam.nActiveMinChnTimeConc;
		} else {
			pScanRequest->maxChnTime =
				pMac->roam.configParam.nPassiveMaxChnTimeConc;
			pScanRequest->minChnTime =
				pMac->roam.configParam.nPassiveMinChnTimeConc;
		}
		pScanRequest->restTime = pMac->roam.configParam.nRestTimeConc;

		pScanRequest->min_rest_time =
			pMac->roam.configParam.min_rest_time_conc;
		/* IF AP is active set min rest time same as max rest time */
		if (csr_is_infra_ap_started(pMac))
			pScanRequest->min_rest_time = pScanRequest->restTime;

		pScanRequest->idle_time =
			pMac->roam.configParam.idle_time_conc;

		/* Return so that fields set above will not be overwritten. */
		return;
	}
#endif

	/* This portion of the code executed if multi-session not supported */
	/* (WLAN_AP_STA_CONCURRENCY not defined) or no multi-session. */
	/* Use the "regular" (non-concurrency) default scan timing. */
	cfg_set_int(pMac, WNI_CFG_PASSIVE_MAXIMUM_CHANNEL_TIME,
		    pMac->roam.configParam.nPassiveMaxChnTime);
	if (pScanRequest->scanType == eSIR_ACTIVE_SCAN) {
		pScanRequest->maxChnTime =
			pMac->roam.configParam.nActiveMaxChnTime;
		pScanRequest->minChnTime =
			pMac->roam.configParam.nActiveMinChnTime;
	} else {
		pScanRequest->maxChnTime =
			pMac->roam.configParam.nPassiveMaxChnTime;
		pScanRequest->minChnTime =
			pMac->roam.configParam.nPassiveMinChnTime;
	}
#ifdef WLAN_AP_STA_CONCURRENCY

	/* No rest time/Idle time if no sessions are connected. */
	pScanRequest->restTime = 0;
	pScanRequest->min_rest_time = 0;
	pScanRequest->idle_time = 0;

#endif
}

/**
 * csr_scan_2g_only_request() - This function will update the scan request with
 * only 2.4GHz valid channel list.
 * @mac_ctx:      Pointer to Global MAC structure
 * @scan_cmd      scan cmd
 * @scan_req      scan req
 *
 * This function will update the scan request with  only 2.4GHz valid channel
 * list.
 *
 * @Return: status of operation
 */
static QDF_STATUS
csr_scan_2g_only_request(tpAniSirGlobal mac_ctx,
			 tSmeCmd *scan_cmd,
			 tCsrScanRequest *scan_req)
{
	uint8_t idx, lst_sz = 0;

	QDF_ASSERT(scan_cmd && scan_req);
	/* To silence the KW tool null check is added */
	if ((scan_cmd == NULL) || (scan_req == NULL)) {
		sme_err("Scan Cmd or Scan Request is NULL");
		return QDF_STATUS_E_INVAL;
	}

	if (eCSR_SCAN_REQUEST_FULL_SCAN != scan_req->requestType)
		return QDF_STATUS_SUCCESS;

	sme_debug("Scanning only 2G Channels during first scan");

	/* Contsruct valid Supported 2.4 GHz Channel List */
	if (NULL == scan_req->ChannelInfo.ChannelList) {
		scan_req->ChannelInfo.ChannelList =
			qdf_mem_malloc(NUM_24GHZ_CHANNELS);
		if (NULL == scan_req->ChannelInfo.ChannelList) {
			sme_err("Memory allocation failed");
			return QDF_STATUS_E_NOMEM;
		}
		for (idx = 1; idx <= NUM_24GHZ_CHANNELS; idx++) {
			if (csr_is_supported_channel(mac_ctx, idx)) {
				scan_req->ChannelInfo.ChannelList[lst_sz] = idx;
				lst_sz++;
			}
		}
	} else {
		for (idx = 0;
		     idx < scan_req->ChannelInfo.numOfChannels;
		     idx++) {
			if (scan_req->ChannelInfo.ChannelList[idx] <=
				CDS_24_GHZ_CHANNEL_14
			    && csr_is_supported_channel(mac_ctx,
				scan_req->ChannelInfo.ChannelList[idx])) {
				scan_req->ChannelInfo.ChannelList[lst_sz] =
					scan_req->ChannelInfo.ChannelList[idx];
				lst_sz++;
			}
		}
	}
	scan_req->ChannelInfo.numOfChannels = lst_sz;
	return QDF_STATUS_SUCCESS;
}

static void
csr_set_scan_reason(tSmeCmd *scan_cmd, eCsrRequestType req_type)
{
	switch (req_type) {
	case eCSR_SCAN_REQUEST_11D_SCAN:
		scan_cmd->u.scanCmd.reason = eCsrScan11d1;
		break;
#ifdef SOFTAP_CHANNEL_RANGE
	case eCSR_SCAN_SOFTAP_CHANNEL_RANGE:
#endif
	case eCSR_SCAN_REQUEST_FULL_SCAN:
	case eCSR_SCAN_P2P_DISCOVERY:
	case eCSR_SCAN_RRM:
		scan_cmd->u.scanCmd.reason = eCsrScanUserRequest;
		break;
	case eCSR_SCAN_HO_PROBE_SCAN:
		scan_cmd->u.scanCmd.reason = eCsrScanProbeBss;
		break;
	case eCSR_SCAN_P2P_FIND_PEER:
		scan_cmd->u.scanCmd.reason = eCsrScanP2PFindPeer;
		break;
	default:
		break;
	}
}

static QDF_STATUS
csr_issue_11d_scan(tpAniSirGlobal mac_ctx, tSmeCmd *scan_cmd,
		   tCsrScanRequest *scan_req, uint16_t session_id)
{
	QDF_STATUS status;
	tSmeCmd *scan_11d_cmd = NULL;
	tCsrScanRequest tmp_rq;
	tCsrChannelInfo *chn_info = &tmp_rq.ChannelInfo;
	uint32_t num_chn = mac_ctx->scan.base_channels.numChannels;
	tCsrRoamSession *csr_session = CSR_GET_SESSION(mac_ctx, session_id);

	if (csr_session == NULL) {
		sme_err("session %d not found", session_id);
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAILURE;
	}

	if (num_chn > WNI_CFG_VALID_CHANNEL_LIST_LEN) {
		sme_err("invalid number of channels: %d", num_chn);
		return QDF_STATUS_E_FAILURE;
	}

	if (!(((false == mac_ctx->first_scan_done)
	     && (eCSR_SCAN_REQUEST_11D_SCAN != scan_req->requestType))
#ifdef SOFTAP_CHANNEL_RANGE
	    && (eCSR_SCAN_SOFTAP_CHANNEL_RANGE != scan_req->requestType)
#endif
	    && (false == mac_ctx->scan.fEnableBypass11d)))
		return QDF_STATUS_SUCCESS;

	qdf_mem_set(&tmp_rq, sizeof(tCsrScanRequest), 0);
	scan_11d_cmd = csr_get_command_buffer(mac_ctx);
	if (!scan_11d_cmd) {
		sme_err("scan_11d_cmd failed");
		return QDF_STATUS_E_FAILURE;
	}

	qdf_mem_set(&scan_11d_cmd->u.scanCmd, sizeof(tScanCmd), 0);
	chn_info->ChannelList = qdf_mem_malloc(num_chn);
	if (NULL == chn_info->ChannelList) {
		sme_err("Failed to allocate memory");
		return QDF_STATUS_E_NOMEM;
	}
	qdf_mem_copy(chn_info->ChannelList,
		     mac_ctx->scan.base_channels.channelList, num_chn);

	chn_info->numOfChannels = (uint8_t) num_chn;
	scan_11d_cmd->command = eSmeCommandScan;
	scan_11d_cmd->u.scanCmd.callback = mac_ctx->scan.callback11dScanDone;
	scan_11d_cmd->u.scanCmd.pContext = NULL;
	wma_get_scan_id(&scan_11d_cmd->u.scanCmd.scanID);
	tmp_rq.BSSType = eCSR_BSS_TYPE_ANY;
	tmp_rq.scan_id = scan_11d_cmd->u.scanCmd.scanID;

	status = qdf_mc_timer_init(&scan_cmd->u.scanCmd.csr_scan_timer,
			QDF_TIMER_TYPE_SW,
			csr_scan_active_list_timeout_handle, scan_11d_cmd);

	if (csr_is11d_supported(mac_ctx)) {
		tmp_rq.bcnRptReqScan = scan_req->bcnRptReqScan;
		if (scan_req->bcnRptReqScan)
			tmp_rq.scanType = scan_req->scanType ?
				eSIR_PASSIVE_SCAN : scan_req->scanType;
		else
			tmp_rq.scanType = eSIR_PASSIVE_SCAN;
		tmp_rq.requestType = eCSR_SCAN_REQUEST_11D_SCAN;
		scan_11d_cmd->u.scanCmd.reason = eCsrScan11d1;
		tmp_rq.maxChnTime =
			mac_ctx->roam.configParam.nPassiveMaxChnTime;
		tmp_rq.minChnTime =
			mac_ctx->roam.configParam.nPassiveMinChnTime;
	} else {
		tmp_rq.bcnRptReqScan = scan_req->bcnRptReqScan;
		if (scan_req->bcnRptReqScan)
			tmp_rq.scanType = scan_req->scanType;
		else
			tmp_rq.scanType = eSIR_ACTIVE_SCAN;
		tmp_rq.requestType = scan_req->requestType;
		scan_11d_cmd->u.scanCmd.reason = scan_cmd->u.scanCmd.reason;
		tmp_rq.maxChnTime = mac_ctx->roam.configParam.nActiveMaxChnTime;
		tmp_rq.minChnTime = mac_ctx->roam.configParam.nActiveMinChnTime;
	}
	if (mac_ctx->roam.configParam.nInitialDwellTime) {
		tmp_rq.maxChnTime = mac_ctx->roam.configParam.nInitialDwellTime;
		sme_debug("11d scan, updating dwell time for first scan %u",
			tmp_rq.maxChnTime);
	}

	status = csr_scan_copy_request(mac_ctx,
			&scan_11d_cmd->u.scanCmd.u.scanRequest, &tmp_rq);
	/* Free the channel list */
	qdf_mem_free(chn_info->ChannelList);
	chn_info->ChannelList = NULL;
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_err("csr_scan_copy_request failed");
		return QDF_STATUS_E_FAILURE;
	}

	mac_ctx->scan.scanProfile.numOfChannels =
		scan_11d_cmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels;


	status = csr_queue_sme_command(mac_ctx, scan_11d_cmd, false);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_err("Failed to send message status = %d",
			status);
		return QDF_STATUS_E_FAILURE;
	}
	return QDF_STATUS_SUCCESS;
}

QDF_STATUS csr_scan_request(tpAniSirGlobal pMac, uint16_t sessionId,
			    tCsrScanRequest *scan_req,
			    csr_scan_completeCallback callback, void *pContext)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	tSmeCmd *scan_cmd = NULL;
	tCsrScanRequest *pTempScanReq = NULL;
	tCsrConfig *cfg_prm = &pMac->roam.configParam;

	if (scan_req == NULL) {
		sme_err("scan_req is NULL");
		QDF_ASSERT(0);
		return status;
	}

	/*
	 * During group formation, the P2P client scans for GO with the specific
	 * SSID. There will be chances of GO switching to other channels because
	 * of scan or to STA channel in case of STA+GO MCC scenario. So to
	 * increase the possibility of client to find the GO, the dwell time of
	 * scan is increased to 100ms.
	 * If the scan request is for specific SSId the length of SSID will be
	 * greater than 7 as SSID for p2p search contains "DIRECT-")
	 */
	if (scan_req->p2pSearch
	    && scan_req->SSIDs.numOfSSIDs
	    && (NULL != scan_req->SSIDs.SSIDList)
	    && (scan_req->SSIDs.SSIDList->SSID.length > DIRECT_SSID_LEN)) {
		sme_debug("P2P: Increasing the min and max Dwell time to %d for specific SSID scan %.*s",
			MAX_CHN_TIME_TO_FIND_GO,
			scan_req->SSIDs.SSIDList->SSID.length,
			scan_req->SSIDs.SSIDList->SSID.ssId);
		scan_req->maxChnTime = MAX_CHN_TIME_TO_FIND_GO;
		scan_req->minChnTime = MIN_CHN_TIME_TO_FIND_GO;
	}

	if (!pMac->scan.fScanEnable) {
		sme_err("SId: %d Scanning not enabled Scan type=%u, numOfSSIDs=%d P2P search=%d",
			sessionId, scan_req->requestType,
			scan_req->SSIDs.numOfSSIDs,
			scan_req->p2pSearch);
		goto release_cmd;
	}

	scan_cmd = csr_get_command_buffer(pMac);
	if (!scan_cmd) {
		sme_err("scan_cmd is NULL");
		goto release_cmd;
	}

	qdf_mem_set(&scan_cmd->u.scanCmd, sizeof(tScanCmd), 0);
	scan_cmd->command = eSmeCommandScan;
	scan_cmd->sessionId = sessionId;
	if (scan_cmd->sessionId >= CSR_ROAM_SESSION_MAX)
		sme_err("Invalid Sme SessionID: %d", sessionId);
	scan_cmd->u.scanCmd.callback = callback;
	scan_cmd->u.scanCmd.pContext = pContext;
	csr_set_scan_reason(scan_cmd, scan_req->requestType);
	if (scan_req->minChnTime == 0 && scan_req->maxChnTime == 0) {
		/* The caller doesn't set the time correctly. Set it here */
		csr_set_default_scan_timing(pMac, scan_req->scanType, scan_req);
		sme_debug("Setting default min %d and max %d ChnTime",
			scan_req->minChnTime, scan_req->maxChnTime);
	}
#ifdef WLAN_AP_STA_CONCURRENCY
	/*
	 * Need to set restTime/min_Ret_time/idle_time
	 * only if at least one session is connected
	 */
	if (scan_req->restTime == 0 && csr_is_any_session_connected(pMac)) {
		scan_req->restTime = cfg_prm->nRestTimeConc;
		scan_req->min_rest_time = cfg_prm->min_rest_time_conc;
		/* IF AP is active set min rest time same as max rest time */
		if (csr_is_infra_ap_started(pMac))
			scan_req->min_rest_time = scan_req->restTime;
		scan_req->idle_time = cfg_prm->idle_time_conc;
		if (scan_req->scanType == eSIR_ACTIVE_SCAN) {
			scan_req->maxChnTime = cfg_prm->nActiveMaxChnTimeConc;
			scan_req->minChnTime = cfg_prm->nActiveMinChnTimeConc;
		} else {
			scan_req->maxChnTime = cfg_prm->nPassiveMaxChnTimeConc;
			scan_req->minChnTime = cfg_prm->nPassiveMinChnTimeConc;
		}
	}
#endif
	/* Increase dwell time in case P2P Search and Miracast is not present */
	if (scan_req->p2pSearch && scan_req->ChannelInfo.numOfChannels
	    == P2P_SOCIAL_CHANNELS && (!(pMac->sme.miracast_value))) {
		scan_req->maxChnTime += P2P_SEARCH_DWELL_TIME_INCREASE;
	}
	scan_cmd->u.scanCmd.scanID = scan_req->scan_id;
	/*
	 * If it is the first scan request from HDD, CSR checks if it is for 11d
	 * If it is not, CSR will save the scan request in the pending cmd queue
	 * & issue an 11d scan request to PE.
	 */
	status = csr_issue_11d_scan(pMac, scan_cmd, scan_req, sessionId);
	if (status != QDF_STATUS_SUCCESS)
		goto release_cmd;

	/*
	 * Scan only 2G Channels if set in ini file. This is mainly to reduce
	 * the First Scan duration once we turn on Wifi
	 */
	if (pMac->scan.fFirstScanOnly2GChnl
	    && false == pMac->first_scan_done) {
		csr_scan_2g_only_request(pMac, scan_cmd, scan_req);
		pMac->first_scan_done = true;
	}


	if (cfg_prm->nInitialDwellTime) {
		scan_req->maxChnTime = cfg_prm->nInitialDwellTime;
		cfg_prm->nInitialDwellTime = 0;
		sme_debug("updating dwell time for first scan %u",
			scan_req->maxChnTime);
	}
	/*
	 * RRM is an internally triggered scan and to
	 * achieve reliable results, Adaptive dwell scan is
	 * disabled using the dwell_mode.
	 */
	if (scan_req->requestType != eCSR_SCAN_RRM)
		scan_req->scan_adaptive_dwell_mode =
			cfg_prm->scan_adaptive_dwell_mode;

	status = csr_scan_copy_request(pMac, &scan_cmd->u.scanCmd.u.scanRequest,
				       scan_req);
	/*
	 * Reset the variable after the first scan is  queued after loading the
	 * driver. The purpose of this parameter is that DFS channels are
	 * skipped during the first scan after loading the driver. The above API
	 * builds the target scan request in which this variable is used.
	 */
	cfg_prm->initial_scan_no_dfs_chnl = 0;
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_err("fail to copy request status: %d", status);
		goto release_cmd;
	}

	pTempScanReq = &scan_cmd->u.scanCmd.u.scanRequest;
	pMac->scan.scanProfile.numOfChannels =
		pTempScanReq->ChannelInfo.numOfChannels;
	status = qdf_mc_timer_init(&scan_cmd->u.scanCmd.csr_scan_timer,
				QDF_TIMER_TYPE_SW,
				csr_scan_active_list_timeout_handle, scan_cmd);
	sme_debug(
		"SId=%d scanId=%d Scan reason=%u numSSIDs=%d numChan=%d P2P search=%d minCT=%d maxCT=%d uIEFieldLen=%d BSSID: " MAC_ADDRESS_STR,
		sessionId, scan_cmd->u.scanCmd.scanID,
		scan_cmd->u.scanCmd.reason, pTempScanReq->SSIDs.numOfSSIDs,
		pTempScanReq->ChannelInfo.numOfChannels,
		pTempScanReq->p2pSearch, pTempScanReq->minChnTime,
		pTempScanReq->maxChnTime, pTempScanReq->uIEFieldLen,
		MAC_ADDR_ARRAY(scan_cmd->u.scanCmd.u.scanRequest.bssid.bytes));

	status = csr_queue_sme_command(pMac, scan_cmd, false);
	if (!QDF_IS_STATUS_SUCCESS(status))
		sme_err("fail to send message status: %d", status);

release_cmd:
	if (!QDF_IS_STATUS_SUCCESS(status) && scan_cmd) {
		sme_err(" SId: %d Failed with status=%d Scan reason=%u numOfSSIDs=%d P2P search=%d scanId=%d",
			sessionId, status, scan_cmd->u.scanCmd.reason,
			scan_req->SSIDs.numOfSSIDs, scan_req->p2pSearch,
			scan_cmd->u.scanCmd.scanID);
		csr_release_command_scan(pMac, scan_cmd);
	}

	return status;
}

static QDF_STATUS csr_issue_roam_after_lostlink_scan(tpAniSirGlobal pMac,
						     uint32_t sessionId,
						     eCsrRoamReason reason)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	tScanResultHandle hBSSList = NULL;
	tCsrScanResultFilter *pScanFilter = NULL;
	uint32_t roamId = 0;
	tCsrRoamProfile *pProfile = NULL;
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);

	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}

	if (pSession->fCancelRoaming) {
		sme_debug("lost link roaming canceled");
		status = QDF_STATUS_SUCCESS;
		goto free_filter;
	}
	/* Here is the profile we need to connect to */
	pScanFilter = qdf_mem_malloc(sizeof(tCsrScanResultFilter));
	if (NULL == pScanFilter) {
		status = QDF_STATUS_E_NOMEM;
		goto free_filter;
	}
	if (NULL == pSession->pCurRoamProfile) {
		pScanFilter->EncryptionType.numEntries = 1;
		pScanFilter->EncryptionType.encryptionType[0] =
			eCSR_ENCRYPT_TYPE_NONE;
	} else {
		/*
		 * We have to make a copy of pCurRoamProfile because it will
		 * be free inside csr_roam_issue_connect
		 */
		pProfile = qdf_mem_malloc(sizeof(tCsrRoamProfile));
		if (NULL == pProfile) {
			status = QDF_STATUS_E_NOMEM;
			goto free_filter;
		}
		status = csr_roam_copy_profile(pMac, pProfile,
					       pSession->pCurRoamProfile);
		if (!QDF_IS_STATUS_SUCCESS(status))
			goto free_filter;
		status = csr_roam_prepare_filter_from_profile(pMac, pProfile,
							      pScanFilter);
	} /* We have a profile */
	roamId = GET_NEXT_ROAM_ID(&pMac->roam);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto free_filter;

	status = csr_scan_get_result(pMac, pScanFilter, &hBSSList);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto free_filter;

	if (eCsrLostLink1 == reason) {
		/* if possible put the last connected BSS in beginning */
		csr_move_bss_to_head_from_bssid(pMac,
				&pSession->connectedProfile.bssid, hBSSList);
	}
	status = csr_roam_issue_connect(pMac, sessionId, pProfile, hBSSList,
					reason, roamId, true, true);
	if (!QDF_IS_STATUS_SUCCESS(status))
		csr_scan_result_purge(pMac, hBSSList);

free_filter:
	if (pScanFilter) {
		/* we need to free memory for filter if profile exists */
		csr_free_scan_filter(pMac, pScanFilter);
		qdf_mem_free(pScanFilter);
	}
	if (NULL != pProfile) {
		csr_release_profile(pMac, pProfile);
		qdf_mem_free(pProfile);
	}
	return status;
}

QDF_STATUS csr_scan_handle_failed_lostlink1(tpAniSirGlobal pMac,
					    uint32_t sessionId)
{
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);

	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}
	sme_debug("Lost link scan 1 failed");
	if (pSession->fCancelRoaming)
		return QDF_STATUS_E_FAILURE;
	if (!pSession->pCurRoamProfile)
		return csr_scan_request_lost_link3(pMac, sessionId);
	/*
	 * We fail lostlink1 but there may be other BSS in the cached result
	 * fit the profile. Give it a try first
	 */
	if (pSession->pCurRoamProfile->SSIDs.numOfSSIDs == 0 ||
	    pSession->pCurRoamProfile->SSIDs.numOfSSIDs > 1)
		/* try lostlink scan2 */
		return csr_scan_request_lost_link2(pMac, sessionId);
	if (!pSession->pCurRoamProfile->ChannelInfo.ChannelList
	    || pSession->pCurRoamProfile->ChannelInfo.ChannelList[0] == 0)
		/* go straight to lostlink scan3 */
		return csr_scan_request_lost_link3(pMac, sessionId);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS csr_scan_handle_failed_lostlink2(tpAniSirGlobal pMac,
					    uint32_t sessionId)
{
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);

	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}

	sme_debug("Lost link scan 2 failed");
	if (pSession->fCancelRoaming)
		return QDF_STATUS_E_FAILURE;

	if (!pSession->pCurRoamProfile
	    || !pSession->pCurRoamProfile->ChannelInfo.ChannelList
	    || pSession->pCurRoamProfile->ChannelInfo.ChannelList[0] == 0)
		/* try lostlink scan3 */
		return csr_scan_request_lost_link3(pMac, sessionId);

	return QDF_STATUS_E_FAILURE;
}

QDF_STATUS csr_scan_handle_failed_lostlink3(tpAniSirGlobal pMac,
					    uint32_t sessionId)
{
	sme_debug("Lost link scan 3 failed");
	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS
csr_update_lost_link1_cmd(tpAniSirGlobal mac_ctx, tSmeCmd *cmd,
			  tCsrRoamSession *pSession, uint32_t session_id)
{
	uint8_t i, num_ch = 0;
	tScanResultHandle bss_lst = NULL;
	tCsrScanResultInfo *scan_result = NULL;
	tCsrScanResultFilter *scan_filter = NULL;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tCsrSSIDs *ssid_list = &cmd->u.scanCmd.u.scanRequest.SSIDs;
	tCsrChannelInfo *ch_info = &cmd->u.scanCmd.u.scanRequest.ChannelInfo;

	cmd->command = eSmeCommandScan;
	cmd->sessionId = (uint8_t) session_id;
	cmd->u.scanCmd.reason = eCsrScanLostLink1;
	cmd->u.scanCmd.callback = NULL;
	cmd->u.scanCmd.pContext = NULL;
	cmd->u.scanCmd.u.scanRequest.maxChnTime =
		mac_ctx->roam.configParam.nActiveMaxChnTime;
	cmd->u.scanCmd.u.scanRequest.minChnTime =
		mac_ctx->roam.configParam.nActiveMinChnTime;
	cmd->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
	wma_get_scan_id(&cmd->u.scanCmd.scanID);
	status = qdf_mc_timer_init(&cmd->u.scanCmd.csr_scan_timer,
			QDF_TIMER_TYPE_SW,
			csr_scan_active_list_timeout_handle, cmd);
	cmd->u.scanCmd.u.scanRequest.scan_id =
		cmd->u.scanCmd.scanID;

	if (pSession->connectedProfile.SSID.length) {
		/*
		 * on error: following memory will be released by call to
		 * csr_release_command_scan in the end
		 */
		ssid_list->SSIDList = qdf_mem_malloc(sizeof(tCsrSSIDInfo));
		if (NULL == ssid_list->SSIDList)
			return QDF_STATUS_E_NOMEM;
		ssid_list->numOfSSIDs = 1;
		qdf_mem_copy(&ssid_list->SSIDList[0].SSID,
			     &pSession->connectedProfile.SSID,
			     sizeof(tSirMacSSid));
	} else
		ssid_list->numOfSSIDs = 0;

	if (!pSession->pCurRoamProfile)
		return QDF_STATUS_SUCCESS;

	scan_filter = qdf_mem_malloc(sizeof(tCsrScanResultFilter));
	if (NULL == scan_filter)
		return QDF_STATUS_E_NOMEM;

	status = csr_roam_prepare_filter_from_profile(mac_ctx,
			pSession->pCurRoamProfile, scan_filter);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto free_lost_link1_local_mem;

	if (!(QDF_IS_STATUS_SUCCESS(csr_scan_get_result(mac_ctx, scan_filter,
		&bss_lst)) && bss_lst)) {
		if (csr_roam_is_channel_valid(mac_ctx,
			pSession->connectedProfile.operationChannel)) {
			ch_info->ChannelList = qdf_mem_malloc(1);
			if (NULL == ch_info->ChannelList) {
				status = QDF_STATUS_E_NOMEM;
				goto free_lost_link1_local_mem;
			}
			ch_info->ChannelList[0] =
				pSession->connectedProfile.operationChannel;
			ch_info->numOfChannels = 1;
		}
		csr_free_scan_filter(mac_ctx, scan_filter);
		qdf_mem_free(scan_filter);
		return status;
	}

	/* on error: this mem will be released by csr_release_command_scan */
	ch_info->ChannelList = qdf_mem_malloc(WNI_CFG_VALID_CHANNEL_LIST_LEN);
	if (NULL == ch_info->ChannelList) {
		status = QDF_STATUS_E_NOMEM;
		goto free_lost_link1_local_mem;
	}

	scan_result = csr_scan_result_get_next(mac_ctx, bss_lst);
	while (scan_result != NULL && num_ch < WNI_CFG_VALID_CHANNEL_LIST_LEN) {
		for (i = 0; i < num_ch; i++) {
			if (ch_info->ChannelList[i] ==
				scan_result->BssDescriptor.channelId)
				break;
		}
		if (i == num_ch)
			ch_info->ChannelList[num_ch++] =
				scan_result->BssDescriptor.channelId;
		scan_result = csr_scan_result_get_next(mac_ctx, bss_lst);
	}
	/* Include the last connected BSS' channel */
	if (csr_roam_is_channel_valid(mac_ctx,
		pSession->connectedProfile.operationChannel)) {
		for (i = 0; i < num_ch; i++) {
			if (ch_info->ChannelList[i] ==
				pSession->connectedProfile.operationChannel)
				break;
		}
		if (i == num_ch)
			ch_info->ChannelList[num_ch++] =
				pSession->connectedProfile.operationChannel;
	}
	ch_info->numOfChannels = num_ch;
free_lost_link1_local_mem:
	if (scan_filter) {
		csr_free_scan_filter(mac_ctx, scan_filter);
		qdf_mem_free(scan_filter);
	}
	if (bss_lst)
		csr_scan_result_purge(mac_ctx, bss_lst);
	return status;
}

/**
 * csr_scan_request_lost_link1() - start scan on link lost 1
 * @mac_ctx:       mac global context
 * @session_id:    session id
 *
 * Lostlink1 scan is to actively scan the last connected profile's SSID on all
 * matched BSS channels. If no roam profile (it should not), it is like
 * lostlinkscan3
 *
 * Return: status of operation
 */
QDF_STATUS
csr_scan_request_lost_link1(tpAniSirGlobal mac_ctx, uint32_t session_id)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tSmeCmd *cmd = NULL;
	tCsrRoamSession *session = CSR_GET_SESSION(mac_ctx, session_id);

	if (!session) {
		sme_err("session %d not found", session_id);
		return QDF_STATUS_E_FAILURE;
	}

	cmd = csr_get_command_buffer(mac_ctx);
	if (!cmd) {
		status = QDF_STATUS_E_RESOURCES;
		goto release_lost_link1_cmd;
	}
	qdf_mem_set(&cmd->u.scanCmd, sizeof(tScanCmd), 0);
	status = csr_update_lost_link1_cmd(mac_ctx, cmd, session, session_id);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto release_lost_link1_cmd;

	qdf_mem_set(&cmd->u.scanCmd.u.scanRequest.bssid,
		    sizeof(struct qdf_mac_addr), 0xFF);
	status = csr_queue_sme_command(mac_ctx, cmd, false);
	if (!QDF_IS_STATUS_SUCCESS(status))
		sme_err("fail to send message status: %d", status);

release_lost_link1_cmd:
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_warn("failed with status %d", status);
		if (cmd)
			csr_release_command_scan(mac_ctx, cmd);
		status = csr_scan_handle_failed_lostlink1(mac_ctx, session_id);
	}
	return status;
}

static QDF_STATUS
csr_update_lost_link2_cmd(tpAniSirGlobal mac_ctx, tSmeCmd *cmd,
			  uint32_t session_id, tCsrRoamSession *session)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	uint8_t i, num_ch = 0;
	tScanResultHandle bss_lst = NULL;
	tCsrScanResultInfo *scan_result = NULL;
	tCsrScanResultFilter *scan_fltr = NULL;
	tCsrChannelInfo *ch_info = &cmd->u.scanCmd.u.scanRequest.ChannelInfo;

	cmd->command = eSmeCommandScan;
	cmd->sessionId = (uint8_t) session_id;
	cmd->u.scanCmd.reason = eCsrScanLostLink2;
	cmd->u.scanCmd.callback = NULL;
	cmd->u.scanCmd.pContext = NULL;
	cmd->u.scanCmd.u.scanRequest.maxChnTime =
		mac_ctx->roam.configParam.nActiveMaxChnTime;
	cmd->u.scanCmd.u.scanRequest.minChnTime =
		mac_ctx->roam.configParam.nActiveMinChnTime;
	cmd->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
	wma_get_scan_id(&cmd->u.scanCmd.scanID);
	cmd->u.scanCmd.u.scanRequest.scan_id =
		cmd->u.scanCmd.scanID;
	if (!session->pCurRoamProfile)
		return QDF_STATUS_SUCCESS;
	status = qdf_mc_timer_init(&cmd->u.scanCmd.csr_scan_timer,
			QDF_TIMER_TYPE_SW,
			csr_scan_active_list_timeout_handle, cmd);
	scan_fltr = qdf_mem_malloc(sizeof(tCsrScanResultFilter));
	if (NULL == scan_fltr)
		return QDF_STATUS_E_NOMEM;

	qdf_mem_set(scan_fltr, sizeof(tCsrScanResultFilter), 0);
	status = csr_roam_prepare_filter_from_profile(mac_ctx,
				session->pCurRoamProfile, scan_fltr);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto free_lost_link2_local_mem;

	status = csr_scan_get_result(mac_ctx, scan_fltr, &bss_lst);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto free_lost_link2_local_mem;

	if (!bss_lst)
		goto free_lost_link2_local_mem;

	ch_info->ChannelList = qdf_mem_malloc(WNI_CFG_VALID_CHANNEL_LIST_LEN);
	if (NULL == ch_info->ChannelList) {
		status = QDF_STATUS_E_NOMEM;
		goto free_lost_link2_local_mem;
	}
	scan_result = csr_scan_result_get_next(mac_ctx, bss_lst);
	while (scan_result != NULL && num_ch < WNI_CFG_VALID_CHANNEL_LIST_LEN) {
		for (i = 0; i < num_ch; i++) {
			if (ch_info->ChannelList[i] ==
				scan_result->BssDescriptor.channelId)
				break;
		}
		if (i == num_ch)
			ch_info->ChannelList[num_ch++] =
				scan_result->BssDescriptor.channelId;
		scan_result = csr_scan_result_get_next(mac_ctx, bss_lst);
	}
	ch_info->numOfChannels = num_ch;

free_lost_link2_local_mem:
	if (scan_fltr) {
		csr_free_scan_filter(mac_ctx, scan_fltr);
		qdf_mem_free(scan_fltr);
	}
	if (bss_lst)
		csr_scan_result_purge(mac_ctx, bss_lst);
	return status;
}

/**
 * csr_scan_request_lost_link2() - start scan on link lost 2
 * @mac_ctx:       mac global context
 * @session_id:    session id
 *
 * Lostlink2 scan is to actively scan the all SSIDs of the last roaming
 * profile's on all matched BSS channels. Since MAC doesn't support multiple
 * SSID, we scan all SSIDs and filter them afterwards
 *
 * Return: status of operation
 */
QDF_STATUS
csr_scan_request_lost_link2(tpAniSirGlobal mac_ctx, uint32_t session_id)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tSmeCmd *cmd = NULL;
	tCsrRoamSession *session = CSR_GET_SESSION(mac_ctx, session_id);

	if (!session) {
		sme_err("session %d not found", session_id);
		return QDF_STATUS_E_FAILURE;
	}

	cmd = csr_get_command_buffer(mac_ctx);
	if (!cmd) {
		status = QDF_STATUS_E_RESOURCES;
		goto release_lost_link2_cmd;
	}
	qdf_mem_set(&cmd->u.scanCmd, sizeof(tScanCmd), 0);
	status = csr_update_lost_link2_cmd(mac_ctx, cmd, session_id, session);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto release_lost_link2_cmd;

	qdf_mem_set(&cmd->u.scanCmd.u.scanRequest.bssid,
		    sizeof(struct qdf_mac_addr), 0xFF);
	/* Put to the head in pending queue */
	status = csr_queue_sme_command(mac_ctx, cmd, true);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_err("fail to send message status: %d", status);
		goto release_lost_link2_cmd;
	}

release_lost_link2_cmd:
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_warn("failed with status %d", status);
		if (cmd)
			csr_release_command_scan(mac_ctx, cmd);
		status = csr_scan_handle_failed_lostlink2(mac_ctx, session_id);
	}
	return status;
}

/**
 * csr_scan_request_lost_link3() - To actively scan all valid channels
 * @mac_ctx:       mac global context
 * @session_id:    session id
 *
 * Return: status of operation
 */
QDF_STATUS
csr_scan_request_lost_link3(tpAniSirGlobal mac_ctx, uint32_t session_id)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tSmeCmd *cmd;

	do {
		cmd = csr_get_command_buffer(mac_ctx);
		if (!cmd) {
			status = QDF_STATUS_E_RESOURCES;
			break;
		}
		qdf_mem_set(&cmd->u.scanCmd, sizeof(tScanCmd), 0);
		cmd->command = eSmeCommandScan;
		cmd->sessionId = (uint8_t) session_id;
		cmd->u.scanCmd.reason = eCsrScanLostLink3;
		cmd->u.scanCmd.callback = NULL;
		cmd->u.scanCmd.pContext = NULL;
		cmd->u.scanCmd.u.scanRequest.maxChnTime =
			mac_ctx->roam.configParam.nActiveMaxChnTime;
		cmd->u.scanCmd.u.scanRequest.minChnTime =
			mac_ctx->roam.configParam.nActiveMinChnTime;
		cmd->u.scanCmd.u.scanRequest.scanType = eSIR_ACTIVE_SCAN;
		wma_get_scan_id(&cmd->u.scanCmd.scanID);
		status = qdf_mc_timer_init(&cmd->u.scanCmd.csr_scan_timer,
			QDF_TIMER_TYPE_SW,
			csr_scan_active_list_timeout_handle, cmd);
		cmd->u.scanCmd.u.scanRequest.scan_id =
			cmd->u.scanCmd.scanID;
		qdf_set_macaddr_broadcast(&cmd->u.scanCmd.u.scanRequest.bssid);
		/* Put to the head of pending queue */
		status = csr_queue_sme_command(mac_ctx, cmd, true);
		if (!QDF_IS_STATUS_SUCCESS(status)) {
			sme_err("fail to send message status: %d", status);
			break;
		}
	} while (0);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_warn("failed with status %d", status);
		if (cmd)
			csr_release_command_scan(mac_ctx, cmd);
	}

	return status;
}

QDF_STATUS csr_scan_handle_search_for_ssid(tpAniSirGlobal pMac,
					   tSmeCmd *pCommand)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	tScanResultHandle hBSSList = CSR_INVALID_SCANRESULT_HANDLE;
	tCsrScanResultFilter *pScanFilter = NULL;
	tCsrRoamProfile *pProfile = pCommand->u.scanCmd.pToRoamProfile;
	uint32_t sessionId = pCommand->sessionId;

	do {
		/* If this scan is for LFR */
		if (pMac->roam.neighborRoamInfo[sessionId].
					uOsRequestedHandoff) {
			/* notify LFR state m/c */
			status = csr_neighbor_roam_sssid_scan_done(pMac,
						sessionId, QDF_STATUS_SUCCESS);
			if (QDF_STATUS_SUCCESS != status)
				csr_neighbor_roam_start_lfr_scan(pMac,
								 sessionId);
			status = QDF_STATUS_SUCCESS;
			break;
		}
		/*
		 * If there is roam command waiting, ignore this roam because
		 * the newer roam command is the one to execute
		 */
		if (csr_is_roam_command_waiting_for_session(pMac, sessionId)) {
			sme_warn("aborts because roam command waiting");
			break;
		}
		if (pProfile == NULL)
			break;
		pScanFilter = qdf_mem_malloc(sizeof(tCsrScanResultFilter));
		if (NULL == pScanFilter) {
			status = QDF_STATUS_E_NOMEM;
			break;
		}
		status = csr_roam_prepare_filter_from_profile(pMac, pProfile,
							      pScanFilter);
		if (!QDF_IS_STATUS_SUCCESS(status))
			break;
		status = csr_scan_get_result(pMac, pScanFilter, &hBSSList);
		if (!QDF_IS_STATUS_SUCCESS(status))
			break;
		if (pMac->roam.roamSession[sessionId].connectState ==
				eCSR_ASSOC_STATE_TYPE_INFRA_DISCONNECTING) {
			sme_err("upper layer issued disconnetion");
			status = QDF_STATUS_E_FAILURE;
			break;
		}
		status = csr_roam_issue_connect(pMac, sessionId, pProfile,
						hBSSList, eCsrHddIssued,
						pCommand->u.scanCmd.roamId,
						true, true);
	} while (0);

	if (!QDF_IS_STATUS_SUCCESS(status)) {
		if (CSR_INVALID_SCANRESULT_HANDLE != hBSSList)
			csr_scan_result_purge(pMac, hBSSList);
		/* We haven't done anything to this profile */
		csr_roam_call_callback(pMac, sessionId, NULL,
				       pCommand->u.scanCmd.roamId,
				       eCSR_ROAM_ASSOCIATION_FAILURE,
				       eCSR_ROAM_RESULT_SCAN_FOR_SSID_FAILURE);
	}
	if (pScanFilter) {
		csr_free_scan_filter(pMac, pScanFilter);
		qdf_mem_free(pScanFilter);
	}
	return status;
}

QDF_STATUS csr_scan_handle_search_for_ssid_failure(tpAniSirGlobal pMac,
						   tSmeCmd *pCommand)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	uint32_t sessionId = pCommand->sessionId;
	tCsrRoamProfile *pProfile = pCommand->u.scanCmd.pToRoamProfile;
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);
	eCsrRoamResult roam_result;

	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}
	/* If this scan is for LFR */
	if (pMac->roam.neighborRoamInfo[sessionId].uOsRequestedHandoff) {
		/* notify LFR state m/c */
		status = csr_neighbor_roam_sssid_scan_done(pMac, sessionId,
							QDF_STATUS_E_FAILURE);
		if (QDF_STATUS_SUCCESS != status)
			csr_neighbor_roam_start_lfr_scan(pMac, sessionId);
		return QDF_STATUS_SUCCESS;
	}
#ifdef WLAN_DEBUG
	if (pCommand->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs == 1) {
		char str[36];
		tSirMacSSid *ptr_ssid =
		&pCommand->u.scanCmd.u.scanRequest.SSIDs.SSIDList[0].SSID;
		qdf_mem_copy(str, ptr_ssid->ssId, ptr_ssid->length);
		str[ptr_ssid->length] = 0;
		sme_debug("SSID: %s", str);
	}
#endif
	/*
	 * Check whether it is for start ibss. No need to do anything if it
	 * is a JOIN request
	 */
	if (pProfile && CSR_IS_START_IBSS(pProfile)) {
		status = csr_roam_issue_connect(pMac, sessionId, pProfile, NULL,
				eCsrHddIssued, pCommand->u.scanCmd.roamId,
				true, true);
		if (!QDF_IS_STATUS_SUCCESS(status)) {
			sme_err(
				"failed to issue startIBSS, status: 0x%08X",
				status);
			csr_roam_call_callback(pMac, sessionId, NULL,
				pCommand->u.scanCmd.roamId, eCSR_ROAM_FAILED,
				eCSR_ROAM_RESULT_FAILURE);
		}
		return status;
	}
	roam_result = eCSR_ROAM_RESULT_FAILURE;
	if (NULL != pProfile && csr_is_bss_type_ibss(pProfile->BSSType)) {
		roam_result = eCSR_ROAM_RESULT_IBSS_START_FAILED;
		goto roam_completion;
	}
	/* Only indicate assoc_completion if we indicate assoc_start. */
	if (pSession->bRefAssocStartCnt > 0) {
		tCsrRoamInfo *pRoamInfo = NULL, roamInfo;

		qdf_mem_set(&roamInfo, sizeof(tCsrRoamInfo), 0);
		pRoamInfo = &roamInfo;
		if (pCommand->u.roamCmd.pRoamBssEntry) {
			struct tag_csrscan_result *pScanResult = GET_BASE_ADDR(
				pCommand->u.roamCmd.pRoamBssEntry,
				struct tag_csrscan_result, Link);
			roamInfo.pBssDesc = &pScanResult->Result.BssDescriptor;
		}
		roamInfo.statusCode = pSession->joinFailStatusCode.statusCode;
		roamInfo.reasonCode = pSession->joinFailStatusCode.reasonCode;
		pSession->bRefAssocStartCnt--;
		csr_roam_call_callback(pMac, sessionId, pRoamInfo,
				       pCommand->u.scanCmd.roamId,
				       eCSR_ROAM_ASSOCIATION_COMPLETION,
				       eCSR_ROAM_RESULT_SCAN_FOR_SSID_FAILURE);
	} else {
		csr_roam_call_callback(pMac, sessionId, NULL,
				       pCommand->u.scanCmd.roamId,
				       eCSR_ROAM_ASSOCIATION_FAILURE,
				       eCSR_ROAM_RESULT_SCAN_FOR_SSID_FAILURE);
	}
roam_completion:
	csr_roam_completion(pMac, sessionId, NULL, pCommand, roam_result,
			    false);
	return status;
}

QDF_STATUS csr_scan_result_purge(tpAniSirGlobal pMac,
				 tScanResultHandle hScanList)
{
	QDF_STATUS status = QDF_STATUS_E_INVAL;
	struct scan_result_list *pScanList =
				(struct scan_result_list *) hScanList;

	if (pScanList) {
		status = csr_ll_scan_purge_result(pMac, &pScanList->List);
		csr_ll_close(&pScanList->List);
		qdf_mem_free(pScanList);
	}
	return status;
}

/**
 * csr_remove_bssid_from_scan_list() - remove the bssid from
 * scan list
 * @mac_tx: mac context.
 * @bssid: bssid to be removed
 *
 * This function remove the given bssid from scan list.
 *
 * Return: void.
 */
void csr_remove_bssid_from_scan_list(tpAniSirGlobal mac_ctx,
			tSirMacAddr bssid)
{
	tListElem *entry, *free_elem;
	struct tag_csrscan_result *bss_desc;
	tDblLinkList *list = &mac_ctx->scan.scanResultList;

	csr_ll_lock(list);
	entry = csr_ll_peek_head(list, LL_ACCESS_NOLOCK);
	while (entry != NULL) {
		bss_desc = GET_BASE_ADDR(entry, struct tag_csrscan_result,
					Link);
		if (!qdf_mem_cmp(bss_desc->Result.BssDescriptor.bssId,
		   bssid, sizeof(tSirMacAddr))) {
			free_elem = entry;
			entry = csr_ll_next(list, entry, LL_ACCESS_NOLOCK);
			csr_ll_remove_entry(list, free_elem, LL_ACCESS_NOLOCK);
			csr_free_scan_result_entry(mac_ctx, bss_desc);
			sme_warn("Removed BSS entry:%pM", bssid);
			continue;
		}

		entry = csr_ll_next(list, entry, LL_ACCESS_NOLOCK);
	}

	csr_ll_unlock(list);
}

/**
 * csr_derive_prefer_value_from_rssi() - to derive prefer value
 * @mac_ctx: Global MAC Context
 * @rssi: RSSI of the BSS
 *
 * This routine will derive preferred value from given rssi
 *
 * Return: value between 0 to 14
 */
static int csr_derive_prefer_value_from_rssi(tpAniSirGlobal mac_ctx, int rssi)
{
	int i = CSR_NUM_RSSI_CAT - 1, pref_val = 0;

	while (i >= 0) {
		if (rssi >= mac_ctx->roam.configParam.RSSICat[i]) {
			pref_val = mac_ctx->roam.configParam.BssPreferValue[i];
			break;
		}
		i--;
	};
	return pref_val;
}

/**
 * csr_get_pcl_weight_of_channel() - Get PCL weight if channel is present in pcl
 * @mac_ctx: Global MAC Context
 * @channel_id: channel of bss
 * @filter: pointer to filter created through profile
 * @pcl_chan_weight: Get PCL weight for corresponding channel
 * @weight_list: Weight list for all the pcl channels.
 *
 * Get pcl_chan_weight if provided channel is present in pcl list
 *
 * Return: true or false
 */
static bool csr_get_pcl_weight_of_channel(tpAniSirGlobal mac_ctx,
		int channel_id,
		tCsrScanResultFilter *filter, int *pcl_chan_weight,
		uint8_t *weight_list)
{
	int i;
	bool status = false;

	if (NULL == filter)
		return status;

	for (i = 0; i < filter->pcl_channels.numChannels; i++) {
		if (filter->pcl_channels.channelList[i] == channel_id) {
			*pcl_chan_weight = weight_list[i];
			status = true;
			break;
		}
	}
	return status;
}
/**
 * csr_get_altered_rssi() - Artificially increase/decrease RSSI
 * @mac_ctx:         Global MAC Context pointer.
 * @rssi:            Actual RSSI of the AP.
 * @channel_id:      Channel on which the AP is parked.
 * @bssid:           BSSID of the AP to connect to.
 *
 * This routine will apply the boost and penalty parameters
 * if the channel_id is of 5G band and it will also apply
 * the preferred bssid score if there is a match between
 * the bssid and the global preferred bssid list.
 *
 * Return:          The modified RSSI Value
 */
static int csr_get_altered_rssi(tpAniSirGlobal mac_ctx, int rssi,
		uint8_t channel_id, struct qdf_mac_addr *bssid)
{
	int modified_rssi;
	int boost_factor;
	int penalty_factor;
	int i;
	struct roam_ext_params *roam_params;
	struct qdf_mac_addr fav_bssid;
	struct qdf_mac_addr local_bssid;

	modified_rssi = rssi;
	qdf_mem_zero(&local_bssid.bytes, QDF_MAC_ADDR_SIZE);
	if (bssid)
		qdf_mem_copy(local_bssid.bytes, bssid->bytes,
				QDF_MAC_ADDR_SIZE);
	roam_params = &mac_ctx->roam.configParam.roam_params;
	/*
	 * If the 5G pref feature is enabled, apply the roaming
	 * parameters to boost or penalize the rssi.
	 * Boost Factor = boost_factor * (Actual RSSI - boost Threshold)
	 * Penalty Factor = penalty factor * (penalty threshold - Actual RSSI)
	 */
	if (CSR_IS_SELECT_5G_PREFERRED(mac_ctx) &&
			CDS_IS_CHANNEL_5GHZ(channel_id)) {
		if (rssi > roam_params->raise_rssi_thresh_5g) {
			/* Check and boost the threshold*/
			boost_factor = roam_params->raise_factor_5g *
				(rssi - roam_params->raise_rssi_thresh_5g);
			/* Check and penalize the threshold */
			modified_rssi += CSR_MIN(roam_params->max_raise_rssi_5g,
						boost_factor);
		} else if (rssi < roam_params->drop_rssi_thresh_5g) {
			penalty_factor = roam_params->drop_factor_5g *
				(roam_params->drop_rssi_thresh_5g - rssi);
			modified_rssi -= CSR_MAX(roam_params->max_drop_rssi_5g,
						penalty_factor);
		}
		sme_debug("5G BSSID"MAC_ADDRESS_STR" AR=%d, MR=%d, ch=%d",
			MAC_ADDR_ARRAY(local_bssid.bytes),
			rssi, modified_rssi, channel_id);
	}
	/*
	 * Check if there are preferred bssid and then apply the
	 * preferred score
	 */
	qdf_mem_zero(&fav_bssid.bytes, QDF_MAC_ADDR_SIZE);
	if (bssid && roam_params->num_bssid_favored) {
		for (i = 0; i < roam_params->num_bssid_favored; i++) {
			qdf_mem_copy(fav_bssid.bytes,
					&roam_params->bssid_favored[i],
					QDF_MAC_ADDR_SIZE);
			if (!qdf_is_macaddr_equal(&fav_bssid, bssid))
				continue;
			modified_rssi += roam_params->bssid_favored_factor[i];
			sme_debug(
				"Pref"MAC_ADDRESS_STR" AR=%d, MR=%d, ch=%d",
				MAC_ADDR_ARRAY(local_bssid.bytes),
				rssi, modified_rssi, channel_id);
		}
	}
	return modified_rssi;
}

/**
 * csr_get_bss_prefer_value() - Get the preference value for BSS
 * @mac_ctx:               Global MAC Context
 * @rssi:                  RSSI of the BSS
 * @bssid:                 BSSID to which the preference value is returned
 * @channel_id:            Channel on which the AP is parked
 *
 * Each BSS descriptor should be assigned a preference value ranging from
 * 14-0, which will be used as an RSSI bucket score while sorting the
 * scan results.
 *
 * Return: Preference value for the BSSID
 */
static uint32_t csr_get_bss_prefer_value(tpAniSirGlobal mac_ctx, int rssi,
		struct qdf_mac_addr *bssid, int channel_id)
{
	uint32_t ret = 0;
	int modified_rssi;

	/*
	 * The RSSI does not get modified in case the 5G
	 * preference or preferred BSSID is not applicable
	 */
	modified_rssi = csr_get_altered_rssi(mac_ctx, rssi, channel_id, bssid);
	ret = csr_derive_prefer_value_from_rssi(mac_ctx, modified_rssi);

	return ret;
}

/* Return a CapValue base on the capabilities of a BSS */
static uint32_t csr_get_bss_cap_value(tpAniSirGlobal pMac,
				      tSirBssDescription *pBssDesc,
				      tDot11fBeaconIEs *pIes)
{
	uint32_t ret = CSR_BSS_CAP_VALUE_NONE;

	if (CSR_IS_ROAM_PREFER_5GHZ(pMac) || CSR_IS_SELECT_5G_PREFERRED(pMac)) {
		if ((pBssDesc) && CDS_IS_CHANNEL_5GHZ(pBssDesc->channelId))
			ret += CSR_BSS_CAP_VALUE_5GHZ;
	}
	/*
	 * if strict select 5GHz is non-zero then ignore the capability checking
	 */
	if (pIes && !CSR_IS_SELECT_5GHZ_MARGIN(pMac)) {
		/* We only care about 11N capability */
		if (pIes->VHTCaps.present)
			ret += CSR_BSS_CAP_VALUE_VHT;
		else if (pIes->HTCaps.present)
			ret += CSR_BSS_CAP_VALUE_HT;
		if (CSR_IS_QOS_BSS(pIes)) {
			ret += CSR_BSS_CAP_VALUE_WMM;
			/* Give advantage to UAPSD */
			if (CSR_IS_UAPSD_BSS(pIes))
				ret += CSR_BSS_CAP_VALUE_UAPSD;
		}
	}

	return ret;
}
/**
 * csr_is_better_bss() - Is bss1 better than bss2
 * @mac_ctx:             Global MAC Context pointer.
 * @bss1:                Pointer to the first BSS.
 * @bss2:                Pointer to the second BSS.
 *
 * This function helps in determining the preference value
 * of a particular BSS in the scan result which is further
 * used in the sorting logic of the final candidate AP's.
 *
 * Return:          true, if bss1 is better than bss2
 *                  false, if bss2 is better than bss1.
 */
static bool csr_is_better_bss(tpAniSirGlobal mac_ctx,
	struct tag_csrscan_result *bss1, struct tag_csrscan_result *bss2)
{
	bool ret;
	if (CSR_IS_BETTER_PREFER_VALUE(bss1->bss_score,
				bss2->bss_score))
		ret = true;
	else
		ret = false;
	return ret;
}

/* Add the channel to the occupiedChannels array */
static void csr_scan_add_to_occupied_channels(tpAniSirGlobal pMac,
					struct tag_csrscan_result *pResult,
					uint8_t sessionId,
					tCsrChannel *occupied_ch,
					tDot11fBeaconIEs *pIes,
					bool is_init_list)
{
	QDF_STATUS status;
	uint8_t ch;
	uint8_t num_occupied_ch = occupied_ch->numChannels;
	uint8_t *occupied_ch_lst = occupied_ch->channelList;

	ch = pResult->Result.BssDescriptor.channelId;
	if (!csr_neighbor_roam_connected_profile_match(pMac,
						sessionId, pResult, pIes))
		return;

	if (is_init_list)
		pMac->scan.roam_candidate_count[sessionId]++;

	if (csr_is_channel_present_in_list(occupied_ch_lst,
					   num_occupied_ch, ch))
		return;

	status = csr_add_to_channel_list_front(occupied_ch_lst,
					       num_occupied_ch, ch);
	if (QDF_IS_STATUS_SUCCESS(status)) {
		occupied_ch->numChannels++;
		sme_debug("Added channel %d to the list (count=%d)",
			ch, occupied_ch->numChannels);
		if (occupied_ch->numChannels >
		    CSR_BG_SCAN_OCCUPIED_CHANNEL_LIST_LEN)
			occupied_ch->numChannels =
				CSR_BG_SCAN_OCCUPIED_CHANNEL_LIST_LEN;
	}
}

/* Put the BSS into the scan result list */
/* pIes can not be NULL */
static void csr_scan_add_result(tpAniSirGlobal pMac, struct tag_csrscan_result
				*pResult,
				tDot11fBeaconIEs *pIes, uint32_t sessionId)
{
	tpCsrNeighborRoamControlInfo pNeighborRoamInfo =
		&pMac->roam.neighborRoamInfo[sessionId];

	struct qdf_mac_addr bssid;
	uint8_t channel_id = pResult->Result.BssDescriptor.channelId;

	qdf_mem_zero(&bssid.bytes, QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(bssid.bytes, &pResult->Result.BssDescriptor.bssId,
			QDF_MAC_ADDR_SIZE);
	pResult->preferValue = csr_get_bss_prefer_value(pMac,
				(int)pResult->Result.BssDescriptor.rssi,
				&bssid, channel_id);
	pResult->capValue = csr_get_bss_cap_value(pMac,
				&pResult->Result.BssDescriptor, pIes);
	csr_ll_insert_tail(&pMac->scan.scanResultList, &pResult->Link,
			   LL_ACCESS_LOCK);
	if (0 == pNeighborRoamInfo->cfgParams.channelInfo.numOfChannels) {
		/*
		 * Build the occupied channel list, only if
		 * "gNeighborScanChannelList" is NOT set in the cfg.ini file
		 */
		csr_scan_add_to_occupied_channels(pMac, pResult, sessionId,
				&pMac->scan.occupiedChannels[sessionId], pIes,
				false);
	}
}

static QDF_STATUS
csr_save_ies(tpAniSirGlobal pMac,
	     tCsrScanResultFilter *pFilter,
	     struct tag_csrscan_result *pBssDesc,
	     tDot11fBeaconIEs **pNewIes,
	     bool *fMatch,
	     eCsrEncryptionType *uc,
	     eCsrEncryptionType *mc,
	     eCsrAuthType *auth)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tDot11fBeaconIEs *pIes = NULL;

	if (!pFilter)
		return status;
	*fMatch = csr_match_bss(pMac, &pBssDesc->Result.BssDescriptor,
			       pFilter, auth, uc, mc, &pIes);
#ifdef WLAN_DEBUG_ROAM_OFFLOAD
	QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
		  FL("csr_match_bss fmatch %d"), *fMatch);
#endif
	if (NULL == pIes)
		return status;
	/* Only save it when matching */
	if (!(*fMatch) && !pBssDesc->Result.pvIes) {
		qdf_mem_free(pIes);
		return status;
	}
	if (!pBssDesc->Result.pvIes) {
		/*
		 * csr_match_bss allocates the memory. Simply pass it and it
		 * is freed later
		 */
		*pNewIes = pIes;
		return status;
	}
	/*
	 * The pIes is allocated by someone else. make a copy
	 * Only to save parsed IEs if caller provides a filter. Most likely the
	 * caller is using to for association, hence save the parsed IEs
	 */
	*pNewIes = qdf_mem_malloc(sizeof(tDot11fBeaconIEs));
	if (NULL == *pNewIes) {
		status = QDF_STATUS_E_NOMEM;
		sme_err("fail to allocate memory for IEs");
		/* Need to free memory allocated by csr_match_bss */
		if (!pBssDesc->Result.pvIes)
			qdf_mem_free(pIes);
		return status;
	}
	qdf_mem_copy(*pNewIes, pIes, sizeof(tDot11fBeaconIEs));
	return status;
}

/**
 * _csr_calculate_bss_score () - Calculate BSS score based on AP capabilty
 *                              and channel condition for best candidate
 *                              selection
 * @bss_info: bss information
 * @pcl_chan_weight: pcl weight of BSS channel
 * @nss: NSS supported by station
 *
 * Return : int32_t
 */
static int32_t _csr_calculate_bss_score(tSirBssDescription *bss_info,
		int pcl_chan_weight, int nss)
{
	int32_t score = 0;
	int32_t ap_load = 0;
	int32_t normalised_width = BEST_CANDIDATE_20MHZ;
	int32_t pcl_score = 0;
	int32_t temp_pcl_chan_weight = 0;
	int8_t  ap_nss = 0;
	int32_t est_air_time_percentage = 0;
	int32_t congestion = 0;
	int32_t rssi_diff = 0;
	int32_t rssi_weight = 0;

	/*
	 * Total weight of a BSSID is calculated on basis of 100 in which
	 * contribution of every factor is considered like this.
	 * RSSI: RSSI_WEIGHTAGE : 25
	 * HT_CAPABILITY_WEIGHTAGE: 7
	 * VHT_CAP_WEIGHTAGE: 5
	 * BEAMFORMING_CAP_WEIGHTAGE: 2
	 * CHAN_WIDTH_WEIGHTAGE:10
	 * CHAN_BAND_WEIGHTAGE: 5
	 * NSS: 5
	 * PCL: 10
	 * CHANNEL_CONGESTION: 5
	 * Reserved : 31
	 */
	/*
	 * Further bucketization of rssi is also done out of 25 score.
	 * RSSI > -55=> weight = 2500
	 * RSSI > -60=> weight = 2250
	 * RSSI >-65 =>weight = 2000
	 * RSSI > -70=> weight = 1750
	 * RSSI > -75=> weight = 1500
	 * RSSI > -80=> weight = 1250
	 */
	if (bss_info->rssi) {
		/*
		 * if RSSI of AP is less then -80, driver should ignore that
		 * candidate.
		 */
		if (bss_info->rssi < BAD_RSSI) {
			QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
			FL("Drop this BSS "MAC_ADDRESS_STR " due to low rssi %d"),
			MAC_ADDR_ARRAY(bss_info->bssId), bss_info->rssi);
			score = 0;
			return score;
		}
		if (bss_info->rssi >= EXCELLENT_RSSI) {
			rssi_weight = EXCELLENT_RSSI_WEIGHT *
					RSSI_WEIGHTAGE;
		} else {
			rssi_diff = EXCELLENT_RSSI -
				bss_info->rssi;
			do_div(rssi_diff, 5);
			rssi_weight = (rssi_diff + 1) * RSSI_WEIGHT_BUCKET;
			rssi_weight = (EXCELLENT_RSSI_WEIGHT *
					RSSI_WEIGHTAGE) - rssi_weight;

		}
		score += rssi_weight;
	}

	/* If BSS is in PCL list extra pcl Weight is added n % */
	if (pcl_chan_weight) {
		temp_pcl_chan_weight =
			(MAX_WEIGHT_OF_PCL_CHANNELS - pcl_chan_weight);
		do_div(temp_pcl_chan_weight,
				PCL_GROUPS_WEIGHT_DIFFERENCE);
		pcl_score = PCL_WEIGHT - temp_pcl_chan_weight;

		if (pcl_score < 0)
			pcl_score = 0;

		score += pcl_score * BEST_CANDIDATE_MAX_WEIGHT;
	}
	/* If AP supports HT caps, extra 10% score will be added */
	if (bss_info->ht_caps_present)
		score += BEST_CANDIDATE_MAX_WEIGHT * HT_CAPABILITY_WEIGHTAGE;

	/* If AP supports VHT caps, Extra 6% score will be added to score */
	if (bss_info->vht_caps_present)
		score += BEST_CANDIDATE_MAX_WEIGHT * VHT_CAP_WEIGHTAGE;

	/* If AP supports beam forming, extra 2% score will be added to score.*/
	if (bss_info->beacomforming_capable)
		score += BEST_CANDIDATE_MAX_WEIGHT * BEAMFORMING_CAP_WEIGHTAGE;

	/*
	 * Channel width is again calculated on basis of 100.
	 * Where if AP is
	 * 80MHZ = 100
	 * 40MHZ = 70
	 * 20MHZ = 30 weightage is given out of 100.
	 * Channel width weightage is given as CHAN_WIDTH_WEIGHTAGE (10%).
	 */
	if (bss_info->chan_width == eHT_CHANNEL_WIDTH_80MHZ)
		normalised_width = BEST_CANDIDATE_80MHZ;
	else if (bss_info->chan_width == eHT_CHANNEL_WIDTH_40MHZ)
		normalised_width = BEST_CANDIDATE_40MHZ;
	else
		normalised_width = BEST_CANDIDATE_20MHZ;
	score += normalised_width * CHAN_WIDTH_WEIGHTAGE;

	/* If AP is on 5Ghz channel , extra score of 5% is added to BSS score.*/
	if (get_rf_band(bss_info->channelId) == SIR_BAND_5_GHZ &&
			bss_info->rssi > RSSI_THRESHOLD_5GHZ)
		score += BEST_CANDIDATE_MAX_WEIGHT * CHAN_BAND_WEIGHTAGE;
	/*
	 * If ESP is being transmitted by the AP, use the estimated airtime for
	 * AC_BE from that, Estimated airtime 0-25% = 120, 25-50% = 250, 50-75%
	 * = 370, 75-100% = 500.
	 * Else if QBSSLoad is being transmitted and QBSSLoad < 25% = 500
	 * else assing default weight of 370
	 */

	if (bss_info->air_time_fraction) {
		/* Convert 0-255 range to percentage */
		est_air_time_percentage =
			bss_info->air_time_fraction * ROAM_MAX_CHANNEL_WEIGHT;
		do_div(est_air_time_percentage,
				MAX_ESTIMATED_AIR_TIME_FRACTION);
		/*
		 * Calculate channel congestion from estimated air time
		 * fraction.
		 */
		congestion = MAX_CHANNEL_UTILIZATION - est_air_time_percentage;

		if (congestion >= LOW_CHANNEL_CONGESTION &&
				congestion < MODERATE_CHANNEL_CONGESTION)
			score += LOW_CHANNEL_CONGESTION_WEIGHT;
		else if (congestion >= MODERATE_CHANNEL_CONGESTION &&
				congestion < CONSIDERABLE_CHANNEL_CONGESTION)
			score += MODERATE_CHANNEL_CONGESTION_WEIGHT;
		else if (congestion >= CONSIDERABLE_CHANNEL_CONGESTION &&
				congestion < HIGH_CHANNEL_CONGESTION)
			score += CONSIDERABLE_CHANNEL_CONGESTION_WEIGHT;
		else
			score += HIGH_CHANNEL_CONGESTION_WEIGHT;
	} else if (bss_info->QBSSLoad_present) {
		ap_load = (bss_info->qbss_chan_load *
				BEST_CANDIDATE_MAX_WEIGHT);
		/*
		 * Calculate ap_load in % from qbss channel load from 0-255
		 * range
		 */
		do_div(ap_load, MAX_AP_LOAD);
		congestion = ap_load;
		if (congestion < MODERATE_CHANNEL_CONGESTION)
			score += LOW_CHANNEL_CONGESTION_WEIGHT;
		else
			score += HIGH_CHANNEL_CONGESTION_WEIGHT;
	} else {
		score += MODERATE_CHANNEL_CONGESTION_WEIGHT;
	}
	ap_nss = bss_info->nss;
	if (wma_is_current_hwmode_dbs())
		nss--;
	if (nss == 2 && ap_nss == 1)
		score += BEST_CANDIDATE_MAX_WEIGHT * NSS_1X1_WEIGHTAGE;
	else
		score += BEST_CANDIDATE_MAX_WEIGHT * NSS_WEIGHTAGE;

	QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
		FL("BSSID:"MAC_ADDRESS_STR" rssi=%d htcaps=%d vht=%d bw=%d channel=%d beamforming=%d ap_load=%d est_air_time_percentage=%d pcl_score %d Final Score %d ap_NSS %d  nss %d"),
		MAC_ADDR_ARRAY(bss_info->bssId),
		bss_info->rssi, bss_info->ht_caps_present,
		bss_info->vht_caps_present, bss_info->chan_width,
		bss_info->channelId,
		bss_info->beacomforming_capable, ap_load,
		est_air_time_percentage,
		pcl_score,
		score, ap_nss, nss);

	return score;
}
/**
 * csr_calculate_bss_score() - Calculate candidate AP score for Best
 * candidate selection for connection
 * @mac_ctx: Pointer to mac context
 * @bss: BSSID
 * @pcl_chan_weight: pcl weight for bss channel
 *
 * Return: void
 *
 * Calculate candidate AP score for Best candidate selection for connection.
 */
static void csr_calculate_bss_score(tpAniSirGlobal pMac,
		struct tag_csrscan_result *pBss,
		int pcl_chan_weight)
{
	int32_t score = 0;
	int channel_id;
	int nss = 1;
	tSirBssDescription *bss_info = &(pBss->Result.BssDescriptor);

	channel_id = cds_get_channel_enum(pBss->Result.BssDescriptor.channelId);
	if (pMac->roam.configParam.enable2x2)
		nss = 2;

	if (channel_id < NUM_CHANNELS)
		score = _csr_calculate_bss_score(bss_info,
			pcl_chan_weight, nss);

	pBss->bss_score = score;
	return;
}

static QDF_STATUS
csr_save_scan_entry(tpAniSirGlobal pMac,
		tCsrScanResultFilter *pFilter,
		bool fMatch,
		struct tag_csrscan_result *pBssDesc,
		tDot11fBeaconIEs *pNewIes,
		struct scan_result_list *pRetList,
		uint32_t *count,
		eCsrEncryptionType uc,
		eCsrEncryptionType mc,
		eCsrAuthType *auth,
		uint8_t *weight_list)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct tag_csrscan_result *pResult;
	uint32_t bssLen, allocLen;
	/* To sort the list */
	tListElem *pTmpEntry;
	struct tag_csrscan_result *pTmpResult;
	int pcl_chan_weight = 0;

	if (!(NULL == pFilter || fMatch))
		return status;

	bssLen = pBssDesc->Result.BssDescriptor.length +
		sizeof(pBssDesc->Result.BssDescriptor.length);
	allocLen = sizeof(struct tag_csrscan_result) + bssLen;
	pResult = qdf_mem_malloc(allocLen);
	if (NULL == pResult) {
		status = QDF_STATUS_E_NOMEM;
		sme_err(
			"fail to allocate memory for scan result, len: %d",
			allocLen);
		if (pNewIes)
			qdf_mem_free(pNewIes);
		return status;
	}
	pResult->capValue = pBssDesc->capValue;
	pResult->preferValue = pBssDesc->preferValue;
	pResult->ucEncryptionType = uc;
	pResult->mcEncryptionType = mc;
	pResult->authType = *auth;
	pResult->Result.ssId = pBssDesc->Result.ssId;
	pResult->Result.timer = pBssDesc->Result.timer;
	/* save the pIes for later use */
	pResult->Result.pvIes = pNewIes;
	pResult->bss_score = 0;
	/* save bss description */
	qdf_mem_copy(&pResult->Result.BssDescriptor,
		     &pBssDesc->Result.BssDescriptor,
		     bssLen);
	/*
	 * calculate pcl_chan_weight from weight list to honor PCL list
	 */
	if (pFilter  && pFilter->pcl_channels.numChannels > 0 &&
		(pBssDesc->Result.BssDescriptor.rssi > PCL_RSSI_THRESHOLD)) {
		if (csr_get_pcl_weight_of_channel(pMac,
				pBssDesc->Result.BssDescriptor.channelId,
				pFilter, &pcl_chan_weight, weight_list)) {
			QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
				FL(" pcl channel %d pcl_chan_weight %d"),
				pBssDesc->Result.BssDescriptor.channelId,
				pcl_chan_weight);
		}
	}
	if (pFilter)
		csr_calculate_bss_score(pMac, pResult, pcl_chan_weight);

	/*
	 * No need to lock pRetList because it is locally allocated and no
	 * outside can access it at this time
	 */
	if (csr_ll_is_list_empty(&pRetList->List, LL_ACCESS_NOLOCK)) {
		csr_ll_insert_tail(&pRetList->List, &pResult->Link,
				   LL_ACCESS_NOLOCK);
		(*count)++;
		return status;
	}
	if (pFilter &&
	   !qdf_mem_cmp(pResult->Result.BssDescriptor.bssId,
	   pFilter->bssid_hint.bytes, QDF_MAC_ADDR_SIZE) &&
	   pMac->roam.configParam.is_bssid_hint_priority) {
		/* bssid hint AP should be on head */
		csr_ll_insert_head(&pRetList->List,
			&pResult->Link, LL_ACCESS_NOLOCK);
		(*count)++;
		pResult->bss_score = BEST_CANDIDATE_MAX_BSS_SCORE;
		return status;
	}
	pTmpEntry = csr_ll_peek_head(&pRetList->List, LL_ACCESS_NOLOCK);
	while (pTmpEntry) {
		pTmpResult = GET_BASE_ADDR(pTmpEntry, struct tag_csrscan_result,
					Link);

		if (csr_is_better_bss(pMac, pResult, pTmpResult)) {
			csr_ll_insert_entry(&pRetList->List, pTmpEntry,
					    &pResult->Link, LL_ACCESS_NOLOCK);
			/* To indicate we are done */
			pResult = NULL;
			break;
		}
		pTmpEntry = csr_ll_next(&pRetList->List,
					pTmpEntry, LL_ACCESS_NOLOCK);
	}
	if (pResult != NULL) {
		/* This one is not better than any one */
		csr_ll_insert_tail(&pRetList->List, &pResult->Link,
				   LL_ACCESS_NOLOCK);
	}
	(*count)++;
	return status;
}

static QDF_STATUS
csr_parse_scan_results(tpAniSirGlobal pMac,
		       tCsrScanResultFilter *pFilter,
		       struct scan_result_list *pRetList,
		       uint32_t *count)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tListElem *pEntry;
	bool fMatch = false;
	struct tag_csrscan_result *pBssDesc = NULL;
	tDot11fBeaconIEs *pIes, *pNewIes = NULL;
	eCsrEncryptionType uc, mc;
	eCsrAuthType auth = eCSR_AUTH_TYPE_OPEN_SYSTEM;
	uint32_t len = 0;
	enum cds_con_mode new_mode;
	uint8_t weight_list[QDF_MAX_NUM_CHAN];


	csr_ll_lock(&pMac->scan.scanResultList);

	if (pFilter && (0 == pFilter->BSSIDs.numOfBSSIDs)) {
		if (cds_map_concurrency_mode(
					&pFilter->csrPersona, &new_mode)) {
			status = cds_get_pcl(new_mode,
				&pFilter->pcl_channels.channelList[0], &len,
				weight_list, QDF_ARRAY_SIZE(weight_list));
			pFilter->pcl_channels.numChannels = (uint8_t)len;
		}
	}

	if (QDF_STATUS_E_FAILURE == status)
		sme_err("Retrieving pcl failed from HDD");

	pEntry = csr_ll_peek_head(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);
	while (pEntry) {
		pBssDesc = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		pIes = (tDot11fBeaconIEs *) (pBssDesc->Result.pvIes);
		/*
		 * if pBssDesc->Result.pvIes is NULL, we need to free any memory
		 * allocated by csr_match_bss for any error condition,
		 * otherwiase, it will be freed later
		 */
		fMatch = false;
		pNewIes = NULL;
		status = csr_save_ies(pMac, pFilter, pBssDesc, &pNewIes,
				      &fMatch, &uc, &mc, &auth);
		if (!QDF_IS_STATUS_SUCCESS(status)) {
			sme_debug("save ies fail %d", status);
			break;
		}
		status = csr_save_scan_entry(pMac, pFilter, fMatch, pBssDesc,
					     pNewIes, pRetList, count, uc, mc,
					     &auth, weight_list);
		if (!QDF_IS_STATUS_SUCCESS(status)) {
			sme_debug("save entry fail %d",	status);
			break;
		}
		pEntry = csr_ll_next(&pMac->scan.scanResultList, pEntry,
				     LL_ACCESS_NOLOCK);
	} /* while */
	csr_ll_unlock(&pMac->scan.scanResultList);
	return status;
}

/**
 * csr_remove_ap_due_to_rssi() - check if bss is present in
 * list of BSSID which rejected Assoc due to RSSI
 * @list: rssi based rejected BSS list
 * @bss_descr: pointer to bss description
 *
 * Check if the time interval indicated in last Assoc reject
 * has expired OR rssi has improved by margin indicated
 * in last Assoc reject. If any of the condition match remove
 * the AP from the avoid list, else do not try to conenct
 * to the AP
 *
 * Return: true if connection cannot be tried with AP else false
 */
static bool csr_remove_ap_due_to_rssi(qdf_list_t *list,
	tSirBssDescription *bss_descr)
{
	QDF_STATUS status;
	struct sir_rssi_disallow_lst *cur_node = NULL;
	qdf_list_node_t *cur_lst = NULL, *next_lst = NULL;
	qdf_time_t cur_time;
	uint32_t time_diff;

	if (!qdf_list_size(list))
		return false;

	cur_time = qdf_do_div(qdf_get_monotonic_boottime(),
		QDF_MC_TIMER_TO_MS_UNIT);

	qdf_list_peek_front(list, &cur_lst);
	while (cur_lst) {
		cur_node = qdf_container_of(cur_lst,
				struct sir_rssi_disallow_lst, node);

		qdf_list_peek_next(list, cur_lst, &next_lst);

		time_diff = cur_time - cur_node->time_during_rejection;
		if ((time_diff > cur_node->retry_delay)) {
			sme_debug("Remove %pM as time diff %d is greater retry delay %d",
				cur_node->bssid.bytes, time_diff,
				cur_node->retry_delay);
			status = qdf_list_remove_node(list, cur_lst);
			if (QDF_IS_STATUS_SUCCESS(status))
				qdf_mem_free(cur_node);
			cur_lst = next_lst;
			next_lst = NULL;
			cur_node = NULL;
			continue;
		}

		if (!qdf_mem_cmp(cur_node->bssid.bytes,
		    bss_descr->bssId, QDF_MAC_ADDR_SIZE))
			break;
		cur_lst = next_lst;
		next_lst = NULL;
		cur_node = NULL;
	}

	if (cur_node) {
		time_diff = cur_time - cur_node->time_during_rejection;
		if (!(time_diff > cur_node->retry_delay ||
		   bss_descr->rssi_raw >= cur_node->expected_rssi)) {
			sme_err("Don't Attempt to connect %pM (time diff %d retry delay %d rssi %d expected rssi %d)",
				cur_node->bssid.bytes, time_diff,
				cur_node->retry_delay, bss_descr->rssi_raw,
				cur_node->expected_rssi);
			return true;
		}
		sme_debug("Remove %pM as time diff %d is greater retry delay %d or RSSI %d is greater than expected %d",
				cur_node->bssid.bytes, time_diff,
				cur_node->retry_delay,
				bss_descr->rssi_raw,
				cur_node->expected_rssi);
		status = qdf_list_remove_node(list, cur_lst);
		if (QDF_IS_STATUS_SUCCESS(status))
			qdf_mem_free(cur_node);
	}

	return false;
}

/**
 * csr_filter_ap_due_to_rssi_reject() - filter the AP who has sent
 * assoc reject due to RSSI if condition has not improved
 * @mac_ctx: mac context
 * @scan_list: candidate list for the connection
 *
 * Return: void
 */
static void csr_filter_ap_due_to_rssi_reject(tpAniSirGlobal mac_ctx,
	struct scan_result_list *scan_list)
{
	tListElem *cur_entry;
	tListElem *next_entry;
	struct tag_csrscan_result *scan_res;
	bool remove;

	if (!scan_list ||
	   !qdf_list_size(&mac_ctx->roam.rssi_disallow_bssid))
		return;

	csr_ll_lock(&scan_list->List);

	cur_entry = csr_ll_peek_head(&scan_list->List, LL_ACCESS_NOLOCK);
	while (cur_entry) {
		scan_res = GET_BASE_ADDR(cur_entry, struct tag_csrscan_result,
					Link);
		next_entry = csr_ll_next(&scan_list->List,
						cur_entry, LL_ACCESS_NOLOCK);
		remove = csr_remove_ap_due_to_rssi(
			&mac_ctx->roam.rssi_disallow_bssid,
			&scan_res->Result.BssDescriptor);
		if (remove) {
			csr_ll_remove_entry(&scan_list->List,
				cur_entry, LL_ACCESS_NOLOCK);
			csr_free_scan_result_entry(mac_ctx, scan_res);
		}
		cur_entry = next_entry;
		next_entry = NULL;
	}
	csr_ll_unlock(&scan_list->List);

}

QDF_STATUS csr_scan_get_result(tpAniSirGlobal pMac,
			       tCsrScanResultFilter *pFilter,
			       tScanResultHandle *phResult)
{
	QDF_STATUS status;
	struct scan_result_list *pRetList;
	uint32_t count = 0;

	if (phResult)
		*phResult = CSR_INVALID_SCANRESULT_HANDLE;

	pRetList = qdf_mem_malloc(sizeof(struct scan_result_list));
	if (NULL == pRetList) {
		sme_err("pRetList is NULL");
		return QDF_STATUS_E_NOMEM;
	}

	csr_ll_open(pMac->hHdd, &pRetList->List);
	pRetList->pCurEntry = NULL;
	status = csr_parse_scan_results(pMac, pFilter, pRetList, &count);
	sme_debug("return %d BSS %d",
		status, csr_ll_count(&pRetList->List));
	if (!QDF_IS_STATUS_SUCCESS(status) || (phResult == NULL)) {
		/* Fail or No one wants the result. */
		csr_scan_result_purge(pMac, (tScanResultHandle) pRetList);
	} else {
		if (pFilter)
			csr_filter_ap_due_to_rssi_reject(pMac, pRetList);
		if (0 == count ||
		   !csr_ll_count(&pRetList->List)) {
			/* We are here meaning the there is no match */
			csr_ll_close(&pRetList->List);
			qdf_mem_free(pRetList);
			status = QDF_STATUS_E_NULL_VALUE;
		} else if (phResult) {
			*phResult = pRetList;
		}
	}
	return status;
}

tCsrScanResultInfo *csr_scan_get_result_for_bssid(tpAniSirGlobal mac_ctx,
						  struct qdf_mac_addr *bssid)
{
	tDblLinkList *list = &mac_ctx->scan.scanResultList;
	tListElem *entry;
	tCsrScanResultInfo *scan_info = NULL;
	struct tag_csrscan_result *scan_res;
	size_t bss_len, len;
	tDot11fBeaconIEs *ies = NULL;
	QDF_STATUS status;

	csr_ll_lock(list);
	entry = csr_ll_peek_head(list, LL_ACCESS_NOLOCK);
	while (entry) {
		scan_res = GET_BASE_ADDR(entry, struct tag_csrscan_result,
					Link);
		if (qdf_mem_cmp(scan_res->Result.BssDescriptor.bssId,
				bssid->bytes, QDF_MAC_ADDR_SIZE) == 0) {
			/* Found matching scan resut for this bssid */
			bss_len = scan_res->Result.BssDescriptor.length +
				  sizeof(scan_res->Result.BssDescriptor.length);
			len = sizeof(*scan_info) + bss_len;
			scan_info = qdf_mem_malloc(len);
			if (!scan_info) {
				sme_err("Failed to allocate memory for scan info, len = %zu",
					len);
				break;
			}
			scan_info->ssId = scan_res->Result.ssId;
			scan_info->timer = scan_res->Result.timer;
			qdf_mem_copy(&scan_info->BssDescriptor,
				     &scan_res->Result.BssDescriptor, bss_len);
			status = csr_get_parsed_bss_description_ies(
						mac_ctx,
						&scan_info->BssDescriptor,
						&ies);
			if (QDF_IS_STATUS_ERROR(status)) {
				qdf_mem_free(scan_info);
				scan_info = NULL;
			} else {
				scan_info->pvIes = ies;
			}
			break;
		}
		entry = csr_ll_next(list, entry, LL_ACCESS_NOLOCK);
	}
	csr_ll_unlock(list);

	return scan_info;
}

/*
 * NOTE: This routine is being added to make
 * sure that scan results are not being flushed
 * while roaming. If the scan results are flushed,
 * we are unable to recover from
 * csr_roam_roaming_state_disassoc_rsp_processor.
 * If it is needed to remove this routine,
 * first ensure that we recover gracefully from
 * csr_roam_roaming_state_disassoc_rsp_processor if
 * csr_scan_get_result returns with a failure because
 * of not being able to find the roaming BSS.
 */
static bool csr_scan_flush_denied(tpAniSirGlobal pMac)
{
	uint8_t sessionId;

	for (sessionId = 0; sessionId < CSR_ROAM_SESSION_MAX; sessionId++) {
		if (CSR_IS_SESSION_VALID(pMac, sessionId)) {
			if (csr_neighbor_middle_of_roaming(pMac, sessionId))
				return 1;
		}
	}
	return 0;
}

QDF_STATUS csr_scan_flush_result(tpAniSirGlobal pMac)
{
	bool isFlushDenied = csr_scan_flush_denied(pMac);

	if (isFlushDenied) {
		sme_warn("scan flush denied in roam state %d", isFlushDenied);
		return QDF_STATUS_E_FAILURE;
	}

	sme_debug("Flushing all scan results");
	csr_ll_scan_purge_result(pMac, &pMac->scan.tempScanResults);
	csr_ll_scan_purge_result(pMac, &pMac->scan.scanResultList);
	return QDF_STATUS_SUCCESS;
}

QDF_STATUS csr_scan_flush_selective_result(tpAniSirGlobal pMac, bool flushP2P)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tListElem *pEntry, *pFreeElem;
	struct tag_csrscan_result *pBssDesc;
	tDblLinkList *pList = &pMac->scan.scanResultList;

	csr_ll_lock(pList);

	pEntry = csr_ll_peek_head(pList, LL_ACCESS_NOLOCK);
	while (pEntry != NULL) {
		pBssDesc = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		if (flushP2P && !qdf_mem_cmp(pBssDesc->Result.ssId.ssId,
						"DIRECT-", 7)) {
			pFreeElem = pEntry;
			pEntry = csr_ll_next(pList, pEntry, LL_ACCESS_NOLOCK);
			csr_ll_remove_entry(pList, pFreeElem, LL_ACCESS_NOLOCK);
			csr_free_scan_result_entry(pMac, pBssDesc);
			continue;
		}
		pEntry = csr_ll_next(pList, pEntry, LL_ACCESS_NOLOCK);
	}

	csr_ll_unlock(pList);

	return status;
}

void csr_scan_flush_bss_entry(tpAniSirGlobal pMac,
			      tpSmeCsaOffloadInd pCsaOffloadInd)
{
	tListElem *pEntry, *pFreeElem;
	struct tag_csrscan_result *pBssDesc;
	tDblLinkList *pList = &pMac->scan.scanResultList;

	csr_ll_lock(pList);

	pEntry = csr_ll_peek_head(pList, LL_ACCESS_NOLOCK);
	while (pEntry != NULL) {
		pBssDesc = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		if (!qdf_mem_cmp(pBssDesc->Result.BssDescriptor.bssId,
			pCsaOffloadInd->bssid.bytes, QDF_MAC_ADDR_SIZE)) {
			pFreeElem = pEntry;
			pEntry = csr_ll_next(pList, pEntry, LL_ACCESS_NOLOCK);
			csr_ll_remove_entry(pList, pFreeElem, LL_ACCESS_NOLOCK);
			csr_free_scan_result_entry(pMac, pBssDesc);
			sme_debug("Removed BSS entry:%pM",
				pCsaOffloadInd->bssid.bytes);
			continue;
		}

		pEntry = csr_ll_next(pList, pEntry, LL_ACCESS_NOLOCK);
	}

	csr_ll_unlock(pList);
}

/**
 * csr_check11d_channel
 *
 ***FUNCTION:
 * This function is called from csr_scan_filter_results function and
 * compare channel number with given channel list.
 *
 ***LOGIC:
 * Check Scan result channel number with CFG channel list
 *
 ***ASSUMPTIONS:
 *
 *
 ***NOTE:
 *
 * @param  channelId      channel number
 * @param  pChannelList   Pointer to channel list
 * @param  numChannels    Number of channel in channel list
 *
 * @return Status
 */

static QDF_STATUS csr_check11d_channel(uint8_t channelId, uint8_t *pChannelList,
				uint32_t numChannels)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	uint8_t i = 0;

	for (i = 0; i < numChannels; i++) {
		if (pChannelList[i] == channelId) {
			status = QDF_STATUS_SUCCESS;
			break;
		}
	}
	return status;
}

/**
 * csr_scan_filter_results
 *
 ***FUNCTION:
 * This function is called from csr_apply_country_information function and
 * filter scan result based on valid channel list number.
 *
 ***LOGIC:
 * Get scan result from scan list and Check Scan result channel number
 * with 11d channel list if channel number is found in 11d channel list
 * then do not remove scan result entry from scan list
 *
 ***ASSUMPTIONS:
 *
 *
 ***NOTE:
 *
 * @param  pMac        Pointer to Global MAC structure
 *
 * @return Status
 */

QDF_STATUS csr_scan_filter_results(tpAniSirGlobal pMac)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tListElem *pEntry, *pTempEntry;
	struct tag_csrscan_result *pBssDesc;
	uint32_t len = sizeof(pMac->roam.validChannelList);

	/* Get valid channels list from CFG */
	if (!QDF_IS_STATUS_SUCCESS(csr_get_cfg_valid_channels(pMac,
							      pMac->roam.
							      validChannelList,
							      &len))) {
		sme_err("Failed to get Channel list from CFG");
	}

	csr_ll_lock(&pMac->scan.scanResultList);
	pEntry = csr_ll_peek_head(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);
	while (pEntry) {
		pBssDesc = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		pTempEntry = csr_ll_next(&pMac->scan.scanResultList, pEntry,
					 LL_ACCESS_NOLOCK);
		if (csr_check11d_channel(pBssDesc->Result.BssDescriptor.
				channelId, pMac->roam.validChannelList, len)) {
			/* Remove Scan result which does not have 11d channel */
			if (csr_ll_remove_entry(&pMac->scan.scanResultList,
				 pEntry, LL_ACCESS_NOLOCK))
				csr_free_scan_result_entry(pMac, pBssDesc);
		} else
			sme_debug("%d is a Valid channel",
				pBssDesc->Result.BssDescriptor.channelId);
		pEntry = pTempEntry;
	}

	csr_ll_unlock(&pMac->scan.scanResultList);
	csr_ll_lock(&pMac->scan.tempScanResults);

	pEntry = csr_ll_peek_head(&pMac->scan.tempScanResults,
					 LL_ACCESS_NOLOCK);
	while (pEntry) {
		pBssDesc = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		pTempEntry = csr_ll_next(&pMac->scan.tempScanResults, pEntry,
					 LL_ACCESS_NOLOCK);
		if (csr_check11d_channel(pBssDesc->Result.BssDescriptor.channelId,
					 pMac->roam.validChannelList, len)) {
			/* Remove Scan result which does not have 11d channel */
			if (csr_ll_remove_entry
				    (&pMac->scan.tempScanResults, pEntry,
				    LL_ACCESS_NOLOCK))
				csr_free_scan_result_entry(pMac, pBssDesc);
		} else
			sme_debug("%d is a Valid channel",
				pBssDesc->Result.BssDescriptor.channelId);
		pEntry = pTempEntry;
	}

	csr_ll_unlock(&pMac->scan.tempScanResults);
	return status;
}

QDF_STATUS csr_scan_copy_result_list(tpAniSirGlobal pMac, tScanResultHandle hIn,
				     tScanResultHandle *phResult)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct scan_result_list *pRetList, *pInList =
						(struct scan_result_list *) hIn;
	struct tag_csrscan_result *pResult, *pScanResult;
	uint32_t count = 0;
	tListElem *pEntry;
	uint32_t bssLen, allocLen;

	if (phResult)
		*phResult = CSR_INVALID_SCANRESULT_HANDLE;

	pRetList = qdf_mem_malloc(sizeof(struct scan_result_list));
	if (NULL == pRetList)
		status = QDF_STATUS_E_NOMEM;
	else {
		csr_ll_open(pMac->hHdd, &pRetList->List);
		pRetList->pCurEntry = NULL;
		csr_ll_lock(&pMac->scan.scanResultList);
		csr_ll_lock(&pInList->List);

		pEntry = csr_ll_peek_head(&pInList->List, LL_ACCESS_NOLOCK);
		while (pEntry) {
			pScanResult =
				GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
						Link);
			bssLen = pScanResult->Result.BssDescriptor.length +
			sizeof(pScanResult->Result.BssDescriptor.length);
			allocLen = sizeof(struct tag_csrscan_result) + bssLen;
			pResult = qdf_mem_malloc(allocLen);
			if (NULL == pResult)
				status = QDF_STATUS_E_NOMEM;
			else
				status = QDF_STATUS_SUCCESS;
			if (!QDF_IS_STATUS_SUCCESS(status)) {
				csr_scan_result_purge(pMac,
						      (tScanResultHandle *)
						      pRetList);
				count = 0;
				break;
			}
			qdf_mem_set(pResult, allocLen, 0);
			qdf_mem_copy(&pResult->Result.BssDescriptor,
				     &pScanResult->Result.BssDescriptor,
				     bssLen);
			if (pScanResult->Result.pvIes) {
				pResult->Result.pvIes =
				qdf_mem_malloc(sizeof(tDot11fBeaconIEs));
				if (NULL == pResult->Result.pvIes)
					status = QDF_STATUS_E_NOMEM;
				else
					status = QDF_STATUS_SUCCESS;
				if (!QDF_IS_STATUS_SUCCESS(status)) {
					/* Free the memory we allocate above */
					qdf_mem_free(pResult);
					csr_scan_result_purge(pMac,
							   (tScanResultHandle *)
							      pRetList);
					count = 0;
					break;
				}
				qdf_mem_copy(pResult->Result.pvIes,
					     pScanResult->Result.pvIes,
					     sizeof(tDot11fBeaconIEs));
			}
			csr_ll_insert_tail(&pRetList->List, &pResult->Link,
					   LL_ACCESS_LOCK);
			count++;
			pEntry = csr_ll_next(&pInList->List, pEntry,
						LL_ACCESS_NOLOCK);
		} /* while */
		csr_ll_unlock(&pInList->List);
		csr_ll_unlock(&pMac->scan.scanResultList);

		if (QDF_IS_STATUS_SUCCESS(status)) {
			if (0 == count) {
				csr_ll_close(&pRetList->List);
				qdf_mem_free(pRetList);
				status = QDF_STATUS_E_NULL_VALUE;
			} else if (phResult) {
				*phResult = pRetList;
			}
		}
	} /* Allocated pRetList */

	return status;
}

QDF_STATUS csr_scanning_state_msg_processor(tpAniSirGlobal pMac,
	void *pMsgBuf)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tSirMbMsg *pMsg = (tSirMbMsg *) pMsgBuf;
	tCsrRoamSession *pSession;
	tSirSmeAssocIndToUpperLayerCnf *pUpperLayerAssocCnf;
	tCsrRoamInfo roamInfo;
	tCsrRoamInfo *pRoamInfo = NULL;
	uint32_t sessionId;

	if (eWNI_SME_SCAN_RSP == pMsg->type)
		return csr_scan_sme_scan_response(pMac, pMsgBuf);

	if (pMsg->type != eWNI_SME_UPPER_LAYER_ASSOC_CNF) {
		if (csr_is_any_session_in_connect_state(pMac)) {
			/*
			 * In case of we are connected, we need to check whether
			 * connect status changes because scan may also run
			 * while connected.
			 */
			csr_roam_check_for_link_status_change(pMac,
						(tSirSmeRsp *) pMsgBuf);
		} else {
			sme_warn("Message [0x%04x] received in wrong state",
				pMsg->type);
		}
		return status;
	}

	sme_debug("Scanning: ASSOC cnf can be given to upper layer");
	qdf_mem_set(&roamInfo, sizeof(tCsrRoamInfo), 0);
	pRoamInfo = &roamInfo;
	pUpperLayerAssocCnf = (tSirSmeAssocIndToUpperLayerCnf *) pMsgBuf;
	status = csr_roam_get_session_id_from_bssid(pMac,
			(struct qdf_mac_addr *)pUpperLayerAssocCnf->bssId,
			&sessionId);
	pSession = CSR_GET_SESSION(pMac, sessionId);

	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}

	/* send the status code as Success */
	pRoamInfo->statusCode = eSIR_SME_SUCCESS;
	pRoamInfo->u.pConnectedProfile = &pSession->connectedProfile;
	pRoamInfo->staId = (uint8_t) pUpperLayerAssocCnf->aid;
	pRoamInfo->rsnIELen = (uint8_t) pUpperLayerAssocCnf->rsnIE.length;
	pRoamInfo->prsnIE = pUpperLayerAssocCnf->rsnIE.rsnIEdata;
	pRoamInfo->addIELen = (uint8_t) pUpperLayerAssocCnf->addIE.length;
	pRoamInfo->paddIE = pUpperLayerAssocCnf->addIE.addIEdata;
	qdf_mem_copy(pRoamInfo->peerMac.bytes,
			pUpperLayerAssocCnf->peerMacAddr,
			QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(&pRoamInfo->bssid.bytes, pUpperLayerAssocCnf->bssId,
		     QDF_MAC_ADDR_SIZE);
	pRoamInfo->wmmEnabledSta = pUpperLayerAssocCnf->wmmEnabledSta;
	if (CSR_IS_INFRA_AP(pRoamInfo->u.pConnectedProfile)) {
		pMac->roam.roamSession[sessionId].connectState =
			eCSR_ASSOC_STATE_TYPE_INFRA_CONNECTED;
		pRoamInfo->fReassocReq = pUpperLayerAssocCnf->reassocReq;
		status = csr_roam_call_callback(pMac, sessionId,
					pRoamInfo, 0,
					eCSR_ROAM_INFRA_IND,
					eCSR_ROAM_RESULT_INFRA_ASSOCIATION_CNF);
	}
	return status;
}

static void csr_check_n_save_wsc_ie(tpAniSirGlobal pMac,
				    tSirBssDescription *pNewBssDescr,
				    tSirBssDescription *pOldBssDescr)
{
	int idx, len;
	uint8_t *pbIe;

	/* If failed to remove, assuming someone else got it. */
	if ((pNewBssDescr->fProbeRsp != pOldBssDescr->fProbeRsp) &&
	    (0 == pNewBssDescr->WscIeLen)) {
		idx = 0;
		len = GET_IE_LEN_IN_BSS(pOldBssDescr->length) -
		      DOT11F_IE_WSCPROBERES_MIN_LEN - 2;
		pbIe = (uint8_t *) pOldBssDescr->ieFields;
		/* Save WPS IE if it exists */
		pNewBssDescr->WscIeLen = 0;
		while (idx < len) {
			if ((DOT11F_EID_WSCPROBERES == pbIe[0]) &&
			    (0x00 == pbIe[2]) && (0x50 == pbIe[3])
			    && (0xf2 == pbIe[4]) && (0x04 == pbIe[5])) {
				/* Founrd it */
				if ((DOT11F_IE_WSCPROBERES_MAX_LEN - 2) >=
				    pbIe[1]) {
					qdf_mem_copy(pNewBssDescr->
						     WscIeProbeRsp, pbIe,
						     pbIe[1] + 2);
					pNewBssDescr->WscIeLen = pbIe[1] + 2;
				}
				break;
			}
			idx += pbIe[1] + 2;
			pbIe += pbIe[1] + 2;
		}
	}
}


/**
 * csr_check_hidden_ssid_entry() - Used for checking if probersp entry for
 * hidden ssid ap has existed or not.
 * @mac_ctx: pointer to Global MAC structure
 * @bssdesc_old:  bss desc has existed in the list
 * @bssdesc_new:  bss desc will be added to the list
 *
 * This function will return true if new desc's ssid is null and the existed
 * bss desc's ssid is not null.
 *
 * Return:
 *        true   - probersp entry has existed
 *        false  - probersp entry doesn't exist
 */
static bool csr_check_hidden_ssid_entry(tpAniSirGlobal mac_ctx,
				      tSirBssDescription *bssdesc_old,
				      tSirBssDescription *bssdesc_new)
{
	bool is_hiddenap_probersp_entry_present = false;
	tDot11fBeaconIEs *p_ies_old = NULL;
	tDot11fBeaconIEs *p_ies_new = NULL;
	QDF_STATUS status;

	status = csr_get_parsed_bss_description_ies(mac_ctx, bssdesc_old,
							&p_ies_old);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto free_ies;

	status = csr_get_parsed_bss_description_ies(mac_ctx, bssdesc_new,
							&p_ies_new);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto free_ies;


	if (p_ies_old->SSID.present && p_ies_new->SSID.present
		&& csr_is_nullssid(
			p_ies_new->SSID.ssid, p_ies_new->SSID.num_ssid)
		&& !csr_is_nullssid(
			p_ies_old->SSID.ssid, p_ies_old->SSID.num_ssid)) {
		/*
		 * This is for hidden ssid. When beacon(null ssid) receives late
		 * than probersp(non-null ssid), it should not duplicate the
		 * non-null ssid scan entry. otherwise, it can't get match AP by
		 * csr_scan_get_result when connection happens.
		 */
		is_hiddenap_probersp_entry_present = true;
	}

free_ies:
	if (p_ies_old)
		qdf_mem_free(p_ies_old);
	if (p_ies_new)
		qdf_mem_free(p_ies_new);
	return is_hiddenap_probersp_entry_present;
}

/* pIes may be NULL */
static bool csr_remove_dup_bss_description(tpAniSirGlobal pMac,
					   tSirBssDescription *bss_dscp,
					   tAniSSID *pSsid,
					   unsigned long *timer)
{
	tListElem *pEntry;
	struct tag_csrscan_result *scan_entry;
	bool fRC = false;

	/*
	 * Walk through all the chained BssDescriptions. If we find a chained
	 * BssDescription that matches the BssID of the BssDescription passed
	 * in, then these must be duplicate scan results for this Bss. In that
	 * case, remove the 'old' Bss description from the linked list.
	 */
	csr_ll_lock(&pMac->scan.scanResultList);
	pEntry = csr_ll_peek_head(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);

	while (pEntry) {
		scan_entry = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		/*
		 * we have a duplicate scan results only when BSSID, SSID,
		 * Channel and NetworkType matches
		 */
		if (csr_is_duplicate_bss_description(pMac,
			&scan_entry->Result.BssDescriptor,
			bss_dscp)) {
			if (bss_dscp->rx_channel == bss_dscp->channelId) {
				/*
				 * Update rssi values only if beacon is
				 * received on the same channel that was
				 * sent on.
				 */
				int32_t rssi_new, rssi_old;
				const int32_t weight =
						CSR_SCAN_RESULT_RSSI_WEIGHT;

				rssi_new = (int32_t) bss_dscp->rssi;
				rssi_old = (int32_t) scan_entry->
						Result.BssDescriptor.rssi;
				rssi_new = ((rssi_new * weight) +
					     rssi_old * (100 - weight)) / 100;
				bss_dscp->rssi = (int8_t) rssi_new;

				rssi_new = (int32_t) bss_dscp->rssi_raw;
				rssi_old = (int32_t) scan_entry->
						Result.BssDescriptor.rssi_raw;
				rssi_new = ((rssi_new * weight) +
					     rssi_old * (100 - weight)) / 100;
				bss_dscp->rssi_raw = (int8_t) rssi_new;
			}

			/* Remove the old entry from the list */
			if (csr_ll_remove_entry
				    (&pMac->scan.scanResultList, pEntry,
				    LL_ACCESS_NOLOCK)) {
				/*
				 * we need to free the memory associated with
				 * this node. If failed to remove, assuming
				 * someone else got it.
				 */
				*pSsid = scan_entry->Result.ssId;
				*timer = scan_entry->Result.timer;
				csr_check_n_save_wsc_ie(pMac, bss_dscp,
							&scan_entry->Result.
							BssDescriptor);
				csr_free_scan_result_entry(pMac, scan_entry);
			} else {
				sme_warn("fail to remove entry");
			}
			fRC = true;
			/*
			 * If we found a match, we can stop looking through
			 * the list.
			 */
			break;
		}
		pEntry = csr_ll_next(&pMac->scan.scanResultList, pEntry,
				     LL_ACCESS_NOLOCK);
	}

	csr_ll_unlock(&pMac->scan.scanResultList);
	return fRC;
}

static QDF_STATUS csr_add_pmkid_candidate_list(tpAniSirGlobal pMac,
					       uint32_t sessionId,
					       tSirBssDescription *pBssDesc,
					       tDot11fBeaconIEs *pIes)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);
	tPmkidCandidateInfo *pmkid_info = NULL;
#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
	WLAN_HOST_DIAG_EVENT_DEF(secEvent,
				 host_event_wlan_security_payload_type);
#endif /* FEATURE_WLAN_DIAG_SUPPORT_CSR */
	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}

	sme_debug("NumPmkidCandidate: %d", pSession->NumPmkidCandidate);
	if (!pIes)
		return status;
		/* check if this is a RSN BSS */
	if (!pIes->RSN.present)
		return status;

	if (pSession->NumPmkidCandidate >= CSR_MAX_PMKID_ALLOWED)
		return QDF_STATUS_E_FAILURE;

	/* BSS is capable of doing pre-authentication */
#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
	qdf_mem_set(&secEvent, sizeof(host_event_wlan_security_payload_type),
		    0);
	secEvent.eventId = WLAN_SECURITY_EVENT_PMKID_CANDIDATE_FOUND;
	secEvent.encryptionModeMulticast = (uint8_t)diag_enc_type_from_csr_type(
				pSession->connectedProfile.mcEncryptionType);
	secEvent.encryptionModeUnicast = (uint8_t)diag_enc_type_from_csr_type(
				pSession->connectedProfile.EncryptionType);
	qdf_mem_copy(secEvent.bssid, pSession->connectedProfile.bssid.bytes,
			QDF_MAC_ADDR_SIZE);
	secEvent.authMode = (uint8_t)diag_auth_type_from_csr_type(
				pSession->connectedProfile.AuthType);
	WLAN_HOST_DIAG_EVENT_REPORT(&secEvent, EVENT_WLAN_SECURITY);
#endif /* #ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR */

	pmkid_info = &pSession->PmkidCandidateInfo[pSession->NumPmkidCandidate];
	/* if yes, then add to PMKIDCandidateList */
	qdf_mem_copy(pmkid_info->BSSID.bytes, pBssDesc->bssId,
			QDF_MAC_ADDR_SIZE);
	/* Bit 0 offirst byte - PreAuthentication Capability */
	if ((pIes->RSN.RSN_Cap[0] >> 0) & 0x1)
		pmkid_info->preAuthSupported = true;
	else
		pmkid_info->preAuthSupported = false;
	pSession->NumPmkidCandidate++;
	return status;
}

/*
 * This function checks whether new AP is found for the current connected
 * profile. If it is found, it return the sessionId, else it return invalid
 * sessionID
 */
static QDF_STATUS csr_process_bss_desc_for_pmkid_list(tpAniSirGlobal pMac,
						tSirBssDescription *pBssDesc,
						tDot11fBeaconIEs *pIes,
						uint8_t sessionId)
{
	tCsrRoamSession *pSession;
	tDot11fBeaconIEs *pIesLocal = pIes;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;

	if (!(pIesLocal ||
	    QDF_IS_STATUS_SUCCESS(
		csr_get_parsed_bss_description_ies(pMac, pBssDesc,
						   &pIesLocal))))
		return status;

	if (!CSR_IS_SESSION_VALID(pMac, sessionId)) {
		if (!pIes)
			qdf_mem_free(pIesLocal);
		return status;
	}

	pSession = CSR_GET_SESSION(pMac, sessionId);
	if (csr_is_conn_state_connected_infra(pMac, sessionId)
	    && (eCSR_AUTH_TYPE_RSN == pSession->connectedProfile.AuthType)
	    && csr_match_bss_to_connect_profile(pMac,
				&pSession->connectedProfile,
				pBssDesc, pIesLocal)) {
		/* This new BSS fits the current profile connected */
		status = csr_add_pmkid_candidate_list(pMac, sessionId,
						      pBssDesc, pIesLocal);
		if (!QDF_IS_STATUS_SUCCESS(status))
			sme_err("csr_add_pmkid_candidate_list failed");
		else
			status = QDF_STATUS_SUCCESS;
	}

	if (!pIes)
		qdf_mem_free(pIesLocal);

	return status;
}

#ifdef FEATURE_WLAN_WAPI
static QDF_STATUS csr_add_bkid_candidate_list(tpAniSirGlobal pMac,
					      uint32_t sessionId,
					      tSirBssDescription *pBssDesc,
					      tDot11fBeaconIEs *pIes)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);

	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}

	sme_debug("csr_add_bkid_candidate_list called pMac->scan.NumBkidCandidate: %d",
		pSession->NumBkidCandidate);
	if (pIes) {
		/* check if this is a WAPI BSS */
		if (pIes->WAPI.present) {
			/* Check if the BSS is capable of doing
			 * pre-authentication
			 */
			if (pSession->NumBkidCandidate < CSR_MAX_BKID_ALLOWED) {

				/* if yes, then add to BKIDCandidateList */
				qdf_mem_copy(pSession->
					     BkidCandidateInfo[pSession->
							      NumBkidCandidate].
					     BSSID.bytes, pBssDesc->bssId,
					     QDF_MAC_ADDR_SIZE);
				if (pIes->WAPI.preauth) {
					pSession->BkidCandidateInfo[pSession->
							NumBkidCandidate].
					preAuthSupported = true;
				} else {
					pSession->BkidCandidateInfo[pSession->
							NumBkidCandidate].
					preAuthSupported = false;
				}
				pSession->NumBkidCandidate++;
			} else
				status = QDF_STATUS_E_FAILURE;
		}
	}

	return status;
}

/*
 * This function checks whether new AP is found for the current connected
 * profile, if so add to BKIDCandidateList
 */
static bool csr_process_bss_desc_for_bkid_list(tpAniSirGlobal pMac,
					       tSirBssDescription *pBssDesc,
					       tDot11fBeaconIEs *pIes)
{
	bool fRC = false;
	tDot11fBeaconIEs *pIesLocal = pIes;
	uint32_t sessionId;
	tCsrRoamSession *pSession;
	QDF_STATUS status;

	if (!(pIesLocal ||
	    QDF_IS_STATUS_SUCCESS(
		csr_get_parsed_bss_description_ies(pMac, pBssDesc,
						   &pIesLocal))))
		return fRC;

	for (sessionId = 0; sessionId < CSR_ROAM_SESSION_MAX; sessionId++) {
		if (!CSR_IS_SESSION_VALID(pMac, sessionId))
			continue;
		pSession = CSR_GET_SESSION(pMac, sessionId);
		if (csr_is_conn_state_connected_infra(pMac, sessionId)
		    && (eCSR_AUTH_TYPE_WAPI_WAI_CERTIFICATE ==
			pSession->connectedProfile.AuthType)
		    && csr_match_bss_to_connect_profile(pMac,
				&pSession->connectedProfile,
				pBssDesc, pIesLocal)) {
			/* this new BSS fits the current profile connected */
			status = csr_add_bkid_candidate_list(pMac, sessionId,
							pBssDesc, pIesLocal);
			if (QDF_IS_STATUS_SUCCESS(status))
				fRC = true;
		}
	}
	if (!pIes)
		qdf_mem_free(pIesLocal);
	return fRC;
}

#endif

/**
 * csr_purge_scan_results() - This function removes scan entry based
 * on RSSI or AGE
 * @mac_ctx: pointer to Global MAC structure
 *
 * This function removes scan entry based on RSSI or AGE.
 * If an scan entry with RSSI less than CSR_PURGE_RSSI_THRESHOLD,
 * the scan entry is removed else oldest entry is removed.
 *
 * Return: None
 */
static void csr_purge_scan_results(tpAniSirGlobal mac_ctx)
{
	tListElem *pentry, *tmp_entry;
	struct tag_csrscan_result *presult, *oldest_bss = NULL,
					*weakest_bss = NULL;
	uint64_t oldest_entry = 0;
	uint64_t curr_time = (uint64_t)qdf_mc_timer_get_system_time();
	int8_t weakest_rssi = 0;

	csr_ll_lock(&mac_ctx->scan.scanResultList);
	pentry = csr_ll_peek_head(&mac_ctx->scan.scanResultList,
					LL_ACCESS_NOLOCK);
	while (pentry) {
		tmp_entry = csr_ll_next(&mac_ctx->scan.scanResultList, pentry,
			LL_ACCESS_NOLOCK);
		presult = GET_BASE_ADDR(pentry, struct tag_csrscan_result,
					Link);
		if ((curr_time - presult->Result.BssDescriptor.received_time) >
		     oldest_entry) {
			oldest_entry = curr_time -
				presult->Result.BssDescriptor.received_time;
			oldest_bss = presult;
		}
		if (presult->Result.BssDescriptor.rssi < weakest_rssi) {
			weakest_rssi = presult->Result.BssDescriptor.rssi;
			weakest_bss = presult;
		}
		pentry = tmp_entry;
	}
	if (oldest_bss) {
		struct tag_csrscan_result *bss_to_remove;

		if (weakest_rssi < CSR_PURGE_RSSI_THRESHOLD)
			bss_to_remove = weakest_bss;
		else
			bss_to_remove = oldest_bss;
		/* Free the old BSS Entries */
		if (csr_ll_remove_entry(&mac_ctx->scan.scanResultList,
		    &bss_to_remove->Link, LL_ACCESS_NOLOCK)) {
			sme_debug("BSSID: "MAC_ADDRESS_STR" Removed, time delta (%llu) RSSI %d",
				MAC_ADDR_ARRAY(
				bss_to_remove->Result.BssDescriptor.bssId),
				(curr_time -
				bss_to_remove->
				Result.BssDescriptor.received_time),
				bss_to_remove->Result.BssDescriptor.rssi);
			csr_free_scan_result_entry(mac_ctx, bss_to_remove);
		}
	}
	csr_ll_unlock(&mac_ctx->scan.scanResultList);
}

static void
csr_remove_from_tmp_list(tpAniSirGlobal mac_ctx,
			 uint8_t reason,
			 uint8_t session_id)
{
	QDF_STATUS status;
	tListElem *entry;
	struct tag_csrscan_result *bss_dscp;
	tDot11fBeaconIEs *local_ie = NULL;
	bool dup_bss;
	tAniSSID tmpSsid;
	unsigned long timer = 0;

	tmpSsid.length = 0;
	while ((entry = csr_ll_remove_tail(&mac_ctx->scan.tempScanResults,
					   LL_ACCESS_LOCK)) != NULL) {
		bss_dscp = GET_BASE_ADDR(entry, struct tag_csrscan_result,
					Link);
		sme_debug(
			"...Bssid= "MAC_ADDRESS_STR" chan= %d, rssi = -%d",
			MAC_ADDR_ARRAY(bss_dscp->Result.BssDescriptor.
				       bssId),
			bss_dscp->Result.BssDescriptor.channelId,
			bss_dscp->Result.BssDescriptor.rssi * (-1));

		/* At this time, bss_dscp->Result.pvIes may be NULL */
		local_ie = (tDot11fBeaconIEs *)(bss_dscp->Result.pvIes);
		if (local_ie == NULL) {
			status = csr_get_parsed_bss_description_ies(mac_ctx,
				&bss_dscp->Result.BssDescriptor, &local_ie);
			if (!local_ie || !QDF_IS_STATUS_SUCCESS(status)) {
				sme_err("Cannot pared IEs");
				csr_free_scan_result_entry(mac_ctx, bss_dscp);
				continue;
			}
		}
		dup_bss = csr_remove_dup_bss_description(mac_ctx,
				&bss_dscp->Result.BssDescriptor,
				&tmpSsid, &timer);
		/*
		 * Check whether we have reach out limit, but don't lose the
		 * LFR candidates came from FW
		 */
		if (CSR_SCAN_IS_OVER_BSS_LIMIT(mac_ctx)) {
			sme_debug("BSS Limit reached");
			csr_purge_scan_results(mac_ctx);
		}
		/* check for duplicate scan results */
		if (!dup_bss) {
			status = csr_process_bss_desc_for_pmkid_list(mac_ctx,
					&bss_dscp->Result.BssDescriptor,
					local_ie, session_id);
			if (QDF_IS_STATUS_SUCCESS(status)) {
				/* Found a new BSS */
				csr_roam_call_callback(mac_ctx, session_id,
					NULL, 0, eCSR_ROAM_SCAN_FOUND_NEW_BSS,
					eCSR_ROAM_RESULT_NONE);
			}
		} else {
			/*
			 * Check if the new one has SSID it it, if not, use
			 * the older SSID if it exists.
			 *
			 * New BSS has a hidden SSID and old one has the SSID.
			 * Keep the SSID only if diff of saved SSID time and
			 * current time is less than 1 min to avoid side effect
			 * of saving SSID with old one is that if AP changes
			 * its SSID while remain hidden, we may never see it
			 * and also to address the requirement of When we remove
			 * hidden ssid from the profile i.e., forget the SSID
			 * via GUI that SSID shouldn't see in the profile
			 */
			uint64_t time_gap =
				(uint64_t)qdf_mc_timer_get_system_time() -
									 timer;
			if ((0 == bss_dscp->Result.ssId.length)
			    && (time_gap <= HIDDEN_TIMER)
			    && tmpSsid.length) {
				bss_dscp->Result.timer = timer;
				bss_dscp->Result.ssId = tmpSsid;
			}
		}

		if (csr_is11d_supported(mac_ctx)
		    && local_ie->Country.present) {
			csr_add_vote_for_country_info(mac_ctx,
						local_ie->Country.country);
			sme_debug(
				"11d AP Bssid "MAC_ADDRESS_STR
				   " chan= %d, rssi = -%d, countryCode %c%c",
				MAC_ADDR_ARRAY(
					bss_dscp->Result.BssDescriptor.bssId),
				bss_dscp->Result.BssDescriptor.channelId,
				bss_dscp->Result.BssDescriptor.rssi * (-1),
				local_ie->Country.country[0],
				local_ie->Country.country[1]);
		}
		/* append to main list */
		csr_scan_add_result(mac_ctx, bss_dscp, local_ie, session_id);
		if ((bss_dscp->Result.pvIes == NULL) && local_ie)
			qdf_mem_free(local_ie);
	} /* end of loop */
}


static bool is_us_country(uint8_t *country_code)
{
	if ((country_code[0] == 'U') &&
	    (country_code[1] == 'S'))
		return true;

	return false;
}

static bool is_world_mode(uint8_t *country_code)
{
	if ((country_code[0] == '0') &&
	    (country_code[1] == '0'))
		return true;

	return false;
}

static bool csr_elected_country_algo_fcc(tpAniSirGlobal mac_ctx)
{
	bool ctry_11d_found = false;
	uint8_t max_votes = 0;
	uint8_t i = 0;
	uint8_t ctry_index;

	if (!mac_ctx->scan.countryCodeCount) {
		sme_warn("No AP with 11d Country code is present in scan list");
		return ctry_11d_found;
	}

	max_votes = mac_ctx->scan.votes11d[0].votes;
	if (is_us_country(mac_ctx->scan.votes11d[0].countryCode)) {
		ctry_11d_found = true;
		ctry_index = 0;
		goto algo_done;
	} else if (max_votes >= MIN_11D_AP_COUNT) {
		ctry_11d_found = true;
		ctry_index = 0;
	}

	for (i = 1; i < mac_ctx->scan.countryCodeCount; i++) {
		if (is_us_country(mac_ctx->scan.votes11d[i].countryCode)) {
			ctry_11d_found = true;
			ctry_index = i;
			goto algo_done;
		}

		if ((max_votes < mac_ctx->scan.votes11d[i].votes) &&
		    (mac_ctx->scan.votes11d[i].votes >= MIN_11D_AP_COUNT)) {
			sme_debug(" Votes for Country %c%c : %d",
				  mac_ctx->scan.votes11d[i].countryCode[0],
				  mac_ctx->scan.votes11d[i].countryCode[1],
				  mac_ctx->scan.votes11d[i].votes);
			max_votes = mac_ctx->scan.votes11d[i].votes;
			ctry_index = i;
			ctry_11d_found = true;
		}
	}

algo_done:

	if (ctry_11d_found) {
		qdf_mem_copy(mac_ctx->scan.countryCodeElected,
			     mac_ctx->scan.votes11d[ctry_index].countryCode,
			     WNI_CFG_COUNTRY_CODE_LEN);

		sme_debug("Selected Country is %c%c With count %d",
			  mac_ctx->scan.votes11d[ctry_index].countryCode[0],
			  mac_ctx->scan.votes11d[ctry_index].countryCode[1],
			  mac_ctx->scan.votes11d[ctry_index].votes);
	}

	return ctry_11d_found;
}


static void csr_move_temp_scan_results_to_main_list(tpAniSirGlobal pMac,
						    uint8_t reason,
						    uint8_t sessionId)
{
	tCsrRoamSession *pSession;
	uint32_t i;
	bool found_11d_ctry = false;

	/* remove the BSS descriptions from temporary list */
	csr_remove_from_tmp_list(pMac, reason, sessionId);
	/*
	 * We don't need to update CC while connected to an AP which is
	 * advertising CC already
	 */
	if (!csr_is11d_supported(pMac))
		return;

	for (i = 0; i < CSR_ROAM_SESSION_MAX; i++) {
		if (!CSR_IS_SESSION_VALID(pMac, i))
			continue;
		pSession = CSR_GET_SESSION(pMac, i);
		if (csr_is_conn_state_connected(pMac, i)) {
			sme_debug("No need to update CC in connected state");
			return;
		}
	}

	if (is_us_country(pMac->scan.countryCodeCurrent) ||
	    is_world_mode(pMac->scan.countryCodeCurrent))
		found_11d_ctry = csr_elected_country_algo_fcc(pMac);
	else
		found_11d_ctry = csr_elected_country_info(pMac);

	if (found_11d_ctry)
		csr_learn_11dcountry_information(pMac, NULL, NULL, true);
}

static struct tag_csrscan_result *csr_scan_save_bss_description(tpAniSirGlobal
							pMac,
						     tSirBssDescription *
						     pBSSDescription,
						     tDot11fBeaconIEs *pIes,
						     uint8_t sessionId)
{
	struct tag_csrscan_result *pCsrBssDescription = NULL;
	uint32_t cbBSSDesc;
	uint32_t cbAllocated;

	/* figure out how big the BSS description is (the BSSDesc->length does
	 * NOT include the size of the length field itself).
	 */
	cbBSSDesc = pBSSDescription->length + sizeof(pBSSDescription->length);

	cbAllocated = sizeof(struct tag_csrscan_result) + cbBSSDesc;

	pCsrBssDescription = qdf_mem_malloc(cbAllocated);
	if (NULL != pCsrBssDescription) {
		pCsrBssDescription->AgingCount =
			(int32_t) pMac->roam.configParam.agingCount;
		sme_debug(
			"Set Aging Count = %d for BSS " MAC_ADDRESS_STR " ",
			pCsrBssDescription->AgingCount,
			MAC_ADDR_ARRAY(pCsrBssDescription->Result.BssDescriptor.
				       bssId));
		qdf_mem_copy(&pCsrBssDescription->Result.BssDescriptor,
			     pBSSDescription, cbBSSDesc);
#if defined(QDF_ENSBALED)
		if (NULL != pCsrBssDescription->Result.pvIes) {
			QDF_ASSERT(pCsrBssDescription->Result.pvIes == NULL);
			return NULL;
		}
#endif
		csr_scan_add_result(pMac, pCsrBssDescription, pIes, sessionId);
	}

	return pCsrBssDescription;
}

/* Append a Bss Description... */
struct tag_csrscan_result *csr_scan_append_bss_description(tpAniSirGlobal pMac,
						tSirBssDescription *
						pSirBssDescription,
						tDot11fBeaconIEs *pIes,
						uint8_t sessionId)
{
	struct tag_csrscan_result *pCsrBssDescription = NULL;
	tAniSSID tmpSsid;
	unsigned long timer = 0;
	int result;

	tmpSsid.length = 0;
	result = csr_remove_dup_bss_description(pMac, pSirBssDescription,
						&tmpSsid, &timer);

	pCsrBssDescription = csr_scan_save_bss_description(pMac,
					pSirBssDescription, pIes, sessionId);
	if (result && (pCsrBssDescription != NULL)) {
		/*
		 * Check if the new one has SSID it it, if not, use the older
		 * SSID if it exists.
		 */
		if ((0 == pCsrBssDescription->Result.ssId.length)
		    && tmpSsid.length) {
			/*
			 * New BSS has a hidden SSID and old one has the SSID.
			 * Keep the SSID only if diff of saved SSID time and
			 * current time is less than 1 min to avoid side effect
			 * of saving SSID with old one is that if AP changes its
			 * SSID while remain hidden, we may never see it and
			 * also to address the requirement of. When we remove
			 * hidden ssid from the profile i.e., forget the SSID
			 * via GUI that SSID shouldn't see in the profile
			 */
			if (((uint64_t)qdf_mc_timer_get_system_time() - timer)
				<= HIDDEN_TIMER) {
				pCsrBssDescription->Result.ssId = tmpSsid;
				pCsrBssDescription->Result.timer = timer;
			}
		}
	}

	return pCsrBssDescription;
}

static void csr_purge_channel_power(tpAniSirGlobal pMac, tDblLinkList
					*pChannelList)
{
	tCsrChannelPowerInfo *pChannelSet;
	tListElem *pEntry;

	csr_ll_lock(pChannelList);
	/*
	 * Remove the channel sets from the learned list and put them
	 * in the free list
	 */
	while ((pEntry = csr_ll_remove_head(pChannelList,
					    LL_ACCESS_NOLOCK)) != NULL) {
		pChannelSet = GET_BASE_ADDR(pEntry, tCsrChannelPowerInfo, link);
		if (pChannelSet)
			qdf_mem_free(pChannelSet);
	}
	csr_ll_unlock(pChannelList);
}

/*
 * Save the channelList into the ultimate storage as the final stage of channel
 * Input: pCountryInfo -- the country code (e.g. "USI"), channel list, and power
 * limit are all stored inside this data structure
 */
QDF_STATUS csr_save_to_channel_power2_g_5_g(tpAniSirGlobal pMac,
					    uint32_t tableSize,
					    tSirMacChanInfo *channelTable)
{
	uint32_t i = tableSize / sizeof(tSirMacChanInfo);
	tSirMacChanInfo *pChannelInfo;
	tCsrChannelPowerInfo *pChannelSet;
	bool f2GHzInfoFound = false;
	bool f2GListPurged = false, f5GListPurged = false;

	pChannelInfo = channelTable;
	/* atleast 3 bytes have to be remaining  -- from "countryString" */
	while (i--) {
		pChannelSet = qdf_mem_malloc(sizeof(tCsrChannelPowerInfo));
		if (NULL == pChannelSet) {
			pChannelInfo++;
			continue;
		}
		pChannelSet->firstChannel = pChannelInfo->firstChanNum;
		pChannelSet->numChannels = pChannelInfo->numChannels;
		/*
		 * Now set the inter-channel offset based on the frequency band
		 * the channel set lies in
		 */
		if ((CDS_IS_CHANNEL_24GHZ(pChannelSet->firstChannel)) &&
		    ((pChannelSet->firstChannel +
		      (pChannelSet->numChannels - 1)) <=
		     CDS_MAX_24GHZ_CHANNEL_NUMBER)) {
			pChannelSet->interChannelOffset = 1;
			f2GHzInfoFound = true;
		} else if ((CDS_IS_CHANNEL_5GHZ(pChannelSet->firstChannel))
		    && ((pChannelSet->firstChannel +
		      ((pChannelSet->numChannels - 1) * 4)) <=
		     CDS_MAX_5GHZ_CHANNEL_NUMBER)) {
			pChannelSet->interChannelOffset = 4;
			f2GHzInfoFound = false;
		} else {
			sme_warn("Invalid Channel %d Present in Country IE",
				pChannelSet->firstChannel);
			qdf_mem_free(pChannelSet);
			return QDF_STATUS_E_FAILURE;
		}
		pChannelSet->txPower = QDF_MIN(pChannelInfo->maxTxPower,
					pMac->roam.configParam.nTxPowerCap);
		if (f2GHzInfoFound) {
			if (!f2GListPurged) {
				/* purge previous results if found new */
				csr_purge_channel_power(pMac,
							&pMac->scan.
							channelPowerInfoList24);
				f2GListPurged = true;
			}
			if (CSR_IS_OPERATING_BG_BAND(pMac)) {
				/* add to the list of 2.4 GHz channel sets */
				csr_ll_insert_tail(&pMac->scan.
						   channelPowerInfoList24,
						   &pChannelSet->link,
						   LL_ACCESS_LOCK);
			} else {
				sme_debug(
					"Adding 11B/G ch in 11A. 1st ch %d",
					pChannelSet->firstChannel);
				qdf_mem_free(pChannelSet);
			}
		} else {
			/* 5GHz info found */
			if (!f5GListPurged) {
				/* purge previous results if found new */
				csr_purge_channel_power(pMac,
							&pMac->scan.
							channelPowerInfoList5G);
				f5GListPurged = true;
			}
			if (CSR_IS_OPERATING_A_BAND(pMac)) {
				/* add to the list of 5GHz channel sets */
				csr_ll_insert_tail(&pMac->scan.
						   channelPowerInfoList5G,
						   &pChannelSet->link,
						   LL_ACCESS_LOCK);
			} else {
				sme_debug(
					"Adding 11A ch in B/G. 1st ch %d",
					pChannelSet->firstChannel);
				qdf_mem_free(pChannelSet);
			}
		}
		pChannelInfo++; /* move to next entry */
	}
	return QDF_STATUS_SUCCESS;
}

static void csr_clear_dfs_channel_list(tpAniSirGlobal pMac)
{
	tSirMbMsg *pMsg;
	uint16_t msgLen;

	msgLen = (uint16_t) (sizeof(tSirMbMsg));
	pMsg = qdf_mem_malloc(msgLen);
	if (NULL != pMsg) {
		pMsg->type = eWNI_SME_CLEAR_DFS_CHANNEL_LIST;
		pMsg->msgLen = msgLen;
		cds_send_mb_message_to_mac(pMsg);
	}
}

void csr_apply_power2_current(tpAniSirGlobal pMac)
{
	sme_debug("Updating Cfg with power settings");
	csr_save_tx_power_to_cfg(pMac, &pMac->scan.channelPowerInfoList24,
				 WNI_CFG_MAX_TX_POWER_2_4);
	csr_save_tx_power_to_cfg(pMac, &pMac->scan.channelPowerInfoList5G,
				 WNI_CFG_MAX_TX_POWER_5);
}

void csr_apply_channel_power_info_to_fw(tpAniSirGlobal mac_ctx,
					  tCsrChannel *ch_lst,
					  uint8_t *countryCode)
{
	int i;
	uint8_t num_ch = 0;
	uint8_t tempNumChannels = 0;
	tCsrChannel tmp_ch_lst;

	if (ch_lst->numChannels) {
		tempNumChannels = CSR_MIN(ch_lst->numChannels,
					  WNI_CFG_VALID_CHANNEL_LIST_LEN);
		for (i = 0; i < tempNumChannels; i++) {
			tmp_ch_lst.channelList[num_ch] = ch_lst->channelList[i];
			num_ch++;
		}
		tmp_ch_lst.numChannels = num_ch;
		/* Store the channel+power info in the global place: Cfg */
		csr_apply_power2_current(mac_ctx);
		csr_set_cfg_valid_channel_list(mac_ctx, tmp_ch_lst.channelList,
					       tmp_ch_lst.numChannels);
		/*
		 * extend scan capability, build a scan list based on the
		 * channel list : channel# + active/passive scan
		 */
		csr_set_cfg_scan_control_list(mac_ctx, countryCode,
					      &tmp_ch_lst);
		/* Send msg to Lim to clear DFS channel list */
		csr_clear_dfs_channel_list(mac_ctx);
	} else {
		sme_err("11D channel list is empty");
	}
	csr_set_cfg_country_code(mac_ctx, countryCode);
}

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
static void csr_diag_reset_country_information(tpAniSirGlobal pMac)
{

	host_log_802_11d_pkt_type *p11dLog;
	int Index;

	WLAN_HOST_DIAG_LOG_ALLOC(p11dLog, host_log_802_11d_pkt_type,
				 LOG_WLAN_80211D_C);
	if (!p11dLog)
		return;

	p11dLog->eventId = WLAN_80211D_EVENT_RESET;
	qdf_mem_copy(p11dLog->countryCode, pMac->scan.countryCodeCurrent, 3);
	p11dLog->numChannel = pMac->scan.base_channels.numChannels;
	if (p11dLog->numChannel <= HOST_LOG_MAX_NUM_CHANNEL) {
		qdf_mem_copy(p11dLog->Channels,
			     pMac->scan.base_channels.channelList,
			     p11dLog->numChannel);
		for (Index = 0;
		     Index < pMac->scan.base_channels.numChannels;
		     Index++) {
			p11dLog->TxPwr[Index] = QDF_MIN(
				pMac->scan.defaultPowerTable[Index].power,
				pMac->roam.configParam.nTxPowerCap);
		}
	}
	if (!pMac->roam.configParam.Is11dSupportEnabled)
		p11dLog->supportMultipleDomain = WLAN_80211D_DISABLED;
	else
		p11dLog->supportMultipleDomain =
			WLAN_80211D_SUPPORT_MULTI_DOMAIN;
	WLAN_HOST_DIAG_LOG_REPORT(p11dLog);
}
#endif /* FEATURE_WLAN_DIAG_SUPPORT_CSR */

/**
 * csr_apply_channel_power_info_wrapper() - sends channel info to fw
 * @pMac: main MAC data structure
 *
 * This function sends the channel power info to firmware
 *
 * Return: none
 */
void csr_apply_channel_power_info_wrapper(tpAniSirGlobal pMac)
{

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
	csr_diag_reset_country_information(pMac);
#endif /* FEATURE_WLAN_DIAG_SUPPORT_CSR */
	csr_prune_channel_list_for_mode(pMac, &pMac->scan.base_channels);
	csr_save_channel_power_for_band(pMac, false);
	csr_save_channel_power_for_band(pMac, true);
	/* apply the channel list, power settings, and the country code. */
	csr_apply_channel_power_info_to_fw(pMac,
		&pMac->scan.base_channels, pMac->scan.countryCodeCurrent);
	/* clear the 11d channel list */
	qdf_mem_set(&pMac->scan.channels11d, sizeof(pMac->scan.channels11d), 0);
}

void csr_clear_votes_for_country_info(tpAniSirGlobal pMac)
{
	pMac->scan.countryCodeCount = 0;
	qdf_mem_set(pMac->scan.votes11d,
		    sizeof(tCsrVotes11d) * CSR_MAX_NUM_COUNTRY_CODE, 0);
}

void csr_add_vote_for_country_info(tpAniSirGlobal pMac, uint8_t *pCountryCode)
{
	bool match = false;
	uint8_t i;

	/* convert to UPPER here so we are assured
	 * the strings are always in upper case.
	 */
	for (i = 0; i < 3; i++)
		pCountryCode[i] = (uint8_t) csr_to_upper(pCountryCode[i]);

	/* Some of the 'old' Cisco 350 series AP's advertise NA as the
	 * country code (for North America ??). NA is not a valid country code
	 * or domain so let's allow this by changing it to the proper
	 * country code (which is US).  We've also seen some NETGEAR AP's
	 * that have "XX " as the country code with valid 2.4 GHz US channel
	 * information.  If we cannot find the country code advertised in the
	 * 11d information element, let's default to US.
	 */

	if (!QDF_IS_STATUS_SUCCESS(csr_get_regulatory_domain_for_country(pMac,
						pCountryCode, NULL,
						SOURCE_QUERY))) {
		pCountryCode[0] = '0';
		pCountryCode[1] = '0';
	}

	/* We've seen some of the AP's improperly put a 0 for the
	 * third character of the country code. spec says valid charcters are
	 * 'O' (for outdoor), 'I' for Indoor, or ' ' (space; for either).
	 * if we see a 0 in this third character, let's change it to a ' '.
	 */
	if (0 == pCountryCode[2])
		pCountryCode[2] = ' ';

	for (i = 0; i < pMac->scan.countryCodeCount; i++) {
		match = (!qdf_mem_cmp(pMac->scan.votes11d[i].countryCode,
					 pCountryCode, 2));
		if (match)
			break;
	}

	if (match)
		pMac->scan.votes11d[i].votes++;
	else {
		qdf_mem_copy(pMac->scan.votes11d[pMac->scan.countryCodeCount].
			     countryCode, pCountryCode, 3);
		pMac->scan.votes11d[pMac->scan.countryCodeCount].votes = 1;
		pMac->scan.countryCodeCount++;
	}
}

bool csr_elected_country_info(tpAniSirGlobal pMac)
{
	bool fRet = false;
	uint8_t maxVotes = 0;
	uint8_t i, j = 0;

	if (!pMac->scan.countryCodeCount) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_WARN,
			"No AP with 11d Country code is present in scan list");
		return fRet;
	}
	maxVotes = pMac->scan.votes11d[0].votes;
	fRet = true;

	for (i = 1; i < pMac->scan.countryCodeCount; i++) {
		/* If we have a tie for max votes for 2 different country codes,
		 * pick random.we can put some more intelligence - TBD
		 */
		if (maxVotes < pMac->scan.votes11d[i].votes) {
			QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
				  " Votes for Country %c%c : %d\n",
				  pMac->scan.votes11d[i].countryCode[0],
				  pMac->scan.votes11d[i].countryCode[1],
				  pMac->scan.votes11d[i].votes);

			maxVotes = pMac->scan.votes11d[i].votes;
			j = i;
			fRet = true;
		}

	}
	if (fRet) {
		qdf_mem_copy(pMac->scan.countryCodeElected,
		       pMac->scan.votes11d[j].countryCode,
		       WNI_CFG_COUNTRY_CODE_LEN);
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
			  "Selected Country is %c%c With count %d\n",
			  pMac->scan.votes11d[j].countryCode[0],
			  pMac->scan.votes11d[j].countryCode[1],
			  pMac->scan.votes11d[j].votes);
	}
	return fRet;
}

/**
 * csr_set_country_code() - Set country code
 * @pMac: main MAC data structure
 * @pCountry: ptr to Country Code
 *
 * This function sends the channel power info to firmware
 *
 * Return: none
 */
QDF_STATUS csr_set_country_code(tpAniSirGlobal pMac, uint8_t *pCountry)
{
	QDF_STATUS status = QDF_STATUS_E_INVAL;
	v_REGDOMAIN_t domainId;

	if (pCountry) {
		status = csr_get_regulatory_domain_for_country(pMac, pCountry,
							     &domainId,
							     SOURCE_USERSPACE);
		if (QDF_IS_STATUS_SUCCESS(status)) {
			qdf_mem_copy(pMac->scan.countryCodeCurrent,
				     pCountry,
				     WNI_CFG_COUNTRY_CODE_LEN);
			csr_set_cfg_country_code(pMac, pCountry);
		}
	}
	return status;
}

/* caller allocated memory for pNumChn and pChnPowerInfo */
/* As input, *pNumChn has the size of the array of pChnPowerInfo */
/* Upon return, *pNumChn has the number of channels assigned. */
static void csr_get_channel_power_info(tpAniSirGlobal pMac, tDblLinkList *list,
				       uint32_t *num_ch,
				       struct channel_power *chn_pwr_info)
{
	tListElem *entry;
	uint32_t chn_idx = 0, idx;
	tCsrChannelPowerInfo *ch_set;

	/* Get 2.4Ghz first */
	entry = csr_ll_peek_head(list, LL_ACCESS_LOCK);
	while (entry && (chn_idx < *num_ch)) {
		ch_set = GET_BASE_ADDR(entry, tCsrChannelPowerInfo, link);
		for (idx = 0; (idx < ch_set->numChannels)
				&& (chn_idx < *num_ch); idx++) {
			chn_pwr_info[chn_idx].chan_num =
				(uint8_t) (ch_set->firstChannel
				 + (idx * ch_set->interChannelOffset));
			chn_pwr_info[chn_idx++].power = ch_set->txPower;
		}
		entry = csr_ll_next(list, entry, LL_ACCESS_LOCK);
	}
	*num_ch = chn_idx;

}

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
static void csr_diag_apply_country_info(tpAniSirGlobal mac_ctx)
{
	host_log_802_11d_pkt_type *p11dLog;
	struct channel_power chnPwrInfo[WNI_CFG_VALID_CHANNEL_LIST_LEN];
	uint32_t nChnInfo = WNI_CFG_VALID_CHANNEL_LIST_LEN, nTmp;

	WLAN_HOST_DIAG_LOG_ALLOC(p11dLog, host_log_802_11d_pkt_type,
				 LOG_WLAN_80211D_C);
	if (!p11dLog)
		return;

	p11dLog->eventId = WLAN_80211D_EVENT_COUNTRY_SET;
	qdf_mem_copy(p11dLog->countryCode, mac_ctx->scan.countryCode11d, 3);
	p11dLog->numChannel = mac_ctx->scan.channels11d.numChannels;
	if (p11dLog->numChannel > HOST_LOG_MAX_NUM_CHANNEL)
		goto diag_end;

	qdf_mem_copy(p11dLog->Channels,
		     mac_ctx->scan.channels11d.channelList,
		     p11dLog->numChannel);
	csr_get_channel_power_info(mac_ctx,
				&mac_ctx->scan.channelPowerInfoList24,
				&nChnInfo, chnPwrInfo);
	nTmp = nChnInfo;
	nChnInfo = WNI_CFG_VALID_CHANNEL_LIST_LEN - nTmp;
	csr_get_channel_power_info(mac_ctx,
				&mac_ctx->scan.channelPowerInfoList5G,
				&nChnInfo, &chnPwrInfo[nTmp]);
	for (nTmp = 0; nTmp < p11dLog->numChannel; nTmp++) {
		for (nChnInfo = 0;
		     nChnInfo < WNI_CFG_VALID_CHANNEL_LIST_LEN;
		     nChnInfo++) {
			if (p11dLog->Channels[nTmp] ==
			    chnPwrInfo[nChnInfo].chan_num) {
				p11dLog->TxPwr[nTmp] =
					chnPwrInfo[nChnInfo].power;
				break;
			}
		}
	}
diag_end:
	if (!mac_ctx->roam.configParam.Is11dSupportEnabled)
		p11dLog->supportMultipleDomain = WLAN_80211D_DISABLED;
	else
		p11dLog->supportMultipleDomain =
				WLAN_80211D_SUPPORT_MULTI_DOMAIN;
	WLAN_HOST_DIAG_LOG_REPORT(p11dLog);
}
#endif /* #ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR */

/**
 * csr_apply_country_information() - apply country code information
 * @pMac: core MAC data structure
 *
 * This function programs the new country code
 *
 * Return: none
 */
void csr_apply_country_information(tpAniSirGlobal pMac)
{
	v_REGDOMAIN_t domainId;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!csr_is11d_supported(pMac)
	    || 0 == pMac->scan.channelOf11dInfo)
		return;
	status = csr_get_regulatory_domain_for_country(pMac,
			pMac->scan.countryCode11d, &domainId, SOURCE_QUERY);
	if (!QDF_IS_STATUS_SUCCESS(status))
		return;
	/* Check whether we need to enforce default domain */
#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
	csr_diag_apply_country_info(pMac);
#endif /* #ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR */

	if (pMac->scan.domainIdCurrent != domainId)
		return;
	if (pMac->scan.domainIdCurrent != domainId) {
		sme_debug("Domain Changed Old %d, new %d",
			pMac->scan.domainIdCurrent, domainId);
		status = wma_set_reg_domain(pMac, domainId);
	}
	if (status != QDF_STATUS_SUCCESS)
		sme_err("fail to set regId %d", domainId);
	pMac->scan.domainIdCurrent = domainId;
	/* switch to active scans using this new channel list */
	pMac->scan.curScanType = eSIR_ACTIVE_SCAN;
}

void csr_save_channel_power_for_band(tpAniSirGlobal pMac, bool fill_5f)
{
	uint32_t idx, count = 0;
	tSirMacChanInfo *chan_info;
	tSirMacChanInfo *ch_info_start;
	int32_t max_ch_idx;
	bool tmp_bool;
	uint8_t ch = 0;

	max_ch_idx =
		(pMac->scan.base_channels.numChannels <
		WNI_CFG_VALID_CHANNEL_LIST_LEN) ?
		pMac->scan.base_channels.numChannels :
		WNI_CFG_VALID_CHANNEL_LIST_LEN;

	chan_info = qdf_mem_malloc(sizeof(tSirMacChanInfo) *
				   WNI_CFG_VALID_CHANNEL_LIST_LEN);
	if (NULL == chan_info)
		return;

	ch_info_start = chan_info;
	for (idx = 0; idx < max_ch_idx; idx++) {
		ch = pMac->scan.defaultPowerTable[idx].chan_num;
		tmp_bool =  (fill_5f && CDS_IS_CHANNEL_5GHZ(ch))
			|| (!fill_5f && CDS_IS_CHANNEL_24GHZ(ch));
		if (!tmp_bool)
			continue;

		if (count >= WNI_CFG_VALID_CHANNEL_LIST_LEN) {
			sme_warn("count: %d exceeded", count);
			break;
		}

		chan_info->firstChanNum =
			pMac->scan.defaultPowerTable[idx].chan_num;
		chan_info->numChannels = 1;
		chan_info->maxTxPower =
			QDF_MIN(pMac->scan.defaultPowerTable[idx].power,
				pMac->roam.configParam.nTxPowerCap);
		chan_info++;
		count++;
	}
	if (count) {
		csr_save_to_channel_power2_g_5_g(pMac,
				count * sizeof(tSirMacChanInfo), ch_info_start);
	}
	qdf_mem_free(ch_info_start);
}

bool csr_is_supported_channel(tpAniSirGlobal pMac, uint8_t channelId)
{
	bool fRet = false;
	uint32_t i;

	for (i = 0; i < pMac->scan.base_channels.numChannels; i++) {
		if (channelId ==
		    pMac->scan.base_channels.channelList[i]) {
			fRet = true;
			break;
		}
	}

	return fRet;
}

/*
 * 802.11D only: Gather 11d IE via beacon or Probe response and store them in
 * pAdapter->channels11d
 */
bool csr_learn_11dcountry_information(tpAniSirGlobal pMac,
				   tSirBssDescription *pSirBssDesc,
				   tDot11fBeaconIEs *pIes, bool fForce)
{
	QDF_STATUS status;
	uint8_t *pCountryCodeSelected;
	bool fRet = false;
	v_REGDOMAIN_t domainId;
	tDot11fBeaconIEs *pIesLocal = pIes;
	bool useVoting = false;

	if ((NULL == pSirBssDesc) && (NULL == pIes))
		useVoting = true;

	/* check if .11d support is enabled */
	if (!csr_is11d_supported(pMac))
		goto free_ie;

	if (false == useVoting) {
		if (!pIesLocal &&
		   (!QDF_IS_STATUS_SUCCESS(
			csr_get_parsed_bss_description_ies(
				pMac, pSirBssDesc, &pIesLocal))))
			goto free_ie;
		/* check if country information element is present */
		if (!pIesLocal->Country.present)
			/* No country info */
			goto free_ie;
		status = csr_get_regulatory_domain_for_country(pMac,
				pIesLocal->Country.country, &domainId,
				SOURCE_QUERY);
		if (QDF_IS_STATUS_SUCCESS(status)
		    && (domainId == REGDOMAIN_WORLD))
			goto free_ie;
	} /* useVoting == false */

	if (false == useVoting)
		pCountryCodeSelected = pIesLocal->Country.country;
	else
		pCountryCodeSelected = pMac->scan.countryCodeElected;

	if (qdf_mem_cmp(pCountryCodeSelected, pMac->scan.countryCodeCurrent,
			CDS_COUNTRY_CODE_LEN) == 0) {
		qdf_mem_copy(pMac->scan.countryCode11d,
			     pMac->scan.countryCodeCurrent,
			     CDS_COUNTRY_CODE_LEN);
		goto free_ie;
	}

	pMac->reg_hint_src = SOURCE_11D;
	status = csr_get_regulatory_domain_for_country(pMac,
				pCountryCodeSelected, &domainId, SOURCE_11D);
	if (status != QDF_STATUS_SUCCESS) {
		sme_err("fail to get regId %d", domainId);
		fRet = false;
		goto free_ie;
	}

	fRet = true;
free_ie:
	if (!pIes && pIesLocal) {
		/* locally allocated */
		qdf_mem_free(pIesLocal);
	}
	return fRet;
}

void csr_save_scan_results(tpAniSirGlobal pMac, uint8_t reason,
				  uint8_t sessionId)
{
	sme_debug("Saving scan results");

	/* initialize this to false. profMoveInterimScanResultsToMainList()
	 * routine will set this to the channel where an .11d beacon is seen
	 */
	pMac->scan.channelOf11dInfo = 0;
	/* move the scan results from interim list to the main scan list */
	csr_move_temp_scan_results_to_main_list(pMac, reason, sessionId);

	/* Now check if we gathered any domain/country specific information */
	/* If so, we should update channel list and apply Tx power settings */
	if (csr_is11d_supported(pMac))
		csr_apply_country_information(pMac);
}

void csr_reinit_scan_cmd(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
	switch (pCommand->u.scanCmd.reason) {
	case eCsrScanAbortNormalScan:
	default:
		csr_scan_free_request(pMac, &pCommand->u.scanCmd.u.scanRequest);
		break;
	}
	if (pCommand->u.scanCmd.pToRoamProfile) {
		csr_release_profile(pMac, pCommand->u.scanCmd.pToRoamProfile);
		qdf_mem_free(pCommand->u.scanCmd.pToRoamProfile);
	}
	qdf_mem_set(&pCommand->u.scanCmd, sizeof(tScanCmd), 0);
}

static enum csr_scancomplete_nextcommand csr_scan_get_next_command_state(
							tpAniSirGlobal pMac,
							tSmeCmd *pCommand,
							bool fSuccess,
							uint8_t *chan)
{
	enum csr_scancomplete_nextcommand NextCommand = eCsrNextScanNothing;
	int8_t channel;

	switch (pCommand->u.scanCmd.reason) {
	case eCsrScan11d1:
		NextCommand =
			(fSuccess) ? eCsrNext11dScan1Success :
			eCsrNext11dScan1Failure;
		break;
	case eCsrScan11d2:
		NextCommand =
			(fSuccess) ? eCsrNext11dScan2Success :
			eCsrNext11dScan2Failure;
		break;
	case eCsrScan11dDone:
		NextCommand = eCsrNext11dScanComplete;
		break;
	case eCsrScanLostLink1:
		NextCommand =
			(fSuccess) ? eCsrNextLostLinkScan1Success :
			eCsrNextLostLinkScan1Failed;
		break;
	case eCsrScanLostLink2:
		NextCommand =
			(fSuccess) ? eCsrNextLostLinkScan2Success :
			eCsrNextLostLinkScan2Failed;
		break;
	case eCsrScanLostLink3:
		NextCommand =
			(fSuccess) ? eCsrNextLostLinkScan3Success :
			eCsrNextLostLinkScan3Failed;
		break;
	case eCsrScanForSsid:
		/* success:
		 *   set hw_mode success -> csr_scan_handle_search_for_ssid
		 *   set hw_mode fail -> csr_scan_handle_search_for_ssid_failure
		 * failure: csr_scan_handle_search_for_ssid_failure
		 */
		sme_debug("Resp for eCsrScanForSsid");
		channel = cds_search_and_check_for_session_conc(
				pCommand->sessionId,
				pCommand->u.scanCmd.pToRoamProfile);
		if ((!channel) || !fSuccess) {
			NextCommand = eCsrNexteScanForSsidFailure;
			sme_debug("next ScanForSsidFailure %d %d",
				channel, fSuccess);
		} else {
			NextCommand = eCsrNextCheckAllowConc;
			*chan = channel;
			sme_debug("next CheckAllowConc");
		}
		break;
	default:
		NextCommand = eCsrNextScanNothing;
		break;
	}
	return NextCommand;
}

/* Return whether the pCommand is finished. */
static bool csr_handle_scan11d1_failure(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
	bool fRet = true;

	/* Apply back the default setting and passively scan one more time. */
	csr_apply_channel_power_info_wrapper(pMac);
	pCommand->u.scanCmd.reason = eCsrScan11d2;
	if (QDF_IS_STATUS_SUCCESS(csr_scan_channels(pMac, pCommand)))
		fRet = false;

	return fRet;
}

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
static void
csr_diag_scan_complete(tpAniSirGlobal pMac,
		       tSmeCmd *pCommand,
		       tSirSmeScanRsp *pScanRsp)
{
	host_log_scan_pkt_type *pScanLog = NULL;
	tScanResultHandle hScanResult;
	tCsrScanResultInfo *pScanResult;
	tDot11fBeaconIEs *pIes;
	int n = 0, c = 0;
	QDF_STATUS status;

	WLAN_HOST_DIAG_LOG_ALLOC(pScanLog,
				 host_log_scan_pkt_type,
				 LOG_WLAN_SCAN_C);
	if (!pScanLog)
		return;

	if (eCsrScanProbeBss == pCommand->u.scanCmd.reason) {
		pScanLog->eventId = WLAN_SCAN_EVENT_HO_SCAN_RSP;
	} else {
		if (eSIR_PASSIVE_SCAN != pMac->scan.curScanType)
			pScanLog->eventId = WLAN_SCAN_EVENT_ACTIVE_SCAN_RSP;
		else
			pScanLog->eventId = WLAN_SCAN_EVENT_PASSIVE_SCAN_RSP;
	}
	if (eSIR_SME_SUCCESS != pScanRsp->statusCode) {
		pScanLog->status = WLAN_SCAN_STATUS_FAILURE;
		WLAN_HOST_DIAG_LOG_REPORT(pScanLog);
		return;
	}

	status = csr_scan_get_result(pMac, NULL, &hScanResult);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		WLAN_HOST_DIAG_LOG_REPORT(pScanLog);
		return;
	}

	pScanResult = csr_scan_result_get_next(pMac, hScanResult);
	while (pScanResult != NULL) {
		if (n < HOST_LOG_MAX_NUM_BSSID) {
			status = csr_get_parsed_bss_description_ies(pMac,
					&pScanResult->BssDescriptor, &pIes);
			if (!QDF_IS_STATUS_SUCCESS(status)) {
				sme_err("fail to parse IEs");
				break;
			}
			qdf_mem_copy(pScanLog->bssid[n],
				pScanResult->BssDescriptor.bssId, 6);
			if (pIes && pIes->SSID.present &&
			    HOST_LOG_MAX_SSID_SIZE >= pIes->SSID.num_ssid) {
				qdf_mem_copy(pScanLog->ssid[n],
					pIes->SSID.ssid,
					pIes->SSID.num_ssid);
			}
			qdf_mem_free(pIes);
			n++;
		}
		c++;
		pScanResult = csr_scan_result_get_next(pMac, hScanResult);
	}
	pScanLog->numSsid = (uint8_t) n;
	pScanLog->totalSsid = (uint8_t) c;
	csr_scan_result_purge(pMac, hScanResult);
	WLAN_HOST_DIAG_LOG_REPORT(pScanLog);

}
#endif /* #ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR */

/**
 * csr_saved_scan_cmd_free_fields() - Free internal fields of scan command
 *
 * @mac_ctx: Global MAC context
 * @saved_scan_cmd: Pointer to scan command
 *
 * Frees data structures allocated inside saved_scan_cmd and releases
 * the profile.
 * Return: None
 */

static void csr_saved_scan_cmd_free_fields(tpAniSirGlobal mac_ctx,
					   tSmeCmd *saved_scan_cmd)
{
	if (saved_scan_cmd->u.scanCmd.pToRoamProfile) {
		csr_release_profile(mac_ctx,
				    saved_scan_cmd->u.scanCmd.pToRoamProfile);
		qdf_mem_free(saved_scan_cmd->u.scanCmd.pToRoamProfile);
		saved_scan_cmd->u.scanCmd.pToRoamProfile = NULL;
	}
	if (saved_scan_cmd->u.scanCmd.u.scanRequest.SSIDs.SSIDList) {
		qdf_mem_free(saved_scan_cmd->u.scanCmd.u.
			     scanRequest.SSIDs.SSIDList);
		saved_scan_cmd->u.scanCmd.u.scanRequest.SSIDs.SSIDList = NULL;
	}
	if (saved_scan_cmd->u.roamCmd.pRoamBssEntry) {
		qdf_mem_free(saved_scan_cmd->u.roamCmd.pRoamBssEntry);
		saved_scan_cmd->u.roamCmd.pRoamBssEntry = NULL;
	}
}

/**
 * csr_save_profile() - Save the profile info from sme command
 * @mac_ctx: Global MAC context
 * @save_cmd: Pointer where the command will be saved
 * @command: Command from which the profile will be saved
 *
 * Saves the profile information from the SME's scan command
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS csr_save_profile(tpAniSirGlobal mac_ctx,
				   tSmeCmd *save_cmd, tSmeCmd *command)
{
	struct tag_csrscan_result *scan_result;
	struct tag_csrscan_result *temp;
	uint32_t bss_len;
	QDF_STATUS status;

	save_cmd->u.scanCmd.pToRoamProfile =
		qdf_mem_malloc(sizeof(tCsrRoamProfile));
	if (!save_cmd->u.scanCmd.pToRoamProfile) {
		sme_err("pToRoamProfile mem fail");
		goto error;
	}

	status = csr_roam_copy_profile(mac_ctx,
			save_cmd->u.scanCmd.pToRoamProfile,
			command->u.scanCmd.pToRoamProfile);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_err("csr_roam_copy_profile fail");
		goto error;
	}

	save_cmd->sessionId = command->sessionId;
	save_cmd->u.scanCmd.roamId = command->u.scanCmd.roamId;
	save_cmd->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs =
		command->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs;
	save_cmd->u.scanCmd.u.scanRequest.SSIDs.SSIDList =
		qdf_mem_malloc(
			save_cmd->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs *
			sizeof(tCsrSSIDInfo));
	if (!save_cmd->u.scanCmd.u.scanRequest.SSIDs.SSIDList) {
		sme_err("SSIDList mem fail");
		goto error;
	}

	qdf_mem_copy(save_cmd->u.scanCmd.u.scanRequest.SSIDs.SSIDList,
			command->u.scanCmd.u.scanRequest.SSIDs.SSIDList,
			save_cmd->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs *
			sizeof(tCsrSSIDInfo));

	if (!command->u.roamCmd.pRoamBssEntry)
		return QDF_STATUS_SUCCESS;

	scan_result = GET_BASE_ADDR(command->u.roamCmd.pRoamBssEntry,
			struct tag_csrscan_result, Link);

	bss_len = scan_result->Result.BssDescriptor.length +
		sizeof(scan_result->Result.BssDescriptor.length);

	temp = qdf_mem_malloc(sizeof(struct tag_csrscan_result) + bss_len);
	if (!temp) {
		sme_err("bss mem fail");
		goto error;
	}

	temp->AgingCount = scan_result->AgingCount;
	temp->preferValue = scan_result->preferValue;
	temp->capValue = scan_result->capValue;
	temp->ucEncryptionType = scan_result->ucEncryptionType;
	temp->mcEncryptionType = scan_result->mcEncryptionType;
	temp->authType = scan_result->authType;
	/* pvIes is unsued in success/failure */
	temp->Result.pvIes = NULL;

	qdf_mem_copy(temp->Result.pvIes,
			scan_result->Result.pvIes,
			sizeof(*scan_result->Result.pvIes));
	temp->Result.ssId.length = scan_result->Result.ssId.length;
	qdf_mem_copy(temp->Result.ssId.ssId,
			scan_result->Result.ssId.ssId,
			sizeof(scan_result->Result.ssId.ssId));
	temp->Result.timer = scan_result->Result.timer;
	qdf_mem_copy(&temp->Result.BssDescriptor,
			&scan_result->Result.BssDescriptor,
			sizeof(temp->Result.BssDescriptor));
	temp->Link.last = temp->Link.next = NULL;
	save_cmd->u.roamCmd.pRoamBssEntry = (tListElem *)temp;

	return QDF_STATUS_SUCCESS;
error:
	csr_scan_handle_search_for_ssid_failure(mac_ctx,
			command);
	csr_saved_scan_cmd_free_fields(mac_ctx, save_cmd);

	return QDF_STATUS_E_FAILURE;
}

static void
csr_handle_nxt_cmd(tpAniSirGlobal mac_ctx, tSmeCmd *pCommand,
		   enum csr_scancomplete_nextcommand *nxt_cmd,
		   bool *remove_cmd, uint32_t session_id,
		   uint8_t chan)
{
	QDF_STATUS status, ret;
	tSmeCmd *save_cmd = NULL;
	tSmeCmd *saved_scan_cmd;

	switch (*nxt_cmd) {
	case eCsrNext11dScan1Success:
	case eCsrNext11dScan2Success:
		sme_debug(
			"11dScan1/3 produced results. Reissue Active scan");
		/*
		 * if we found country information, no need to continue scanning
		 * further, bail out
		 */
		*remove_cmd = true;
		*nxt_cmd = eCsrNext11dScanComplete;
		break;
	case eCsrNext11dScan1Failure:
		/*
		 * We are not done yet. 11d scan fail once. We will try to reset
		 * anything and do it over again. The only meaningful thing for
		 * this retry is that we cannot find 11d information after a
		 * reset so we clear the "old" 11d info and give it once more
		 * chance
		 */
		*remove_cmd = csr_handle_scan11d1_failure(mac_ctx, pCommand);
		if (*remove_cmd)
			*nxt_cmd = eCsrNext11dScanComplete;
		break;
	case eCsrNextLostLinkScan1Success:
		status = csr_issue_roam_after_lostlink_scan(mac_ctx, session_id,
							    eCsrLostLink1);
		if (!QDF_IS_STATUS_SUCCESS(status))
			csr_scan_handle_failed_lostlink1(mac_ctx, session_id);
		break;
	case eCsrNextLostLinkScan2Success:
		status = csr_issue_roam_after_lostlink_scan(mac_ctx, session_id,
							    eCsrLostLink2);
		if (!QDF_IS_STATUS_SUCCESS(status))
			csr_scan_handle_failed_lostlink2(mac_ctx, session_id);
		break;
	case eCsrNextLostLinkScan3Success:
		status = csr_issue_roam_after_lostlink_scan(mac_ctx, session_id,
							    eCsrLostLink3);
		if (!QDF_IS_STATUS_SUCCESS(status))
			csr_scan_handle_failed_lostlink3(mac_ctx, session_id);
		break;
	case eCsrNextLostLinkScan1Failed:
		csr_scan_handle_failed_lostlink1(mac_ctx, session_id);
		break;
	case eCsrNextLostLinkScan2Failed:
		csr_scan_handle_failed_lostlink2(mac_ctx, session_id);
		break;
	case eCsrNextLostLinkScan3Failed:
		csr_scan_handle_failed_lostlink3(mac_ctx, session_id);
		break;
	case eCsrNexteScanForSsidSuccess:
		csr_scan_handle_search_for_ssid(mac_ctx, pCommand);
		break;
	case eCsrNexteScanForSsidFailure:
		csr_scan_handle_search_for_ssid_failure(mac_ctx, pCommand);
		break;
	case eCsrNextCheckAllowConc:
		ret = cds_current_connections_update(pCommand->sessionId,
					chan,
					SIR_UPDATE_REASON_HIDDEN_STA);
		sme_debug("chan: %d session: %d status: %d",
					chan, pCommand->sessionId, ret);
		saved_scan_cmd = (tSmeCmd *)mac_ctx->sme.saved_scan_cmd;
		if (saved_scan_cmd) {
			csr_saved_scan_cmd_free_fields(mac_ctx,
						       saved_scan_cmd);
			qdf_mem_free(saved_scan_cmd);
			saved_scan_cmd = NULL;
		}

		save_cmd = (tSmeCmd *) qdf_mem_malloc(sizeof(*pCommand));
		if (!save_cmd) {
			sme_err("save_cmd mem fail");
			goto error;
		}

		status = csr_save_profile(mac_ctx, save_cmd, pCommand);
		if (!QDF_IS_STATUS_SUCCESS(status)) {
			sme_err("profile save failed %d", status);
			qdf_mem_free(save_cmd);
			return;
		}

		mac_ctx->sme.saved_scan_cmd = (void *)save_cmd;

		if (QDF_STATUS_E_FAILURE == ret) {
error:
			sme_err("conn update fail %d", chan);
			csr_scan_handle_search_for_ssid_failure(mac_ctx,
								pCommand);
			if (mac_ctx->sme.saved_scan_cmd) {
				csr_saved_scan_cmd_free_fields(mac_ctx,
						mac_ctx->sme.saved_scan_cmd);
				qdf_mem_free(mac_ctx->sme.saved_scan_cmd);
				mac_ctx->sme.saved_scan_cmd = NULL;
			}
		} else if ((QDF_STATUS_E_NOSUPPORT == ret) ||
			(QDF_STATUS_E_ALREADY == ret)) {
			sme_err("conn update ret %d", ret);
			csr_scan_handle_search_for_ssid(mac_ctx, pCommand);
			if (mac_ctx->sme.saved_scan_cmd) {
				csr_saved_scan_cmd_free_fields(mac_ctx,
						mac_ctx->sme.saved_scan_cmd);
				qdf_mem_free(mac_ctx->sme.saved_scan_cmd);
				mac_ctx->sme.saved_scan_cmd = NULL;
			}
		}
		/* Else: Set hw mode was issued and the saved connect would
		 * be issued after set hw mode response
		 */
		break;
	default:
		break;
	}
}

/**
 * csr_get_active_scan_entry() - To get scan entry from active command list
 *
 * @mac_ctx - MAC context
 * @scan_id - Scan identifier of the scan request
 * @entry - scan entry returned.
 *
 * Scan entry in the active scan list mapping to the sent scan id
 * is returned to the caller.
 *
 * Return: QDF_STATUS.
 */
QDF_STATUS csr_get_active_scan_entry(tpAniSirGlobal mac_ctx,
	uint32_t scan_id, tListElem **entry)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	tListElem *localentry;
	tSmeCmd *cmd;
	uint32_t cmd_scan_id = 0;

	csr_ll_lock(&mac_ctx->sme.smeScanCmdActiveList);

	if (csr_ll_is_list_empty(&mac_ctx->sme.smeScanCmdActiveList,
			LL_ACCESS_NOLOCK)) {
		sme_err("Active list Empty scanId: %d", scan_id);
		csr_ll_unlock(&mac_ctx->sme.smeScanCmdActiveList);
		return QDF_STATUS_SUCCESS;
	}
	localentry = csr_ll_peek_head(&mac_ctx->sme.smeScanCmdActiveList,
			LL_ACCESS_NOLOCK);
	while (localentry) {
		cmd = GET_BASE_ADDR(localentry, tSmeCmd, Link);
		if (cmd->command == eSmeCommandScan)
			cmd_scan_id = cmd->u.scanCmd.u.scanRequest.scan_id;
		else if (cmd->command == eSmeCommandRemainOnChannel)
			cmd_scan_id = cmd->u.remainChlCmd.scan_id;
		if (cmd_scan_id == scan_id) {
			sme_debug("scanId Matched %d", scan_id);
			*entry = localentry;
			csr_ll_unlock(&mac_ctx->sme.smeScanCmdActiveList);
			return QDF_STATUS_SUCCESS;
		}
		localentry = csr_ll_next(&mac_ctx->sme.smeScanCmdActiveList,
				localentry, LL_ACCESS_NOLOCK);
	}
	csr_ll_unlock(&mac_ctx->sme.smeScanCmdActiveList);
	return status;
}

/* Return whether the command should be removed */
bool csr_scan_complete(tpAniSirGlobal pMac, tSirSmeScanRsp *pScanRsp)
{
	enum csr_scancomplete_nextcommand NextCommand = eCsrNextScanNothing;
	tListElem *pEntry = NULL;
	tSmeCmd *pCommand;
	bool fRemoveCommand = true;
	bool fSuccess;
	uint32_t sessionId = 0;
	uint8_t chan;

	csr_get_active_scan_entry(pMac, pScanRsp->scan_id, &pEntry);
	if (!pEntry) {
		sme_err("Scan Completion called but NO cmd ACTIVE");
		return false;
	}

	pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
	/*
	 * If the head of the queue is Active and it is a SCAN command, remove
	 * and put this on the Free queue.
	 */
	if (eSmeCommandScan != pCommand->command) {
		sme_warn("Scan Completion called, but active SCAN cmd");
		return false;
	}

	sessionId = pCommand->sessionId;
	if (eSIR_SME_SUCCESS != pScanRsp->statusCode) {
		fSuccess = false;
	} else {
		/*
		 * pMac->scan.tempScanResults is not empty meaning the scan
		 * found something. This check only valid here because
		 * csrSaveScanresults is not yet called
		 */
		fSuccess = (!csr_ll_is_list_empty(&pMac->scan.tempScanResults,
						  LL_ACCESS_LOCK));
	}
	if (pCommand->u.scanCmd.abort_scan_indication &
	    eCSR_SCAN_ABORT_DUE_TO_BAND_CHANGE) {
		/*
		 * Scan aborted due to band change
		 * The scan results need to be flushed
		 */
		if (pCommand->u.scanCmd.callback
		    != pMac->scan.callback11dScanDone) {
			sme_debug("Filtering the scan results");
			csr_scan_filter_results(pMac);
		} else {
			sme_debug("11d_scan_done, flushing the scan results");
		}
	}
	csr_save_scan_results(pMac, pCommand->u.scanCmd.reason, sessionId);

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
	csr_diag_scan_complete(pMac, pCommand, pScanRsp);
#endif /* #ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR */
	NextCommand = csr_scan_get_next_command_state(pMac, pCommand, fSuccess,
							&chan);
	/* We reuse the command here instead reissue a new command */
	csr_handle_nxt_cmd(pMac, pCommand, &NextCommand,
			   &fRemoveCommand, sessionId, chan);
	return fRemoveCommand;
}

/**
 * csr_scan_remove_dup_bss_description_from_interim_list() - To remove duplicate
 * entry in interim scan list
 *
 * @mac_ctx - MAC context
 * @bss_dscp - new bss_dscp will be added to list
 * @flags - flag to indicate if rssi update is needed
 * @is_hiddenap_probersp_entry_present - value return to indicate if probersp
 * entry for hidden ssid ap has existed
 *
 * To check and remove if duplicate entry existed in interim list.
 *
 * Return: none.
 */
static void
csr_scan_remove_dup_bss_description_from_interim_list(tpAniSirGlobal mac_ctx,
				tSirBssDescription *bss_dscp,
				uint32_t flags,
				bool *is_hiddenap_probersp_entry_present)
{
	tListElem *pEntry;
	struct tag_csrscan_result *scan_bss_dscp;
	int8_t scan_entry_rssi = 0;
	int8_t scan_entry_rssi_raw = 0;
	/*
	 * Walk through all the chained BssDescriptions. If we find a chained
	 * BssDescription that matches the BssID of the BssDescription passed
	 * in, then these must be duplicate scan results for this Bss. In that
	 * case, remove the 'old' Bss description from the linked list.
	 */
	sme_debug("for BSS " MAC_ADDRESS_STR " ",
		MAC_ADDR_ARRAY(bss_dscp->bssId));
	csr_ll_lock(&mac_ctx->scan.tempScanResults);
	pEntry = csr_ll_peek_head(&mac_ctx->scan.tempScanResults,
				 LL_ACCESS_NOLOCK);
	while (pEntry) {
		scan_bss_dscp = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		/*
		 * we have a duplicate scan results only when BSSID, SSID,
		 * Channel and NetworkType matches
		 */
		scan_entry_rssi = scan_bss_dscp->Result.BssDescriptor.rssi;
		scan_entry_rssi_raw =
				scan_bss_dscp->Result.BssDescriptor.rssi_raw;

		if (csr_is_duplicate_bss_description(mac_ctx,
			&scan_bss_dscp->Result.BssDescriptor, bss_dscp)) {

			/* Do not update RSSI id skip RSSI Update flag is set */
			if (flags & WLAN_SKIP_RSSI_UPDATE) {
				bss_dscp->rssi =
				   scan_bss_dscp->Result.BssDescriptor.rssi;
				bss_dscp->rssi_raw =
				   scan_bss_dscp->Result.BssDescriptor.rssi_raw;
			} else {
				/*
				 * Following is mathematically
				 * a = (aX + b(100-X))/100 where:
				 * a = bss_dscp->rssi, b = scan_entry_rssi
				 * and X = CSR_SCAN_RESULT_RSSI_WEIGHT
				 */
				bss_dscp->rssi = (int8_t)
					((((int32_t) bss_dscp->rssi *
					CSR_SCAN_RESULT_RSSI_WEIGHT) +
					((int32_t) scan_entry_rssi *
					(100 - CSR_SCAN_RESULT_RSSI_WEIGHT)))
					/ 100);
				bss_dscp->rssi_raw = (int8_t)
					((((int32_t) bss_dscp->rssi_raw *
					CSR_SCAN_RESULT_RSSI_WEIGHT) +
					((int32_t) scan_entry_rssi_raw *
					(100 - CSR_SCAN_RESULT_RSSI_WEIGHT)))
					/ 100);
			}

			*is_hiddenap_probersp_entry_present =
				csr_check_hidden_ssid_entry(mac_ctx,
					&scan_bss_dscp->Result.BssDescriptor,
					bss_dscp);
			if (*is_hiddenap_probersp_entry_present) {
				/*
				 * In the temp list we have a probe resp and we
				 * don't want to overwrite this with beacon in
				 * case of hidden AP scenario.
				 * but rssi must be updated.
				 */
				scan_bss_dscp->Result.BssDescriptor.rssi =
							bss_dscp->rssi;
				scan_bss_dscp->Result.BssDescriptor.rssi_raw =
							bss_dscp->rssi_raw;
				break;
			}

			/* Remove the 'old' entry from the list */
			if (csr_ll_remove_entry(&mac_ctx->scan.tempScanResults,
				pEntry, LL_ACCESS_NOLOCK)) {
				csr_check_n_save_wsc_ie(mac_ctx, bss_dscp,
							&scan_bss_dscp->Result.
							BssDescriptor);
				/*
				 * we need to free the memory associated with
				 * this node
				 */
				csr_free_scan_result_entry(mac_ctx,
							   scan_bss_dscp);
			}
			/*
			 * If we found a match, we can stop looking through
			 * the list.
			 */
			break;
		}
		pEntry = csr_ll_next(&mac_ctx->scan.tempScanResults, pEntry,
				     LL_ACCESS_NOLOCK);
	}

	csr_ll_unlock(&mac_ctx->scan.tempScanResults);
}

/* Caller allocated memory pfNewBssForConn to return whether
 * new candidate for current connection is found. Cannot be NULL
 */
static struct tag_csrscan_result *csr_scan_save_bss_description_to_interim_list(
					tpAniSirGlobal pMac,
					tSirBssDescription *pBSSDescription,
					tDot11fBeaconIEs *pIes)
{
	struct tag_csrscan_result *pCsrBssDescription = NULL;
	uint32_t cbBSSDesc;
	uint32_t cbAllocated;

	/* figure out how big the BSS description is (the BSSDesc->length does
	 * NOT include the size of the length field itself).
	 */
	cbBSSDesc = pBSSDescription->length + sizeof(pBSSDescription->length);

	cbAllocated = sizeof(struct tag_csrscan_result) + cbBSSDesc;

	sme_debug("new BSS description, length %d, cbBSSDesc %d",
		cbAllocated, cbBSSDesc);
	pCsrBssDescription = qdf_mem_malloc(cbAllocated);
	if (NULL != pCsrBssDescription) {
		qdf_mem_copy(&pCsrBssDescription->Result.BssDescriptor,
			     pBSSDescription, cbBSSDesc);
		pCsrBssDescription->AgingCount =
			(int32_t) pMac->roam.configParam.agingCount;
		sme_debug(
			"Set Aging Count = %d for BSS " MAC_ADDRESS_STR " ",
			pCsrBssDescription->AgingCount,
			MAC_ADDR_ARRAY(pCsrBssDescription->Result.BssDescriptor.
				       bssId));
		/* Save SSID separately for later use */
		if (pIes->SSID.present
		    && !csr_is_nullssid(pIes->SSID.ssid, pIes->SSID.num_ssid)) {
			/* SSID not hidden */
			uint32_t len = pIes->SSID.num_ssid;

			if (len > SIR_MAC_MAX_SSID_LENGTH) {
				/* truncate to fit in our struct */
				len = SIR_MAC_MAX_SSID_LENGTH;
			}
			pCsrBssDescription->Result.ssId.length = len;
			pCsrBssDescription->Result.timer =
				qdf_mc_timer_get_system_time();
			qdf_mem_copy(pCsrBssDescription->Result.ssId.ssId,
				     pIes->SSID.ssid, len);
		}
		csr_ll_insert_tail(&pMac->scan.tempScanResults,
				   &pCsrBssDescription->Link, LL_ACCESS_LOCK);
	}

	return pCsrBssDescription;
}

bool csr_is_duplicate_bss_description(tpAniSirGlobal pMac,
				      tSirBssDescription *pSirBssDesc1,
				      tSirBssDescription *pSirBssDesc2)
{
	bool fMatch = false;
	tSirMacCapabilityInfo *pCap1, *pCap2;

	pCap1 = (tSirMacCapabilityInfo *) &pSirBssDesc1->capabilityInfo;
	pCap2 = (tSirMacCapabilityInfo *) &pSirBssDesc2->capabilityInfo;

	if (pCap1->ess != pCap2->ess) {
		if (qdf_is_macaddr_equal(
			(struct qdf_mac_addr *) pSirBssDesc1->bssId,
			(struct qdf_mac_addr *) pSirBssDesc2->bssId))
			sme_err("ess mismatch for same BSSID "MAC_ADDRESS_STR"",
				MAC_ADDR_ARRAY(pSirBssDesc1->bssId));
		return fMatch;
	}

	if (pCap1->ess &&
	    qdf_is_macaddr_equal((struct qdf_mac_addr *) pSirBssDesc1->bssId,
				 (struct qdf_mac_addr *) pSirBssDesc2->bssId)) {
		fMatch = true;
	}
	/* In case of P2P devices, ess and ibss will be set to zero */
	else if (!pCap1->ess &&
		qdf_is_macaddr_equal(
			(struct qdf_mac_addr *) pSirBssDesc1->bssId,
			(struct qdf_mac_addr *) pSirBssDesc2->bssId)) {
		fMatch = true;
	}

	return fMatch;
}

bool csr_is_network_type_equal(tSirBssDescription *pSirBssDesc1,
			       tSirBssDescription *pSirBssDesc2)
{
	return pSirBssDesc1->nwType == pSirBssDesc2->nwType;
}

/* to check whether the BSS matches the dot11Mode */
static bool csr_scan_is_bss_allowed(tpAniSirGlobal pMac,
				    tSirBssDescription *pBssDesc,
				    tDot11fBeaconIEs *pIes)
{
	bool fAllowed = false;
	eCsrPhyMode phyMode;

	if (QDF_IS_STATUS_SUCCESS(csr_get_phy_mode_from_bss(pMac, pBssDesc,
				&phyMode, pIes))) {
		switch (pMac->roam.configParam.phyMode) {
		case eCSR_DOT11_MODE_11b:
			fAllowed = (bool) (eCSR_DOT11_MODE_11a != phyMode);
			break;
		case eCSR_DOT11_MODE_11g:
			fAllowed = (bool) (eCSR_DOT11_MODE_11a != phyMode);
			break;
		case eCSR_DOT11_MODE_11g_ONLY:
			fAllowed = (bool) (eCSR_DOT11_MODE_11g == phyMode);
			break;
		case eCSR_DOT11_MODE_11a:
			fAllowed = (bool) ((eCSR_DOT11_MODE_11b != phyMode)
					   && (eCSR_DOT11_MODE_11g != phyMode));
			break;
		case eCSR_DOT11_MODE_11n_ONLY:
			fAllowed = (bool) ((eCSR_DOT11_MODE_11n == phyMode));
			break;

		case eCSR_DOT11_MODE_11ac_ONLY:
			fAllowed = (bool) ((eCSR_DOT11_MODE_11ac == phyMode));
			break;
		case eCSR_DOT11_MODE_11b_ONLY:
			fAllowed = (bool) (eCSR_DOT11_MODE_11b == phyMode);
			break;
		case eCSR_DOT11_MODE_11n:
		case eCSR_DOT11_MODE_11ac:
		default:
			fAllowed = true;
			break;
		}
	}

	return fAllowed;
}

/* Return pIes to caller for future use when returning true. */
static bool csr_scan_validate_scan_result(tpAniSirGlobal pMac,
					  uint8_t *pChannels,
					  uint8_t numChn,
					  tSirBssDescription *pBssDesc,
					  tDot11fBeaconIEs **ppIes)
{
	bool valid = false;
	tDot11fBeaconIEs *pIes = NULL;
	uint8_t index;
	QDF_STATUS status;

	for (index = 0; index < numChn; index++) {
		/*
		 * This check relies on the fact that a single BSS description
		 * is returned in each ScanRsp call, which is the way LIM
		 * implemented the scan req/rsp funtions. We changed to this
		 * model when we ran with a large number of APs. If this were to
		 * change, then this check would have to mess with removing the
		 * bssDescription from somewhere in an arbitrary index in the
		 * bssDescription array.
		 */
		if (pChannels[index] == pBssDesc->channelId) {
			valid = true;
			break;
		}
	}
	*ppIes = NULL;
	if (valid) {
		status = csr_get_parsed_bss_description_ies(pMac, pBssDesc,
							    &pIes);
		if (!QDF_IS_STATUS_SUCCESS(status))
			return false;

		valid = csr_scan_is_bss_allowed(pMac, pBssDesc, pIes);
		if (valid)
			*ppIes = pIes;
		else
			qdf_mem_free(pIes);
	}
	return valid;
}

static void csr_update_scantype(tpAniSirGlobal pMac, tDot11fBeaconIEs *pIes,
				uint8_t channelId)
{
	if (eSIR_PASSIVE_SCAN != pMac->scan.curScanType)
		return;

	if (csr_is11d_supported(pMac)) {
		/* Check whether the BSS is acceptable based on
		 * 11d info and our config.
		 */
		if (!csr_match_country_code(pMac, NULL, pIes))
			return;

		/* check if channel is acceptable by config */
		if (csr_is_supported_channel(pMac, channelId))
			pMac->scan.curScanType = eSIR_ACTIVE_SCAN;

	} else
		pMac->scan.curScanType = eSIR_ACTIVE_SCAN;

}

/* Return whether last scan result is received */
static bool csr_scan_process_scan_results(tpAniSirGlobal pMac, tSmeCmd
					*pCommand, tSirSmeScanRsp *pScanRsp,
					  bool *pfRemoveCommand)
{
	bool fRet = false, fRemoveCommand = false;
	QDF_STATUS status;

	sme_debug("scan reason = %d, response status code = %d",
		pCommand->u.scanCmd.reason, pScanRsp->statusCode);
	fRemoveCommand = csr_scan_complete(pMac, pScanRsp);
	fRet = true;
	if (pfRemoveCommand)
		*pfRemoveCommand = fRemoveCommand;

	/*
	 * Currently SET_FCC_CHANNEL issues updated channel list to fw.
	 * At the time of driver load, if scan is issued followed with
	 * SET_FCC_CHANNEL, driver will send update channel list to fw.
	 * Fw will stop ongoing scan because of that GUI will have very less
	 * scan list.
	 * Update channel list should be sent to fw once scan is done
	 */
	if (pMac->scan.defer_update_channel_list) {
		status = csr_update_channel_list(pMac);
		if (!QDF_IS_STATUS_SUCCESS(status))
			sme_err("failed to update the supported channel list");
		pMac->scan.defer_update_channel_list = false;
	}

	return fRet;
}

/**
 * csr_scan_get_session_id() - Get session id
 * @mac_ctx: Pointer to global mac
 *
 * This function returns a valid session id
 *
 * Return: session id
 */
static uint8_t csr_scan_get_session_id(tpAniSirGlobal mac_ctx)
{
	uint8_t i;

	for (i = 0; i < CSR_ROAM_SESSION_MAX; i++)
		if (CSR_IS_SESSION_VALID(mac_ctx, i))
			return i;

	return CSR_SESSION_ID_INVALID;
}

/* csr_scan_process_single_bssdescr() - Add a bssdescriptor to scan table
 *
 * @mac_ctx - MAC context
 * @bssdescr - Pointer to BSS description structure that contains
 *             everything from beacon/probe response frame and additional
 *             information.
 * @scan_id - Scan identifier of the scan request that was running
 *            when this beacon was received. Reserved for future when
 *            firmware provides that information.
 * @flags - Reserved for future use.
 *
 * Callback routine called by LIM when it receives a beacon or probe response
 * from the device. 802.11 frame is already converted to internal
 * tSirBssDescription data structure.
 *
 * Return: 0 or other error codes.
 */

QDF_STATUS csr_scan_process_single_bssdescr(tpAniSirGlobal mac_ctx,
					tSirBssDescription *bssdescr,
					uint32_t scan_id, uint32_t flags)
{
	tDot11fBeaconIEs *ies = NULL;
	uint8_t *chanlist = NULL;
	uint8_t cnt_channels = 0;
	uint32_t len = sizeof(mac_ctx->roam.validChannelList);
	tCsrRoamInfo *roam_info;
	uint8_t session_id;
	bool is_hiddenap_probersp_entry_present = false;

	session_id = csr_scan_get_session_id(mac_ctx);
	sme_debug("CSR: Processing single bssdescr");
	if (QDF_IS_STATUS_SUCCESS(
		csr_get_cfg_valid_channels(mac_ctx,
			(uint8_t *) mac_ctx->roam.validChannelList,
			&len))) {
		chanlist = mac_ctx->roam.validChannelList;
		cnt_channels = (uint8_t) len;
	} else {
		sme_warn("Received results on invalid channel");
		return QDF_STATUS_E_INVAL;
	}

	if (csr_scan_validate_scan_result(mac_ctx, chanlist,
			cnt_channels, bssdescr, &ies)) {

		csr_scan_remove_dup_bss_description_from_interim_list
			(mac_ctx, bssdescr, flags,
			&is_hiddenap_probersp_entry_present);

		if (is_hiddenap_probersp_entry_present)
			goto exit;

		csr_scan_save_bss_description_to_interim_list
			(mac_ctx, bssdescr, ies);
		roam_info = qdf_mem_malloc(sizeof(*roam_info));
		if (roam_info) {
			qdf_mem_zero(roam_info, sizeof(*roam_info));
			roam_info->pBssDesc = bssdescr;

			if (session_id == CSR_SESSION_ID_INVALID) {
				if (ies != NULL)
					qdf_mem_free(ies);
				qdf_mem_free(roam_info);
				return QDF_STATUS_E_INVAL;

			}
			csr_roam_call_callback(mac_ctx, session_id, roam_info,
					       0, eCSR_ROAM_UPDATE_SCAN_RESULT,
					       eCSR_ROAM_RESULT_NONE);
			qdf_mem_free(roam_info);
		} else {
			sme_err("qdf_mem_malloc failed");
		}
		/*
		 * If scan is not in progress and interim list
		 * reach threashold, move scan results from temp
		 * to main list and age out old results.
		 */
		if (csr_ll_is_list_empty(&mac_ctx->sme.smeScanCmdActiveList,
		   LL_ACCESS_LOCK) &&
		   csr_ll_count(&mac_ctx->scan.tempScanResults) >=
				CSR_MAX_BSS_SUPPORT / 10) {
			csr_remove_from_tmp_list(mac_ctx, eCsrScanOther,
				session_id);
			/* Purge the scan results based on Aging */
			if (mac_ctx->scan.scanResultCfgAgingTime)
				csr_purge_scan_result_by_age(mac_ctx);
		}
		csr_update_scantype(mac_ctx, ies, bssdescr->channelId);
	}

exit:
	/* Free the resource */
	if (ies != NULL)
		qdf_mem_free(ies);
	return QDF_STATUS_SUCCESS;
}


static bool csr_scan_is_wild_card_scan(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
	uint8_t bssid[QDF_MAC_ADDR_SIZE] = {0};
	/*
	 * It is not a wild card scan if the bssid is not broadcast and
	 * the number of SSID is 1.
	 */
	return ((!qdf_mem_cmp(pCommand->u.scanCmd.u.scanRequest.bssid.bytes,
					bssid, sizeof(struct qdf_mac_addr)))
		|| (0xff == pCommand->u.scanCmd.u.scanRequest.bssid.bytes[0]))
		&& (pCommand->u.scanCmd.u.scanRequest.SSIDs.numOfSSIDs != 1);
}

QDF_STATUS csr_scan_sme_scan_response(tpAniSirGlobal pMac,
		void *pMsgBuf)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tListElem *pEntry = NULL;
	tSmeCmd *pCommand;
	eCsrScanStatus scanStatus;
	tSirSmeScanRsp *pScanRsp = (tSirSmeScanRsp *)pMsgBuf;
	bool fRemoveCommand = true;
	eCsrScanReason reason = eCsrScanOther;

	csr_get_active_scan_entry(pMac, pScanRsp->scan_id, &pEntry);
	if (!pEntry)
		goto error_handling;

	sme_debug("Scan completion called:scan_id %d, entry = %pK",
		pScanRsp->scan_id, pEntry);

	pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
	if (eSmeCommandScan != pCommand->command)
		goto error_handling;

	scanStatus = (eSIR_SME_SUCCESS == pScanRsp->statusCode) ?
			eCSR_SCAN_SUCCESS : eCSR_SCAN_FAILURE;
	reason = pCommand->u.scanCmd.reason;
	switch (pCommand->u.scanCmd.reason) {
	case eCsrScanAbortNormalScan:
		break;
	case eCsrScanP2PFindPeer:
		scanStatus = (eSIR_SME_SUCCESS == pScanRsp->statusCode) ?
				eCSR_SCAN_FOUND_PEER : eCSR_SCAN_FAILURE;
		csr_scan_process_scan_results(pMac, pCommand, pScanRsp, NULL);
		break;
	default:
		if (csr_scan_process_scan_results(pMac, pCommand, pScanRsp,
						  &fRemoveCommand)
		    && csr_scan_is_wild_card_scan(pMac, pCommand)
		    && !pCommand->u.scanCmd.u.scanRequest.p2pSearch) {

		/* Age out logic will be taken care by the age out timer */
		}
		break;
	}
	/* Purge the scan results based on Aging */
	if (pMac->scan.scanResultCfgAgingTime)
		csr_purge_scan_result_by_age(pMac);
	if (fRemoveCommand)
		csr_release_scan_command(pMac, pCommand, scanStatus);
	sme_process_pending_queue(pMac);
	return status;

error_handling:
#ifdef FEATURE_WLAN_SCAN_PNO
	if (pMac->pnoOffload && pScanRsp->statusCode == eSIR_PNO_SCAN_SUCCESS) {
		sme_debug("PNO Scan completion called");
		csr_save_scan_results(pMac, eCsrScanCandidateFound,
				      pScanRsp->sessionId);
		if (pMac->scan.scanResultCfgAgingTime)
			csr_purge_scan_result_by_age(pMac);
		return QDF_STATUS_SUCCESS;
	}

#endif
	sme_err("Scan completion called, but no active SCAN command");
	return QDF_STATUS_E_FAILURE;
}

tCsrScanResultInfo *csr_scan_result_get_first(tpAniSirGlobal pMac,
					      tScanResultHandle hScanResult)
{
	tListElem *pEntry;
	struct tag_csrscan_result *pResult;
	tCsrScanResultInfo *pRet = NULL;
	struct scan_result_list *pResultList =
				(struct scan_result_list *) hScanResult;

	if (pResultList) {
		csr_ll_lock(&pResultList->List);
		pEntry = csr_ll_peek_head(&pResultList->List, LL_ACCESS_NOLOCK);
		if (pEntry) {
			pResult = GET_BASE_ADDR(pEntry, struct
						tag_csrscan_result, Link);
			pRet = &pResult->Result;
		}
		pResultList->pCurEntry = pEntry;
		csr_ll_unlock(&pResultList->List);
	}

	return pRet;
}

tCsrScanResultInfo *csr_scan_result_get_next(tpAniSirGlobal pMac,
					     tScanResultHandle hScanResult)
{
	tListElem *pEntry = NULL;
	struct tag_csrscan_result *pResult = NULL;
	tCsrScanResultInfo *pRet = NULL;
	struct scan_result_list *pResultList =
				(struct scan_result_list *) hScanResult;

	if (!pResultList)
		return NULL;

	csr_ll_lock(&pResultList->List);
	if (NULL == pResultList->pCurEntry)
		pEntry = csr_ll_peek_head(&pResultList->List, LL_ACCESS_NOLOCK);
	else
		pEntry = csr_ll_next(&pResultList->List, pResultList->pCurEntry,
				     LL_ACCESS_NOLOCK);

	if (pEntry) {
		pResult = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		pRet = &pResult->Result;
	}
	pResultList->pCurEntry = pEntry;
	csr_ll_unlock(&pResultList->List);
	return pRet;
}

/*
 * This function moves the first BSS that matches the bssid to the
 * head of the result
 */
QDF_STATUS csr_move_bss_to_head_from_bssid(tpAniSirGlobal pMac,
					   struct qdf_mac_addr *bssid,
					   tScanResultHandle hScanResult)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	struct scan_result_list *pResultList =
				(struct scan_result_list *) hScanResult;
	struct tag_csrscan_result *pResult = NULL;
	tListElem *pEntry = NULL;

	if (!(pResultList && bssid))
		return status;

	csr_ll_lock(&pResultList->List);
	pEntry = csr_ll_peek_head(&pResultList->List, LL_ACCESS_NOLOCK);
	while (pEntry) {
		pResult = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		if (!qdf_mem_cmp(bssid, pResult->Result.BssDescriptor.bssId,
				    sizeof(struct qdf_mac_addr))) {
			status = QDF_STATUS_SUCCESS;
			csr_ll_remove_entry(&pResultList->List, pEntry,
					    LL_ACCESS_NOLOCK);
			csr_ll_insert_head(&pResultList->List, pEntry,
					   LL_ACCESS_NOLOCK);
			break;
		}
		pEntry = csr_ll_next(&pResultList->List, pResultList->pCurEntry,
				     LL_ACCESS_NOLOCK);
	}
	csr_ll_unlock(&pResultList->List);
	return status;
}

/* Remove the BSS if possible. */
/* Return -- true == the BSS is remove. False == Fail to remove it */
/* This function is called when list lock is held. */
static bool csr_scan_age_out_bss(tpAniSirGlobal pMac, struct tag_csrscan_result
				*pResult)
{
	bool fRet = false;
	uint32_t i;
	tCsrRoamSession *pSession;
	bool isConnBssfound = false;

	for (i = 0; i < CSR_ROAM_SESSION_MAX; i++) {
		if (!CSR_IS_SESSION_VALID(pMac, i))
			continue;
		pSession = CSR_GET_SESSION(pMac, i);
		/* Not to remove the BSS we are connected to. */
		if (csr_is_conn_state_connected_infra(pMac, i)
		    && (NULL != pSession->pConnectBssDesc)
		    && (csr_is_duplicate_bss_description(pMac,
			&pResult->Result.BssDescriptor,
			pSession->pConnectBssDesc))) {
			isConnBssfound = true;
			break;
		}
	}
	if (isConnBssfound) {
		/*
		 * Reset the counter so that aging out of connected BSS won't
		 * hapeen too soon
		 */
		pResult->AgingCount =
			(int32_t) pMac->roam.configParam.agingCount;
		sme_debug(
			"Connected BSS, Set Aging Count=%d for BSS "
			   MAC_ADDRESS_STR, pResult->AgingCount,
			MAC_ADDR_ARRAY(pResult->Result.BssDescriptor.bssId));
		pResult->Result.BssDescriptor.received_time =
			(uint64_t)qdf_mc_timer_get_system_time();
		return fRet;
	}
	/*
	 * No need to hold the spin lock because caller should hold the lock for
	 * pMac->scan.scanResultList
	 */
	if (csr_ll_remove_entry(&pMac->scan.scanResultList, &pResult->Link,
				LL_ACCESS_NOLOCK)) {
		if (qdf_is_macaddr_equal((struct qdf_mac_addr *) &pResult->
				Result.BssDescriptor.bssId,
				(struct qdf_mac_addr *)
				&pMac->scan.currentCountryBssid)) {
			sme_warn("Aging out 11d BSS " MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(
					pResult->Result.BssDescriptor.bssId));
			pMac->scan.currentCountryRSSI = -128;
		}
		csr_free_scan_result_entry(pMac, pResult);
		fRet = true;
	}
	return fRet;
}

QDF_STATUS csr_scan_age_results(tpAniSirGlobal pMac,
				tSmeGetScanChnRsp *pScanChnInfo)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tListElem *pEntry, *tmpEntry;
	struct tag_csrscan_result *pResult;
	tLimScanChn *pChnInfo;
	uint8_t i;

	csr_ll_lock(&pMac->scan.scanResultList);
	for (i = 0; i < pScanChnInfo->numChn; i++) {
		pChnInfo = &pScanChnInfo->scanChn[i];
		pEntry = csr_ll_peek_head(&pMac->scan.scanResultList,
						LL_ACCESS_NOLOCK);
		while (pEntry) {
			tmpEntry =
				csr_ll_next(&pMac->scan.scanResultList, pEntry,
					    LL_ACCESS_NOLOCK);
			pResult = GET_BASE_ADDR(pEntry, struct
						tag_csrscan_result, Link);
			if (pResult->Result.BssDescriptor.channelId ==
			    pChnInfo->channelId) {
				if (pResult->AgingCount <= 0) {
					sme_warn("age out due to ref count");
					csr_scan_age_out_bss(pMac, pResult);
				} else {
					pResult->AgingCount--;
					sme_warn("Decremented AgingCount=%d for BSS "
						MAC_ADDRESS_STR "",
						pResult->AgingCount,
						MAC_ADDR_ARRAY(pResult->Result.
							       BssDescriptor.
							       bssId));
				}
			}
			pEntry = tmpEntry;
		}
	}
	csr_ll_unlock(&pMac->scan.scanResultList);

	return status;
}

/**
 * csr_populate_ie_whitelist_attrs() - Populates ie whitelist attrs
 * @msg: pointer to sme scan req
 * @scan_req: pointer to csr scan req
 *
 * This function populates ie whitelisting attributes of the sme scan req
 * with corresponding whitelisting attrs in csr scan req
 *
 * Return: None
 */
static void csr_populate_ie_whitelist_attrs(tSirSmeScanReq *msg,
					    tCsrScanRequest *scan_req)
{
	msg->ie_whitelist = scan_req->ie_whitelist;
	msg->num_vendor_oui = scan_req->num_vendor_oui;

	if (!msg->ie_whitelist)
		return;

	qdf_mem_copy(msg->probe_req_ie_bitmap, scan_req->probe_req_ie_bitmap,
		     PROBE_REQ_BITMAP_LEN * sizeof(uint32_t));
	msg->oui_field_len = scan_req->num_vendor_oui * sizeof(*scan_req->voui);
	msg->oui_field_offset = (sizeof(tSirSmeScanReq) -
				 sizeof(msg->channelList.channelNumber) +
				 (sizeof(msg->channelList.channelNumber) *
				 scan_req->ChannelInfo.numOfChannels)) +
				 scan_req->uIEFieldLen;

	if (scan_req->num_vendor_oui != 0)
		qdf_mem_copy((uint8_t *)msg + msg->oui_field_offset,
			     (uint8_t *)(scan_req->voui), msg->oui_field_len);
}

static QDF_STATUS csr_send_mb_scan_req(tpAniSirGlobal pMac, uint16_t sessionId,
				       tCsrScanRequest *pScanReq,
				       struct tag_scanreq_param *pScanReqParam)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tSirSmeScanReq *pMsg;
	uint16_t msgLen;
	tSirScanType scanType = pScanReq->scanType;
	uint32_t minChnTime;    /* in units of milliseconds */
	uint32_t maxChnTime;    /* in units of milliseconds */
	uint32_t i;
	struct qdf_mac_addr selfmac;

	msgLen = (uint16_t) (sizeof(tSirSmeScanReq) -
		 sizeof(pMsg->channelList.channelNumber) +
		 (sizeof(pMsg->channelList.channelNumber) *
		 pScanReq->ChannelInfo.numOfChannels)) +
		 (pScanReq->uIEFieldLen) +
		 pScanReq->num_vendor_oui * sizeof(*pScanReq->voui);

	pMsg = qdf_mem_malloc(msgLen);
	if (NULL == pMsg) {
		sme_err("memory allocation failed");
		sme_debug("Failed: SId: %d FirstMatch: %d UniqueResult: %d freshScan: %d hiddenSsid: %d",
			sessionId, pScanReqParam->bReturnAfter1stMatch,
			pScanReqParam->fUniqueResult, pScanReqParam->freshScan,
			pScanReqParam->hiddenSsid);
		sme_debug("scanType: %s (%u) BSSType: %s (%u) numOfSSIDs: %d numOfChannels: %d requestType: %s (%d) p2pSearch: %d",
			sme_scan_type_to_string(pScanReq->scanType),
			pScanReq->scanType,
			sme_bss_type_to_string(pScanReq->BSSType),
			pScanReq->BSSType,
			pScanReq->SSIDs.numOfSSIDs,
			pScanReq->ChannelInfo.numOfChannels,
			sme_request_type_to_string(pScanReq->requestType),
			pScanReq->requestType, pScanReq->p2pSearch);
		return QDF_STATUS_E_NOMEM;
	}

	pMsg->messageType = eWNI_SME_SCAN_REQ;
	pMsg->length = msgLen;
	/* ToDO: Fill in session info when we need to do scan base on session */
	if (sessionId != CSR_SESSION_ID_INVALID)
		pMsg->sessionId = sessionId;
	else
		/* if sessionId == CSR_SESSION_ID_INVALID, then send the scan
		 * request on first available session
		 */
		pMsg->sessionId = 0;

	if (pMsg->sessionId >= CSR_ROAM_SESSION_MAX)
		sme_err("Invalid Sme Session ID: %d",
			pMsg->sessionId);
	pMsg->transactionId = 0;
	pMsg->dot11mode = (uint8_t) csr_translate_to_wni_cfg_dot11_mode(pMac,
				csr_find_best_phy_mode(pMac,
					pMac->roam.configParam.phyMode));
	pMsg->bssType = csr_translate_bsstype_to_mac_type(pScanReq->BSSType);

	if (CSR_IS_SESSION_VALID(pMac, sessionId)) {
		qdf_copy_macaddr(&selfmac,
			&pMac->roam.roamSession[sessionId].selfMacAddr);
	} else {
		/*
		 * Since we don't have session for the scanning, we find a valid
		 * session. In case we fail to do so, get the WNI_CFG_STA_ID
		 */
		for (i = 0; i < CSR_ROAM_SESSION_MAX; i++) {
			if (CSR_IS_SESSION_VALID(pMac, i)) {
				qdf_copy_macaddr(&selfmac,
					&pMac->roam.roamSession[i].selfMacAddr);
				break;
			}
		}
		if (CSR_ROAM_SESSION_MAX == i) {
			uint32_t len = QDF_MAC_ADDR_SIZE;
			tSirRetStatus sir_status;

			sir_status = wlan_cfg_get_str(pMac, WNI_CFG_STA_ID,
						  selfmac.bytes, &len);
			if ((sir_status != eSIR_SUCCESS)
			    || (len < QDF_MAC_ADDR_SIZE)) {
				sme_err("Can't get self MAC: status:%d len:%d",
					sir_status, len);
				status = QDF_STATUS_E_FAILURE;
				goto send_scan_req;
			}
		}
	}
	qdf_copy_macaddr(&pMsg->selfMacAddr, &selfmac);

	qdf_copy_macaddr(&pMsg->bssId, &pScanReq->bssid);
	if (qdf_is_macaddr_zero(&pScanReq->bssid))
		qdf_set_macaddr_broadcast(&pMsg->bssId);
	else
		qdf_copy_macaddr(&pMsg->bssId, &pScanReq->bssid);
	minChnTime = pScanReq->minChnTime;
	maxChnTime = pScanReq->maxChnTime;

	/*
	 * Verify the scan type first, if the scan is active scan, we need to
	 * make sure we are allowed to do so. if 11d is enabled & we don't see
	 * any beacon around, scan type falls back to passive. But in BT AMP STA
	 * mode we need to send out a directed probe
	 */
	if ((eSIR_PASSIVE_SCAN != scanType)
	    && (eCSR_SCAN_P2P_DISCOVERY != pScanReq->requestType)
	    && (false == pMac->scan.fEnableBypass11d)) {
		scanType = pMac->scan.curScanType;
		if (eSIR_PASSIVE_SCAN == pMac->scan.curScanType) {
			if (minChnTime <
			    pMac->roam.configParam.nPassiveMinChnTime) {
				minChnTime =
				    pMac->roam.configParam.nPassiveMinChnTime;
			}
			if (maxChnTime <
			    pMac->roam.configParam.nPassiveMaxChnTime) {
				maxChnTime =
				    pMac->roam.configParam.nPassiveMaxChnTime;
			}
		}
	}
	pMsg->scanType = scanType;
	pMsg->scan_adaptive_dwell_mode = pScanReq->scan_adaptive_dwell_mode;

	pMsg->numSsid = (pScanReq->SSIDs.numOfSSIDs < SIR_SCAN_MAX_NUM_SSID) ?
			 pScanReq->SSIDs.numOfSSIDs : SIR_SCAN_MAX_NUM_SSID;
	if ((pScanReq->SSIDs.numOfSSIDs != 0)
	    && (eSIR_PASSIVE_SCAN != scanType)) {
		for (i = 0; i < pMsg->numSsid; i++) {
			qdf_mem_copy(&pMsg->ssId[i],
				     &pScanReq->SSIDs.SSIDList[i].SSID,
				     sizeof(tSirMacSSid));
		}
	} else {
		/* Otherwise we scan all SSID and let the result filter later */
		for (i = 0; i < SIR_SCAN_MAX_NUM_SSID; i++)
			pMsg->ssId[i].length = 0;
	}

	pMsg->minChannelTime = minChnTime;
	pMsg->maxChannelTime = maxChnTime;
	pMsg->scan_probe_repeat_time =
		pMac->roam.configParam.scan_probe_repeat_time;
	pMsg->scan_num_probes = pMac->roam.configParam.scan_num_probes;
	/* hidden SSID option */
	pMsg->hiddenSsid = pScanReqParam->hiddenSsid;
	/* maximum rest time */
	pMsg->restTime = pScanReq->restTime;
	/* Minimum rest time */
	pMsg->min_rest_time = pScanReq->min_rest_time;
	/* Idle time */
	pMsg->idle_time = pScanReq->idle_time;
	pMsg->returnAfterFirstMatch = pScanReqParam->bReturnAfter1stMatch;
	/* All the scan results caching will be done by Roaming */
	/* We do not want LIM to do any caching of scan results, */
	/* so delete the LIM cache on all scan requests */
	pMsg->returnFreshResults = pScanReqParam->freshScan;
	/* Always ask for unique result */
	pMsg->returnUniqueResults = pScanReqParam->fUniqueResult;
	pMsg->channelList.numChannels =
		(uint8_t) pScanReq->ChannelInfo.numOfChannels;
	if (pScanReq->ChannelInfo.numOfChannels) {
		/* Assuming the channelNumber is uint8_t (1 byte) */
		qdf_mem_copy(pMsg->channelList.channelNumber,
			     pScanReq->ChannelInfo.ChannelList,
			     pScanReq->ChannelInfo.numOfChannels);
	}

	pMsg->uIEFieldLen = (uint16_t) pScanReq->uIEFieldLen;
	pMsg->uIEFieldOffset = (uint16_t) (sizeof(tSirSmeScanReq) -
			sizeof(pMsg->channelList.channelNumber) +
			(sizeof(pMsg->channelList.channelNumber) *
			 pScanReq->ChannelInfo.numOfChannels));
	if (pScanReq->uIEFieldLen != 0) {
		qdf_mem_copy((uint8_t *) pMsg + pMsg->uIEFieldOffset,
			     pScanReq->pIEField, pScanReq->uIEFieldLen);
	}
	pMsg->p2pSearch = pScanReq->p2pSearch;
	pMsg->scan_id = pScanReq->scan_id;

	pMsg->enable_scan_randomization =
					pScanReq->enable_scan_randomization;
	if (pMsg->enable_scan_randomization) {
		qdf_mem_copy(pMsg->mac_addr, pScanReq->mac_addr,
			     QDF_MAC_ADDR_SIZE);
		qdf_mem_copy(pMsg->mac_addr_mask, pScanReq->mac_addr_mask,
			     QDF_MAC_ADDR_SIZE);
	}

	csr_populate_ie_whitelist_attrs(pMsg, pScanReq);

	pMsg->scan_ctrl_flags_ext = pScanReq->scan_ctrl_flags_ext;

send_scan_req:
	sme_debug(
		"scanId %d domainIdCurrent %d scanType %s (%d) bssType %s (%d) requestType %s (%d) numChannels %d",
		pMsg->scan_id, pMac->scan.domainIdCurrent,
		sme_scan_type_to_string(pMsg->scanType), pMsg->scanType,
		sme_bss_type_to_string(pMsg->bssType), pMsg->bssType,
		sme_request_type_to_string(pScanReq->requestType),
		pScanReq->requestType, pMsg->channelList.numChannels);

	for (i = 0; i < pMsg->channelList.numChannels; i++) {
		sme_debug("channelNumber[%d]= %d", i,
			pMsg->channelList.channelNumber[i]);
	}

	if (QDF_IS_STATUS_SUCCESS(status)) {
		status = cds_send_mb_message_to_mac(pMsg);
	} else {
		sme_err(
			"failed to send down scan req with status: %d",
			status);
		qdf_mem_free(pMsg);
	}
	return status;
}

#ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR
static void csr_diag_scan_channels(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
	host_log_scan_pkt_type *pScanLog = NULL;

	WLAN_HOST_DIAG_LOG_ALLOC(pScanLog,
				 host_log_scan_pkt_type,
				 LOG_WLAN_SCAN_C);
	if (!pScanLog)
		return;

	if (eCsrScanProbeBss == pCommand->u.scanCmd.reason) {
		pScanLog->eventId = WLAN_SCAN_EVENT_HO_SCAN_REQ;
	} else {
		if ((eSIR_PASSIVE_SCAN !=
			pCommand->u.scanCmd.u.scanRequest.scanType)
		    && (eSIR_PASSIVE_SCAN != pMac->scan.curScanType)) {
			pScanLog->eventId = WLAN_SCAN_EVENT_ACTIVE_SCAN_REQ;
		} else {
			pScanLog->eventId = WLAN_SCAN_EVENT_PASSIVE_SCAN_REQ;
		}
	}
	pScanLog->minChnTime =
		(uint8_t) pCommand->u.scanCmd.u.scanRequest.minChnTime;
	pScanLog->maxChnTime =
		(uint8_t) pCommand->u.scanCmd.u.scanRequest.maxChnTime;
	pScanLog->numChannel =
	(uint8_t) pCommand->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels;
	if (pScanLog->numChannel &&
	    (pScanLog->numChannel < HOST_LOG_MAX_NUM_CHANNEL)) {
		qdf_mem_copy(pScanLog->channels,
		      pCommand->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList,
		      pScanLog->numChannel);
	}
	WLAN_HOST_DIAG_LOG_REPORT(pScanLog);
}
#else
#define csr_diag_scan_channels(tpAniSirGlobal pMac, tSmeCmd *pCommand) (void)0;
#endif /* #ifdef FEATURE_WLAN_DIAG_SUPPORT_CSR */

static QDF_STATUS csr_scan_channels(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	struct tag_scanreq_param scanReq;

	/*
	 * Don't delete cached results. Rome rssi based scan candidates may land
	 * up in scan cache instead of LFR cache. They will be deleted upon
	 * query
	 */
	scanReq.freshScan = SIR_BG_SCAN_RETURN_FRESH_RESULTS;
	scanReq.fUniqueResult = true;
	scanReq.hiddenSsid = SIR_SCAN_NO_HIDDEN_SSID;
	if (eCsrScanForSsid == pCommand->u.scanCmd.reason) {
		scanReq.bReturnAfter1stMatch =
			CSR_SCAN_RETURN_AFTER_FIRST_MATCH;
	} else {
		/*
		 * Basically do scan on all channels even for 11D 1st scan case
		 */
		scanReq.bReturnAfter1stMatch =
			CSR_SCAN_RETURN_AFTER_ALL_CHANNELS;
	}
	if (eCsrScanProbeBss == pCommand->u.scanCmd.reason)
		scanReq.hiddenSsid = SIR_SCAN_HIDDEN_SSID_PE_DECISION;
	csr_diag_scan_channels(pMac, pCommand);
	csr_clear_votes_for_country_info(pMac);
	status = csr_send_mb_scan_req(pMac, pCommand->sessionId,
				      &pCommand->u.scanCmd.u.scanRequest,
				      &scanReq);
	return status;
}

static QDF_STATUS
csr_issue_user_scan(tpAniSirGlobal mac_ctx, tSmeCmd *cmd)
{
	int i, j;
	QDF_STATUS status;
	uint32_t len = 0;
	uint8_t *ch_lst = NULL;
	tCsrChannelInfo new_ch_info = { 0, NULL };

	if (!mac_ctx->roam.configParam.fScanTwice)
		return csr_scan_channels(mac_ctx, cmd);

	/* We scan 2.4 channel twice */
	if (cmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels
	    && (NULL != cmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList)) {
		len = cmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels;
		/* allocate twice the channel */
		new_ch_info.ChannelList = (uint8_t *) qdf_mem_malloc(len * 2);
		ch_lst = cmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList;
	} else {
		/* get the valid channel list to scan all. */
		len = sizeof(mac_ctx->roam.validChannelList);
		status = csr_get_cfg_valid_channels(mac_ctx,
			    (uint8_t *) mac_ctx->roam.validChannelList, &len);
		if (QDF_IS_STATUS_SUCCESS(status)) {
			/* allocate twice the channel */
			new_ch_info.ChannelList =
				(uint8_t *) qdf_mem_malloc(len * 2);
			ch_lst = mac_ctx->roam.validChannelList;
		}
	}
	if (NULL == new_ch_info.ChannelList) {
		new_ch_info.numOfChannels = 0;
	} else {
		j = 0;
		for (i = 0; i < len; i++) {
			new_ch_info.ChannelList[j++] = ch_lst[i];
			if (CDS_MAX_24GHZ_CHANNEL_NUMBER >= ch_lst[i])
				new_ch_info.ChannelList[j++] = ch_lst[i];
		}
		if (NULL !=
		    cmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList) {
			/*
			 * ch_lst points to the channellist from the command,
			 * free it.
			 */
			qdf_mem_free(
			  cmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList);
			cmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList =
			  NULL;
		}
		cmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels = j;
		cmd->u.scanCmd.u.scanRequest.ChannelInfo.ChannelList =
							new_ch_info.ChannelList;
	}

	return csr_scan_channels(mac_ctx, cmd);
}

QDF_STATUS csr_process_scan_command(tpAniSirGlobal pMac, tSmeCmd *pCommand)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	sme_debug("starting SCAN cmd in %d state. reason %d",
		pCommand->u.scanCmd.lastRoamState[pCommand->sessionId],
		pCommand->u.scanCmd.reason);

	switch (pCommand->u.scanCmd.reason) {
	case eCsrScanUserRequest:
		status = csr_issue_user_scan(pMac, pCommand);
		break;
	default:
		status = csr_scan_channels(pMac, pCommand);
		break;
	}

	if (!QDF_IS_STATUS_SUCCESS(status))
		csr_release_scan_command(pMac, pCommand, eCSR_SCAN_FAILURE);

	return status;
}

/**
 * csr_scan_copy_request_valid_channels_only() - scan request of valid channels
 * @mac_ctx : pointer to Global Mac Structure
 * @dst_req: pointer to tCsrScanRequest
 * @skip_dfs_chnl: 1 - skip dfs channel, 0 - don't skip dfs channel
 * @src_req: pointer to tCsrScanRequest
 *
 * This function makes a copy of scan request with valid channels
 *
 * Return: none
 */
static void csr_scan_copy_request_valid_channels_only(tpAniSirGlobal mac_ctx,
				tCsrScanRequest *dst_req, uint8_t skip_dfs_chnl,
				tCsrScanRequest *src_req)
{
	uint32_t index = 0;
	uint32_t new_index = 0;
	uint16_t  unsafe_chan[NUM_CHANNELS];
	uint16_t  unsafe_chan_cnt = 0;
	uint16_t  cnt = 0;
	bool      is_unsafe_chan;
	qdf_device_t qdf_ctx = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);

	if (!qdf_ctx) {
		cds_err("qdf_ctx is NULL");
		return;
	}
	pld_get_wlan_unsafe_channel(qdf_ctx->dev, unsafe_chan,
			&unsafe_chan_cnt,
			sizeof(unsafe_chan));

	if (mac_ctx->roam.configParam.sta_roam_policy.dfs_mode ==
			CSR_STA_ROAM_POLICY_DFS_DISABLED)
		skip_dfs_chnl = true;

	for (index = 0; index < src_req->ChannelInfo.numOfChannels; index++) {
		/* Allow scan on valid channels only.
		 * If it is p2p scan and valid channel list doesnt contain
		 * social channels, enforce scan on social channels because
		 * that is the only way to find p2p peers.
		 * This can happen only if band is set to 5Ghz mode.
		 */
		if (src_req->ChannelInfo.ChannelList[index] <
		    CDS_MIN_11P_CHANNEL &&
			((csr_roam_is_valid_channel(mac_ctx,
			src_req->ChannelInfo.ChannelList[index])) ||
			((eCSR_SCAN_P2P_DISCOVERY == src_req->requestType) &&
			CSR_IS_SOCIAL_CHANNEL(
				src_req->ChannelInfo.ChannelList[index])))) {
			if (((src_req->skipDfsChnlInP2pSearch || skip_dfs_chnl)
				&& (CHANNEL_STATE_DFS ==
				cds_get_channel_state(src_req->
							ChannelInfo.
							ChannelList
							[index]))) &&
				(src_req->ChannelInfo.numOfChannels > 1)) {
				sme_debug(
					"reqType= %s (%d), numOfChannels=%d, ignoring DFS channel %d",
					sme_request_type_to_string(
						src_req->requestType),
					src_req->requestType,
					src_req->ChannelInfo.numOfChannels,
					src_req->ChannelInfo.ChannelList
						[index]);
				continue;
			}
			if (mac_ctx->roam.configParam.
					sta_roam_policy.skip_unsafe_channels &&
					unsafe_chan_cnt) {
				is_unsafe_chan = false;
				for (cnt = 0; cnt < unsafe_chan_cnt; cnt++) {
					if (unsafe_chan[cnt] ==
						src_req->ChannelInfo.
						ChannelList[index]) {
						is_unsafe_chan = true;
						break;
					}
				}
				if (is_unsafe_chan &&
					((CSR_IS_CHANNEL_24GHZ(
						src_req->ChannelInfo.
						ChannelList[index]) &&
					mac_ctx->roam.configParam.
					sta_roam_policy.sap_operating_band ==
						eCSR_BAND_24) ||
						(CDS_IS_CHANNEL_5GHZ(
							src_req->ChannelInfo.
							ChannelList[index]) &&
					mac_ctx->roam.configParam.
					sta_roam_policy.sap_operating_band ==
						eCSR_BAND_5G))) {
					QDF_TRACE(QDF_MODULE_ID_SME,
						QDF_TRACE_LEVEL_DEBUG,
					      FL("ignoring unsafe channel %d"),
						src_req->ChannelInfo.
						ChannelList[index]);
					continue;
				}
			}

			dst_req->ChannelInfo.ChannelList[new_index] =
				src_req->ChannelInfo.ChannelList[index];
			new_index++;
		}
	}
	dst_req->ChannelInfo.numOfChannels = new_index;
}

/**
 * csr_scan_filter_given_chnl_band() - filter all channels which matches given
 *                                    channel's band
 * @mac_ctx: pointer to mac context
 * @channel: Given channel
 * @dst_req: destination scan request
 *
 * when ever particular connection already exist, STA should not scan the
 * channels which fall under same band as given channel's band.
 * this routine will filter out those channels
 *
 * Return: true if success otherwise false for any failure
 */
static bool csr_scan_filter_given_chnl_band(tpAniSirGlobal mac_ctx,
			uint8_t channel, tCsrScanRequest *dst_req) {
	uint8_t valid_chnl_list[WNI_CFG_VALID_CHANNEL_LIST_LEN] = {0};
	uint32_t filter_chnl_len = 0, i = 0;
	uint32_t valid_chnl_len = WNI_CFG_VALID_CHANNEL_LIST_LEN;

	if (!channel) {
		sme_debug("Nothing to filter as no IBSS session");
		return true;
	}

	if (!dst_req) {
		sme_err("No valid scan requests");
		return false;
	}
	/*
	 * In case of concurrent IBSS session exist, scan only
	 * those channels which are not in IBSS channel's band.
	 * In case if no-concurrent IBSS session exist then scan
	 * full band
	 */
	if (dst_req->ChannelInfo.numOfChannels == 0) {
		csr_get_cfg_valid_channels(mac_ctx, valid_chnl_list,
				&valid_chnl_len);
	} else {
		valid_chnl_len = (WNI_CFG_VALID_CHANNEL_LIST_LEN >
					dst_req->ChannelInfo.numOfChannels) ?
					dst_req->ChannelInfo.numOfChannels :
					WNI_CFG_VALID_CHANNEL_LIST_LEN;
		qdf_mem_copy(valid_chnl_list, dst_req->ChannelInfo.ChannelList,
				valid_chnl_len);
	}
	for (i = 0; i < valid_chnl_len; i++) {
		/*
		 * Don't allow DSRC channel when IBSS or SAP DFS concurrent
		 * connection is up
		 */
		if (valid_chnl_list[i] >= CDS_MIN_11P_CHANNEL)
			continue;
		if (CDS_IS_CHANNEL_5GHZ(channel) &&
			CDS_IS_CHANNEL_24GHZ(valid_chnl_list[i])) {
			valid_chnl_list[filter_chnl_len] =
					valid_chnl_list[i];
			filter_chnl_len++;
		} else if (CDS_IS_CHANNEL_24GHZ(channel) &&
			CDS_IS_CHANNEL_5GHZ(valid_chnl_list[i])) {
			valid_chnl_list[filter_chnl_len] =
					valid_chnl_list[i];
			filter_chnl_len++;
		}
	}
	if (filter_chnl_len == 0) {
		sme_err("there no channels to scan due to IBSS session");
		return false;
	}

	if (dst_req->ChannelInfo.ChannelList) {
		qdf_mem_free(dst_req->ChannelInfo.ChannelList);
		dst_req->ChannelInfo.ChannelList = NULL;
		dst_req->ChannelInfo.numOfChannels = 0;
	}

	dst_req->ChannelInfo.ChannelList =
			qdf_mem_malloc(filter_chnl_len *
				sizeof(*dst_req->ChannelInfo.ChannelList));
	dst_req->ChannelInfo.numOfChannels = filter_chnl_len;
	if (NULL == dst_req->ChannelInfo.ChannelList) {
		sme_err("Memory allocation failed");
		return false;
	}
	qdf_mem_copy(dst_req->ChannelInfo.ChannelList, valid_chnl_list,
			filter_chnl_len);
	return true;
}

/**
 * csr_scan_copy_ie_whitelist_attrs() - Copies ie whitelist attrs
 * @dst_req: pointer to tCsrScanRequest
 * @src_req: pointer to tCsrScanRequest
 *
 * This function makes a copy of ie whitelist attrs
 *
 * Return: QDF_STATUS_SUCCESS - for successful allocation and copy
 *         QDF_STATUS_E_NOMEM - when failed to allocate memory
 */
static QDF_STATUS csr_scan_copy_ie_whitelist_attrs(tCsrScanRequest *dst_req,
						   tCsrScanRequest *src_req)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!src_req->num_vendor_oui) {
		dst_req->num_vendor_oui = 0;
		dst_req->voui = NULL;
		return status;
	}

	dst_req->voui = qdf_mem_malloc(src_req->num_vendor_oui *
				       sizeof(*dst_req->voui));
	if (!dst_req->voui) {
		status = QDF_STATUS_E_NOMEM;
		dst_req->num_vendor_oui = 0;
		sme_err("No memory for voui");
	} else {
		dst_req->num_vendor_oui = src_req->num_vendor_oui;
		qdf_mem_copy(dst_req->voui, src_req->voui,
			     src_req->num_vendor_oui * sizeof(*dst_req->voui));
	}

	return status;
}

/**
 * csr_scan_copy_request() - Function to copy scan request
 * @mac_ctx : pointer to Global Mac Structure
 * @dst_req: pointer to tCsrScanRequest
 * @src_req: pointer to tCsrScanRequest
 *
 * This function makes a copy of scan request
 *
 * Return: 0 - Success, Error number - Failure
 */
QDF_STATUS csr_scan_copy_request(tpAniSirGlobal mac_ctx,
				tCsrScanRequest *dst_req,
				tCsrScanRequest *src_req)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	uint32_t len = sizeof(mac_ctx->roam.validChannelList);
	uint32_t index = 0;
	uint32_t new_index = 0;
	enum channel_state channel_state;
	uint8_t channel = 0;

	bool skip_dfs_chnl =
			mac_ctx->roam.configParam.initial_scan_no_dfs_chnl ||
				!mac_ctx->scan.fEnableDFSChnlScan;

	status = csr_scan_free_request(mac_ctx, dst_req);
	if (!QDF_IS_STATUS_SUCCESS(status))
		goto complete;
	qdf_mem_copy(dst_req, src_req, sizeof(tCsrScanRequest));
	/* Re-initialize the pointers to NULL since we did a copy */
	dst_req->pIEField = NULL;
	dst_req->ChannelInfo.ChannelList = NULL;
	dst_req->SSIDs.SSIDList = NULL;
	dst_req->SSIDs.numOfSSIDs = 0;
	dst_req->voui = NULL;

	if (src_req->uIEFieldLen) {
		dst_req->pIEField =
			qdf_mem_malloc(src_req->uIEFieldLen);
		if (NULL == dst_req->pIEField) {
			status = QDF_STATUS_E_NOMEM;
			sme_err("No memory for scanning IE fields");
			goto complete;
		} else {
			status = QDF_STATUS_SUCCESS;
			qdf_mem_copy(dst_req->pIEField, src_req->pIEField,
				src_req->uIEFieldLen);
			dst_req->uIEFieldLen = src_req->uIEFieldLen;
		}
	}

	/* Allocate memory for IE field */
	if (src_req->ChannelInfo.numOfChannels == 0) {
		dst_req->ChannelInfo.ChannelList = NULL;
		dst_req->ChannelInfo.numOfChannels = 0;
	} else {
		dst_req->ChannelInfo.ChannelList =
			qdf_mem_malloc(src_req->ChannelInfo.numOfChannels *
				sizeof(*dst_req->ChannelInfo.ChannelList));
		if (NULL == dst_req->ChannelInfo.ChannelList) {
			status = QDF_STATUS_E_NOMEM;
			dst_req->ChannelInfo.numOfChannels = 0;
			sme_err("No memory for scanning Channel List");
			goto complete;
		}

		if ((src_req->scanType == eSIR_PASSIVE_SCAN) &&
			(src_req->requestType == eCSR_SCAN_REQUEST_11D_SCAN)) {
			for (index = 0; index < src_req->ChannelInfo.
						numOfChannels; index++) {
				channel_state =
					cds_get_channel_state(src_req->
							ChannelInfo.
							ChannelList[index]);
				if (src_req->ChannelInfo.ChannelList[index] <
						CDS_MIN_11P_CHANNEL &&
					((CHANNEL_STATE_ENABLE ==
						channel_state) ||
					((CHANNEL_STATE_DFS == channel_state) &&
					!skip_dfs_chnl))) {
					dst_req->ChannelInfo.ChannelList
							[new_index] =
								src_req->
								ChannelInfo.
								ChannelList
								[index];
					new_index++;
				}
			}
			dst_req->ChannelInfo.numOfChannels = new_index;
		} else if (QDF_IS_STATUS_SUCCESS(
				csr_get_cfg_valid_channels(mac_ctx,
						mac_ctx->roam.validChannelList,
						&len))) {
			new_index = 0;
			mac_ctx->roam.numValidChannels = len;
			csr_scan_copy_request_valid_channels_only(mac_ctx,
							dst_req, skip_dfs_chnl,
							src_req);
		} else {
			sme_err(
				"Couldn't get the valid Channel List, keeping requester's list");
			new_index = 0;
			for (index = 0; index < src_req->ChannelInfo.
					numOfChannels; index++) {
				if (src_req->ChannelInfo.ChannelList[index] <
						CDS_MIN_11P_CHANNEL) {
					dst_req->ChannelInfo.
						ChannelList[new_index] =
						src_req->ChannelInfo.
						ChannelList[index];
					new_index++;
				}
			}
			dst_req->ChannelInfo.numOfChannels =
				new_index;
		}
	} /* Allocate memory for Channel List */

	/*
	 * If IBSS concurrent connection exist, and if the scan
	 * request comes from STA adapter then we need to filter
	 * out IBSS channel's band otherwise it will cause issue
	 * in IBSS+STA concurrency
	 *
	 * If DFS SAP/GO concurrent connection exist, and if the scan
	 * request comes from STA adapter then we need to filter
	 * out SAP/GO channel's band otherwise it will cause issue in
	 * SAP+STA concurrency
	 */
	if (cds_is_ibss_conn_exist(&channel)) {
		sme_debug("Conc IBSS exist, channel list will be modified");
	} else if (cds_is_any_dfs_beaconing_session_present(&channel)) {
		/*
		 * 1) if agile & DFS scans are supported
		 * 2) if hardware is DBS capable
		 * 3) if current hw mode is non-dbs
		 * if all above 3 conditions are true then don't skip any
		 * channel from scan list
		 */
		if (true != wma_is_current_hwmode_dbs() &&
		    wma_get_dbs_plus_agile_scan_config() &&
		    wma_get_single_mac_scan_with_dfs_config())
			channel = 0;
		else
			sme_debug(
				"Conc DFS SAP/GO exist, channel list will be modified");
	}

	if ((channel > 0) &&
	    (!csr_scan_filter_given_chnl_band(mac_ctx, channel, dst_req))) {
		sme_err("Can't filter channels due to IBSS/SAP DFS");
		goto complete;
	}

	if (src_req->SSIDs.numOfSSIDs == 0) {
		dst_req->SSIDs.numOfSSIDs = 0;
		dst_req->SSIDs.SSIDList = NULL;
	} else {
		dst_req->SSIDs.SSIDList =
			qdf_mem_malloc(src_req->SSIDs.numOfSSIDs *
					sizeof(*dst_req->SSIDs.SSIDList));
		if (NULL == dst_req->SSIDs.SSIDList)
			status = QDF_STATUS_E_NOMEM;
		else
			status = QDF_STATUS_SUCCESS;
		if (QDF_IS_STATUS_SUCCESS(status)) {
			dst_req->SSIDs.numOfSSIDs =
				src_req->SSIDs.numOfSSIDs;
			qdf_mem_copy(dst_req->SSIDs.SSIDList,
				src_req->SSIDs.SSIDList,
				src_req->SSIDs.numOfSSIDs *
				sizeof(*dst_req->SSIDs.SSIDList));
		} else {
			dst_req->SSIDs.numOfSSIDs = 0;
			sme_err("No memory for scanning SSID List");
			goto complete;
		}
	} /* Allocate memory for SSID List */
	qdf_mem_copy(&dst_req->bssid, &src_req->bssid,
		sizeof(struct qdf_mac_addr));
	dst_req->p2pSearch = src_req->p2pSearch;
	dst_req->skipDfsChnlInP2pSearch =
		src_req->skipDfsChnlInP2pSearch;
	dst_req->scan_id = src_req->scan_id;
	dst_req->timestamp = src_req->timestamp;

	status = csr_scan_copy_ie_whitelist_attrs(dst_req, src_req);

complete:
	if (!QDF_IS_STATUS_SUCCESS(status))
		csr_scan_free_request(mac_ctx, dst_req);

	return status;
}

QDF_STATUS csr_scan_free_request(tpAniSirGlobal pMac, tCsrScanRequest *pReq)
{

	if (pReq->ChannelInfo.ChannelList) {
		qdf_mem_free(pReq->ChannelInfo.ChannelList);
		pReq->ChannelInfo.ChannelList = NULL;
	}
	pReq->ChannelInfo.numOfChannels = 0;
	if (pReq->pIEField) {
		qdf_mem_free(pReq->pIEField);
		pReq->pIEField = NULL;
	}
	pReq->uIEFieldLen = 0;
	if (pReq->SSIDs.SSIDList) {
		qdf_mem_free(pReq->SSIDs.SSIDList);
		pReq->SSIDs.SSIDList = NULL;
	}
	pReq->SSIDs.numOfSSIDs = 0;

	if (pReq->voui) {
		qdf_mem_free(pReq->voui);
		pReq->voui = NULL;
	}
	pReq->num_vendor_oui = 0;

	return QDF_STATUS_SUCCESS;
}

void csr_scan_call_callback(tpAniSirGlobal pMac, tSmeCmd *pCommand,
			    eCsrScanStatus scanStatus)
{
	if (pCommand->u.scanCmd.callback) {
		/*
		 * In case of eCsrScanForSsid scan, scan is aborted as host gets
		 * the desired AP, this doesn't necessaryly mean that scan is
		 * failed, send abort indication to upper layers only if scan
		 * status is not success
		 */
		if (pCommand->u.scanCmd.abort_scan_indication) {
			if ((pCommand->u.scanCmd.reason != eCsrScanForSsid) ||
			   (scanStatus != eCSR_SCAN_SUCCESS)) {
				sme_debug("scanDone due to abort");
				scanStatus = eCSR_SCAN_ABORT;
			}
		}
		pCommand->u.scanCmd.callback(pMac, pCommand->u.scanCmd.pContext,
					     pCommand->sessionId,
					     pCommand->u.scanCmd.scanID,
					     scanStatus);
	} else {
		sme_debug("Callback NULL!!!");
	}
}

/**
 * csr_purge_scan_result_by_age() - Purge scan results based on Age
 * @pv: Global context
 *
 * This routine is to purge scan results based on aging.
 *
 * Return: None
 */
static void csr_purge_scan_result_by_age(void *pv)
{
	tpAniSirGlobal mac_ctx = PMAC_STRUCT(pv);
	tListElem *entry, *tmp_entry;
	struct tag_csrscan_result *result;
	uint64_t ageout_time =
		mac_ctx->scan.scanResultCfgAgingTime * SYSTEM_TIME_SEC_TO_MSEC;
	uint64_t cur_time = qdf_mc_timer_get_system_time();
	uint8_t *bssId;

	csr_ll_lock(&mac_ctx->scan.scanResultList);
	entry = csr_ll_peek_head(&mac_ctx->scan.scanResultList,
				LL_ACCESS_NOLOCK);
	sme_debug("Ageout time=%llu", ageout_time);
	while (entry) {
		tmp_entry = csr_ll_next(&mac_ctx->scan.scanResultList, entry,
					LL_ACCESS_NOLOCK);
		result = GET_BASE_ADDR(entry, struct tag_csrscan_result, Link);

		if ((cur_time - result->Result.BssDescriptor.received_time) >
			    ageout_time) {
			bssId = result->Result.BssDescriptor.bssId;
			sme_debug("age out BSSID " MAC_ADDRESS_STR" Channel %d",
				MAC_ADDR_ARRAY(bssId),
				result->Result.BssDescriptor.channelId);
			csr_scan_age_out_bss(mac_ctx, result);
		}
		entry = tmp_entry;
	}
	csr_ll_unlock(&mac_ctx->scan.scanResultList);
}

bool csr_scan_remove_fresh_scan_command(tpAniSirGlobal pMac, uint8_t sessionId)
{
	bool fRet = false;
	tListElem *pEntry, *pEntryTmp;
	tSmeCmd *pCommand;
	tDblLinkList localList;
	tDblLinkList *pCmdList;

	qdf_mem_zero(&localList, sizeof(tDblLinkList));
	if (!QDF_IS_STATUS_SUCCESS(csr_ll_open(pMac->hHdd, &localList))) {
		sme_err("failed to open list");
		return fRet;
	}

	pCmdList = &pMac->sme.smeScanCmdPendingList;

	csr_ll_lock(pCmdList);
	pEntry = csr_ll_peek_head(pCmdList, LL_ACCESS_NOLOCK);
	while (pEntry) {
		pEntryTmp = csr_ll_next(pCmdList, pEntry, LL_ACCESS_NOLOCK);
		pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
		if (!((eSmeCommandScan == pCommand->command)
		    && (sessionId == pCommand->sessionId))) {
			pEntry = pEntryTmp;
			continue;
		}
		sme_debug("-------- abort scan command reason = %d",
			pCommand->u.scanCmd.reason);
		/* The rest are fresh scan requests */
		if (csr_ll_remove_entry(pCmdList, pEntry,
					LL_ACCESS_NOLOCK)) {
			csr_ll_insert_tail(&localList, pEntry,
					   LL_ACCESS_NOLOCK);
		}
		fRet = true;
		pEntry = pEntryTmp;
	}

	csr_ll_unlock(pCmdList);

	while ((pEntry = csr_ll_remove_head(&localList, LL_ACCESS_NOLOCK))) {
		pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
		if (pCommand->u.scanCmd.callback) {
			/*
			 * User scan request is pending, send response with
			 * status eCSR_SCAN_ABORT
			 */
			pCommand->u.scanCmd.callback(pMac,
				pCommand->u.scanCmd.pContext, sessionId,
				pCommand->u.scanCmd.scanID, eCSR_SCAN_ABORT);
		}
		csr_release_command_scan(pMac, pCommand);
	}
	csr_ll_close(&localList);

	return fRet;
}

void csr_release_scan_command(tpAniSirGlobal pMac, tSmeCmd *pCommand,
			      eCsrScanStatus scanStatus)
{
	eCsrScanReason reason = pCommand->u.scanCmd.reason;
	bool status;
	tDblLinkList *cmd_list = NULL;

	csr_scan_call_callback(pMac, pCommand, scanStatus);
	sme_debug("Remove Scan command reason = %d, scan_id %d",
		reason, pCommand->u.scanCmd.scanID);
	cmd_list = &pMac->sme.smeScanCmdActiveList;
	status = csr_ll_remove_entry(cmd_list, &pCommand->Link, LL_ACCESS_LOCK);
	if (!status) {
		sme_err("cannot release command reason %d scan_id %d",
			pCommand->u.scanCmd.reason,
			pCommand->u.scanCmd.scanID);
		return;
	}
	csr_release_command_scan(pMac, pCommand);
}

#ifdef WLAN_FEATURE_ROAM_OFFLOAD
tSmeCmd *csr_find_self_reassoc_cmd(tpAniSirGlobal mac_ctx, uint32_t session_id)
{
	tDblLinkList *cmd_list = NULL;
	tListElem *entry;
	tSmeCmd *temp_cmd = NULL;

	cmd_list = &mac_ctx->sme.smeCmdActiveList;
	entry = csr_ll_peek_head(cmd_list, LL_ACCESS_LOCK);
	if (!entry) {
		sme_err("Queue empty");
		return NULL;
	}
	temp_cmd = GET_BASE_ADDR(entry, tSmeCmd, Link);
	if ((temp_cmd->command != e_sme_command_issue_self_reassoc) ||
			(temp_cmd->sessionId != session_id)) {
		sme_debug("no self-reassoc cmd active");
		return NULL;
	}

	return temp_cmd;
}

void csr_remove_same_ap_reassoc_cmd(tpAniSirGlobal mac_ctx, tSmeCmd *sme_cmd)
{
	bool status = false;
	tDblLinkList *cmd_list = NULL;

	cmd_list = &mac_ctx->sme.smeCmdActiveList;
	sme_debug("Remove self reassoc cmd = %d, session_id = %d",
		sme_cmd->command, sme_cmd->sessionId);
	status = csr_ll_remove_entry(cmd_list,
			&sme_cmd->Link, LL_ACCESS_LOCK);
	if (!status) {
		sme_err(
			"can't del self-reassoc cmd %d session %d",
			sme_cmd->command, sme_cmd->sessionId);
		QDF_ASSERT(0);
	}
	sme_process_pending_queue(mac_ctx);
}
#else
tSmeCmd *csr_find_self_reassoc_cmd(tpAniSirGlobal mac_ctx, uint32_t session_id)
{
	return NULL;
}

void csr_remove_same_ap_reassoc_cmd(tpAniSirGlobal mac_ctx, tSmeCmd *sme_cmd)
{
}
#endif

QDF_STATUS csr_scan_get_pmkid_candidate_list(tpAniSirGlobal pMac,
					     uint32_t sessionId,
					     tPmkidCandidateInfo *pPmkidList,
					     uint32_t *pNumItems)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);
	tCsrScanResultFilter *pScanFilter;
	tCsrScanResultInfo *pScanResult;
	tScanResultHandle hBSSList;
	uint32_t nItems = *pNumItems;

	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}

	sme_debug("pMac->scan.NumPmkidCandidate = %d",
		pSession->NumPmkidCandidate);
	csr_reset_pmkid_candidate_list(pMac, sessionId);
	if (!(csr_is_conn_state_connected(pMac, sessionId)
	    && pSession->pCurRoamProfile))
		return status;

	*pNumItems = 0;
	pScanFilter = qdf_mem_malloc(sizeof(tCsrScanResultFilter));
	if (NULL == pScanFilter)
		return QDF_STATUS_E_NOMEM;

	/* Here is the profile we need to connect to */
	status = csr_roam_prepare_filter_from_profile(pMac,
					pSession->pCurRoamProfile, pScanFilter);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		qdf_mem_free(pScanFilter);
		return status;
	}

	status = csr_scan_get_result(pMac, pScanFilter, &hBSSList);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		csr_free_scan_filter(pMac, pScanFilter);
		qdf_mem_free(pScanFilter);
		return status;
	}

	if (pSession->NumPmkidCandidate < nItems) {
		pScanResult = csr_scan_result_get_next(pMac, hBSSList);
		while (pScanResult != NULL) {
			/* NumPmkidCandidate adds up here */
			csr_process_bss_desc_for_pmkid_list(pMac,
				&pScanResult->BssDescriptor,
				(tDot11fBeaconIEs *)(pScanResult->pvIes),
				sessionId);
			pScanResult = csr_scan_result_get_next(pMac, hBSSList);
		}
	}

	if (pSession->NumPmkidCandidate) {
		*pNumItems = pSession->NumPmkidCandidate;
		qdf_mem_copy(pPmkidList, pSession->PmkidCandidateInfo,
			     pSession->NumPmkidCandidate *
			     sizeof(tPmkidCandidateInfo));
	}

	csr_scan_result_purge(pMac, hBSSList);
	csr_free_scan_filter(pMac, pScanFilter);
	qdf_mem_free(pScanFilter);
	return status;
}

#ifdef FEATURE_WLAN_WAPI
QDF_STATUS csr_scan_get_bkid_candidate_list(tpAniSirGlobal pMac,
					    uint32_t sessionId,
					    tBkidCandidateInfo *pBkidList,
					    uint32_t *pNumItems)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);
	tCsrScanResultFilter *pScanFilter;
	tCsrScanResultInfo *pScanResult;
	tScanResultHandle hBSSList;
	uint32_t nItems = *pNumItems;

	if (!pSession) {
		sme_err("session %d not found", sessionId);
		return QDF_STATUS_E_FAILURE;
	}

	sme_debug("pMac->scan.NumBkidCandidate = %d",
		pSession->NumBkidCandidate);
	csr_reset_bkid_candidate_list(pMac, sessionId);
	if (!(csr_is_conn_state_connected(pMac, sessionId)
	    && pSession->pCurRoamProfile))
		return status;

	*pNumItems = 0;
	pScanFilter = qdf_mem_malloc(sizeof(tCsrScanResultFilter));
	if (NULL == pScanFilter)
		return QDF_STATUS_E_NOMEM;

	/* Here is the profile we need to connect to */
	status = csr_roam_prepare_filter_from_profile(pMac,
					pSession->pCurRoamProfile, pScanFilter);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		qdf_mem_free(pScanFilter);
		return status;
	}

	status = csr_scan_get_result(pMac, pScanFilter, &hBSSList);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		csr_free_scan_filter(pMac, pScanFilter);
		qdf_mem_free(pScanFilter);
		return status;
	}

	if (pSession->NumBkidCandidate < nItems) {
		pScanResult = csr_scan_result_get_next(pMac, hBSSList);
		while (pScanResult != NULL) {
			/* pMac->scan.NumBkidCandidate adds up here */
			csr_process_bss_desc_for_bkid_list(pMac,
				&pScanResult->BssDescriptor,
				(tDot11fBeaconIEs *)(pScanResult->pvIes));
			pScanResult = csr_scan_result_get_next(pMac, hBSSList);
		}
	}

	if (pSession->NumBkidCandidate) {
		*pNumItems = pSession->NumBkidCandidate;
		qdf_mem_copy(pBkidList, pSession->BkidCandidateInfo,
			     pSession->NumBkidCandidate *
			     sizeof(tBkidCandidateInfo));
	}

	csr_scan_result_purge(pMac, hBSSList);
	csr_free_scan_filter(pMac, pScanFilter);
	qdf_mem_free(pScanFilter);
	return status;
}
#endif /* FEATURE_WLAN_WAPI */

/**
 * csr_roam_copy_channellist() - Function to copy channel list
 * @mac_ctx: pointer to Global Mac structure
 * @profile: pointer to tCsrRoamProfile
 * @scan_cmd: pointer to tSmeCmd
 * @index: index for channellist
 *
 * Function copies channel list
 *
 * Return: none
 */
static void csr_roam_copy_channellist(tpAniSirGlobal mac_ctx,
				tCsrRoamProfile *profile,
				tSmeCmd *scan_cmd, uint8_t index)
{
	tCsrChannelInfo *channel_info =
		&scan_cmd->u.scanCmd.u.scanRequest.ChannelInfo;

	for (index = 0; index < profile->ChannelInfo.numOfChannels;
			index++) {
		if (!csr_roam_is_valid_channel(mac_ctx,
			profile->ChannelInfo.ChannelList[index])) {
			sme_warn("process a channel: %d that is invalid",
			profile->ChannelInfo.ChannelList[index]);
			continue;
		}
		channel_info->ChannelList[channel_info->numOfChannels] =
			profile->ChannelInfo.ChannelList[index];
		scan_cmd->u.scanCmd.u.scanRequest.ChannelInfo.numOfChannels++;
	}
}

/**
 * csr_ssid_scan_done_callback() - Callback to indicate
 *                            scan is done for ssid scan
 * @halHandle: handle to hal
 * @context: SSID scan context
 * @scanId: Scan id for the scheduled scan
 * @status: scan done status
 *
 * Return - QDF_STATUS
 */
static QDF_STATUS csr_ssid_scan_done_callback(tHalHandle halHandle,
		void *context,
		uint8_t sessionId,
		uint32_t scanId,
		eCsrScanStatus status)
{
	struct csr_scan_for_ssid_context *scan_context =
		(struct csr_scan_for_ssid_context *)context;

	if (NULL == scan_context) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
				FL("scan for ssid context not found"));
		return QDF_STATUS_E_INVAL;
	}

	if (eCSR_SCAN_ABORT == status)
		csr_roam_call_callback(scan_context->mac_ctx,
				scan_context->session_id,
				NULL, scan_context->roam_id,
				eCSR_ROAM_ASSOCIATION_FAILURE,
				eCSR_ROAM_RESULT_SCAN_FOR_SSID_FAILURE);
	qdf_mem_free(scan_context);
	return QDF_STATUS_SUCCESS;
}


/**
 * csr_scan_for_ssid() -  Function usually used for BSSs that suppresses SSID
 * @mac_ctx: Pointer to Global Mac structure
 * @profile: pointer to tCsrRoamProfile
 * @roam_id: variable representing roam id
 * @notify: boolean variable
 *
 * Function is usually used for BSSs that suppresses SSID so the profile
 * shall have one and only one SSID.
 *
 * Return: Success - QDF_STATUS_SUCCESS, Failure - error number
 */
QDF_STATUS csr_scan_for_ssid(tpAniSirGlobal mac_ctx, uint32_t session_id,
			tCsrRoamProfile *profile, uint32_t roam_id,
			bool notify)
{
	QDF_STATUS status = QDF_STATUS_E_INVAL;
	tSmeCmd *scan_cmd = NULL;
	tCsrScanRequest *scan_req = NULL;
	uint8_t index = 0;
	uint32_t num_ssid = profile->SSIDs.numOfSSIDs;
	tpCsrNeighborRoamControlInfo neighbor_roaminfo =
		&mac_ctx->roam.neighborRoamInfo[session_id];
	tCsrSSIDs *ssids = NULL;
	struct csr_scan_for_ssid_context *context;

	if (!(mac_ctx->scan.fScanEnable) && (num_ssid != 1)) {
		sme_err(
			"cannot scan because scanEnable (%d) or numSSID (%d) is invalid",
			mac_ctx->scan.fScanEnable, profile->SSIDs.numOfSSIDs);
		return status;
	}

	scan_cmd = csr_get_command_buffer(mac_ctx);

	if (!scan_cmd) {
		sme_err("failed to allocate command buffer");
		goto error;
	}

	qdf_mem_set(&scan_cmd->u.scanCmd, sizeof(tScanCmd), 0);
	scan_cmd->u.scanCmd.pToRoamProfile =
			qdf_mem_malloc(sizeof(tCsrRoamProfile));

	if (NULL == scan_cmd->u.scanCmd.pToRoamProfile)
		status = QDF_STATUS_E_NOMEM;
	else
		status = csr_roam_copy_profile(mac_ctx,
					scan_cmd->u.scanCmd.pToRoamProfile,
					profile);

	context = qdf_mem_malloc(sizeof(*context));
	if (NULL == context) {
		sme_err("Failed to allocate memory for ssid scan context");
		goto error;
	}
	context->mac_ctx = mac_ctx;
	context->session_id = session_id;
	context->roam_id = roam_id;

	if (!QDF_IS_STATUS_SUCCESS(status))
		goto error;

	scan_cmd->u.scanCmd.roamId = roam_id;
	scan_cmd->command = eSmeCommandScan;
	scan_cmd->sessionId = (uint8_t) session_id;
	scan_cmd->u.scanCmd.callback = csr_ssid_scan_done_callback;
	scan_cmd->u.scanCmd.pContext = context;
	scan_cmd->u.scanCmd.reason = eCsrScanForSsid;

	/* let it wrap around */
	wma_get_scan_id(&scan_cmd->u.scanCmd.scanID);
	qdf_mem_set(&scan_cmd->u.scanCmd.u.scanRequest,
			sizeof(tCsrScanRequest), 0);
	status = qdf_mc_timer_init(&scan_cmd->u.scanCmd.csr_scan_timer,
			QDF_TIMER_TYPE_SW,
			csr_scan_active_list_timeout_handle, scan_cmd);
	scan_req = &scan_cmd->u.scanCmd.u.scanRequest;
	scan_req->scanType = eSIR_ACTIVE_SCAN;
	scan_req->BSSType = profile->BSSType;
	scan_req->scan_id = scan_cmd->u.scanCmd.scanID;
	/*
	 * To avoid 11b rate in probe request Set p2pSearch
	 * flag as 1 for P2P Client Mode
	 */
	if (QDF_P2P_CLIENT_MODE == profile->csrPersona)
		scan_req->p2pSearch = 1;

	/* Allocate memory for IE field */
	if (profile->pAddIEScan) {
		scan_req->pIEField =
			qdf_mem_malloc(profile->nAddIEScanLength);

		if (NULL == scan_req->pIEField)
			status = QDF_STATUS_E_NOMEM;
		else
			status = QDF_STATUS_SUCCESS;

		if (QDF_IS_STATUS_SUCCESS(status)) {
			qdf_mem_copy(scan_req->pIEField,
					profile->pAddIEScan,
					profile->nAddIEScanLength);
			scan_req->uIEFieldLen = profile->nAddIEScanLength;
		} else {
			sme_err("No memory for scanning IE fields");
		}
	} else
		scan_req->uIEFieldLen = 0;

	/*
	 * For one channel be good enpugh time to receive beacon
	 * atleast
	 */
	if (1 == profile->ChannelInfo.numOfChannels) {
		if (neighbor_roaminfo->handoffReqInfo.src ==
					FASTREASSOC) {
			scan_req->maxChnTime =
				MAX_ACTIVE_SCAN_FOR_ONE_CHANNEL_FASTREASSOC;
			scan_req->minChnTime =
				MIN_ACTIVE_SCAN_FOR_ONE_CHANNEL_FASTREASSOC;
			/* Reset this value */
			neighbor_roaminfo->handoffReqInfo.src = 0;
		} else {
			scan_req->maxChnTime =
					MAX_ACTIVE_SCAN_FOR_ONE_CHANNEL;
			scan_req->minChnTime =
					MIN_ACTIVE_SCAN_FOR_ONE_CHANNEL;
		}
	} else {
		scan_req->maxChnTime =
			mac_ctx->roam.configParam.nActiveMaxChnTime;
		scan_req->minChnTime =
			mac_ctx->roam.configParam.nActiveMinChnTime;
	}

	if (profile->BSSIDs.numOfBSSIDs == 1)
		qdf_copy_macaddr(&scan_req->bssid,
				profile->BSSIDs.bssid);
	else
		qdf_set_macaddr_broadcast(&scan_req->bssid);

	if (profile->ChannelInfo.numOfChannels) {
		scan_req->ChannelInfo.ChannelList =
		    qdf_mem_malloc(sizeof(*scan_req->ChannelInfo.ChannelList) *
					profile->ChannelInfo.numOfChannels);

		if (NULL == scan_req->ChannelInfo.ChannelList)
			status = QDF_STATUS_E_NOMEM;
		else
			status = QDF_STATUS_SUCCESS;

		scan_req->ChannelInfo.numOfChannels = 0;

		if (QDF_IS_STATUS_SUCCESS(status)) {
			csr_roam_is_channel_valid(mac_ctx,
				profile->ChannelInfo.ChannelList[0]);
			csr_roam_copy_channellist(mac_ctx,
				profile, scan_cmd, index);
		} else {
			goto error;
		}
	} else {
		scan_req->ChannelInfo.numOfChannels = 0;
	}

	if (profile->SSIDs.numOfSSIDs) {
		scan_req->SSIDs.SSIDList =
			qdf_mem_malloc(profile->SSIDs.numOfSSIDs *
					sizeof(tCsrSSIDInfo));

		if (NULL == scan_req->SSIDs.SSIDList)
			status = QDF_STATUS_E_NOMEM;
		else
			status = QDF_STATUS_SUCCESS;

		if (!QDF_IS_STATUS_SUCCESS(status))
			goto error;

		ssids = &scan_req->SSIDs;
		ssids->numOfSSIDs =  1;

		qdf_mem_copy(scan_req->SSIDs.SSIDList,
				profile->SSIDs.SSIDList,
				sizeof(tCsrSSIDInfo));
	}

	/* Start process the command */
	status = csr_queue_sme_command(mac_ctx, scan_cmd, false);
error:
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_err("failed to initiate scan with status: %d", status);
		if (scan_cmd)
			csr_release_command_scan(mac_ctx, scan_cmd);
		if (notify)
			csr_roam_call_callback(mac_ctx, session_id, NULL,
					roam_id, eCSR_ROAM_FAILED,
					eCSR_ROAM_RESULT_FAILURE);
	}
	return status;
}

static void csr_set_cfg_valid_channel_list(tpAniSirGlobal pMac,
				uint8_t *pChannelList, uint8_t NumChannels)
{
	uint32_t dataLen = sizeof(uint8_t) * NumChannels;
	QDF_STATUS status;

	QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
		  "%s: dump valid channel list(NumChannels(%d))",
		  __func__, NumChannels);
	QDF_TRACE_HEX_DUMP(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
			   pChannelList, NumChannels);
	cfg_set_str(pMac, WNI_CFG_VALID_CHANNEL_LIST, pChannelList,
			dataLen);

	QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
		  "Scan offload is enabled, update default chan list");
	/*
	 * disable fcc constraint since new country code
	 * is being set
	 */
	pMac->scan.fcc_constraint = false;
	status = csr_update_channel_list(pMac);
	if (QDF_STATUS_SUCCESS != status) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
			  "failed to update the supported channel list");
	}
}

/*
 * The Tx power limits are saved in the cfg for future usage.
 */
static void csr_save_tx_power_to_cfg(tpAniSirGlobal pMac, tDblLinkList *pList,
			      uint32_t cfgId)
{
	tListElem *pEntry;
	uint32_t cbLen = 0, dataLen, tmp_len;
	tCsrChannelPowerInfo *ch_set;
	uint32_t idx;
	tSirMacChanInfo *ch_pwr_set;
	uint8_t *pBuf = NULL;

	/* allocate maximum space for all channels */
	dataLen = WNI_CFG_VALID_CHANNEL_LIST_LEN * sizeof(tSirMacChanInfo);
	pBuf = qdf_mem_malloc(dataLen);
	if (pBuf == NULL)
		return;

	ch_pwr_set = (tSirMacChanInfo *) (pBuf);
	pEntry = csr_ll_peek_head(pList, LL_ACCESS_LOCK);
	/*
	 * write the tuples (startChan, numChan, txPower) for each channel found
	 * in the channel power list.
	 */
	while (pEntry) {
		ch_set = GET_BASE_ADDR(pEntry, tCsrChannelPowerInfo, link);
		if (1 != ch_set->interChannelOffset) {
			/*
			 * we keep the 5G channel sets internally with an
			 * interchannel offset of 4. Expand these to the right
			 * format. (inter channel offset of 1 is the only option
			 * for the triplets that 11d advertises.
			 */
			tmp_len = cbLen + (ch_set->numChannels *
						sizeof(tSirMacChanInfo));
			if (tmp_len >= dataLen) {
				/*
				 * expanding this entry will overflow our
				 * allocation
				 */
				sme_err(
					"Buffer overflow, start %d, num %d, offset %d",
					ch_set->firstChannel,
					ch_set->numChannels,
					ch_set->interChannelOffset);
				break;
			}

			for (idx = 0; idx < ch_set->numChannels; idx++) {
				ch_pwr_set->firstChanNum = (tSirMacChanNum)
					(ch_set->firstChannel + (idx *
						ch_set->interChannelOffset));
				sme_debug(
					"Setting Channel Number %d",
					ch_pwr_set->firstChanNum);
				ch_pwr_set->numChannels = 1;
				ch_pwr_set->maxTxPower =
					QDF_MIN(ch_set->txPower,
					pMac->roam.configParam.nTxPowerCap);
				sme_debug(
					"Setting Max Transmit Power %d",
					ch_pwr_set->maxTxPower);
				cbLen += sizeof(tSirMacChanInfo);
				ch_pwr_set++;
			}
		} else {
			if (cbLen >= dataLen) {
				/* this entry will overflow our allocation */
				sme_err(
					"Buffer overflow, start %d, num %d, offset %d",
					ch_set->firstChannel,
					ch_set->numChannels,
					ch_set->interChannelOffset);
				break;
			}
			ch_pwr_set->firstChanNum = ch_set->firstChannel;
			sme_debug("Setting Channel Number %d",
				ch_pwr_set->firstChanNum);
			ch_pwr_set->numChannels = ch_set->numChannels;
			ch_pwr_set->maxTxPower = QDF_MIN(ch_set->txPower,
					pMac->roam.configParam.nTxPowerCap);
			sme_debug(
				"Setting Max Tx Power %d, nTxPower %d",
				ch_pwr_set->maxTxPower,
				pMac->roam.configParam.nTxPowerCap);
			cbLen += sizeof(tSirMacChanInfo);
			ch_pwr_set++;
		}
		pEntry = csr_ll_next(pList, pEntry, LL_ACCESS_LOCK);
	}
	if (cbLen)
		cfg_set_str(pMac, cfgId, (uint8_t *) pBuf, cbLen);

	qdf_mem_free(pBuf);
}

static void csr_set_cfg_country_code(tpAniSirGlobal pMac, uint8_t *countryCode)
{
	uint8_t cc[WNI_CFG_COUNTRY_CODE_LEN];
	/* v_REGDOMAIN_t DomainId */

	sme_debug("Setting Country Code in Cfg %s", countryCode);
	qdf_mem_copy(cc, countryCode, WNI_CFG_COUNTRY_CODE_LEN);

	/*
	 * Don't program the bogus country codes that we created for Korea in
	 * the MAC. if we see the bogus country codes, program the MAC with
	 * the right country code.
	 */
	if (('K' == countryCode[0] && '1' == countryCode[1]) ||
	    ('K' == countryCode[0] && '2' == countryCode[1]) ||
	    ('K' == countryCode[0] && '3' == countryCode[1]) ||
	    ('K' == countryCode[0] && '4' == countryCode[1])) {
		/*
		 * replace the alternate Korea country codes, 'K1', 'K2', ..
		 * with 'KR' for Korea
		 */
		cc[1] = 'R';
	}
	cfg_set_str(pMac, WNI_CFG_COUNTRY_CODE, cc, WNI_CFG_COUNTRY_CODE_LEN);

	/*
	 * Need to let HALPHY know about the current domain so it can apply some
	 * domain-specific settings (TX filter...)
	 */
}

QDF_STATUS csr_get_country_code(tpAniSirGlobal pMac, uint8_t *pBuf,
				uint8_t *pbLen)
{
	tSirRetStatus status;
	uint32_t len;

	if (pBuf && pbLen && (*pbLen >= WNI_CFG_COUNTRY_CODE_LEN)) {
		len = *pbLen;
		status = wlan_cfg_get_str(pMac, WNI_CFG_COUNTRY_CODE, pBuf,
					&len);
		if (status == eSIR_SUCCESS) {
			*pbLen = (uint8_t) len;
			return QDF_STATUS_SUCCESS;
		}
	}

	return QDF_STATUS_E_INVAL;
}

void csr_set_cfg_scan_control_list(tpAniSirGlobal pMac, uint8_t *countryCode,
				   tCsrChannel *pChannelList)
{
	uint8_t i, j;
	bool found = false;
	uint8_t *pControlList = NULL;
	uint32_t len = WNI_CFG_SCAN_CONTROL_LIST_LEN;

	pControlList = qdf_mem_malloc(WNI_CFG_SCAN_CONTROL_LIST_LEN);
	if (pControlList != NULL) {
		if (IS_SIR_STATUS_SUCCESS(wlan_cfg_get_str(pMac,
					WNI_CFG_SCAN_CONTROL_LIST,
					pControlList, &len))) {
			for (i = 0; i < pChannelList->numChannels; i++) {
				for (j = 0; j < len; j += 2) {
					if (pControlList[j] ==
					    pChannelList->channelList[i]) {
						found = true;
						break;
					}
				}

				if (found) {
					/* insert a pair(channel#, flag) */
					pControlList[j + 1] =
						csr_get_scan_type(pMac,
							pControlList[j]);
					found = false;  /* reset the flag */
				}

			}
			QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_DEBUG,
				  "%s: dump scan control list", __func__);
			QDF_TRACE_HEX_DUMP(QDF_MODULE_ID_SME,
					   QDF_TRACE_LEVEL_DEBUG, pControlList,
					   len);

			cfg_set_str(pMac, WNI_CFG_SCAN_CONTROL_LIST,
					pControlList, len);
		} /* Successfully getting scan control list */
		qdf_mem_free(pControlList);
	} /* AllocateMemory */
}


/**
 * csr_scan_abort_all_scans() - Abort scan on all Sessions
 * @mac_ctx: pointer to Global Mac structure
 * @reason: reason for cancelling scan
 *
 * Abort scan on all Sessions
 *
 * Return: QDF_STATUS
 */
QDF_STATUS csr_scan_abort_all_scans(tpAniSirGlobal mac_ctx,
				   eCsrAbortReason reason)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	uint8_t session_id;

	mac_ctx->scan.fDropScanCmd = true;
	for (session_id = 0; session_id < CSR_ROAM_SESSION_MAX; session_id++) {
		if (CSR_IS_SESSION_VALID(mac_ctx, session_id)) {
			csr_remove_cmd_from_pending_list(
				mac_ctx,
				session_id, INVALID_SCAN_ID,
				&mac_ctx->sme.smeScanCmdPendingList,
				eSmeCommandScan);
			csr_abort_scan_from_active_list(mac_ctx,
				 &mac_ctx->sme.smeScanCmdActiveList,
				 session_id, INVALID_SCAN_ID, eSmeCommandScan,
				 reason);
		}
	}
	mac_ctx->scan.fDropScanCmd = false;

	return status;
}

QDF_STATUS csr_scan_abort_mac_scan(tpAniSirGlobal pMac, uint8_t sessionId,
				   uint32_t scan_id, eCsrAbortReason reason)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	QDF_STATUS ret;

	pMac->scan.fDropScanCmd = true;
	ret = csr_remove_cmd_from_pending_list(pMac,
			sessionId, scan_id, &pMac->sme.smeScanCmdPendingList,
			eSmeCommandScan);
	pMac->scan.fDropScanCmd = false;

	/*
	 * If we are not able to find command for scan id in
	 * pending list, check active list. Also if the session
	 * id is valid then we have to check below active list.
	 */
	if (ret != QDF_STATUS_SUCCESS ||
			sessionId != CSR_SESSION_ID_INVALID) {
		status = csr_abort_scan_from_active_list(pMac,
			&pMac->sme.smeScanCmdActiveList, sessionId, scan_id,
			eSmeCommandScan, reason);
	}
	return status;
}

QDF_STATUS csr_remove_cmd_from_pending_list(tpAniSirGlobal pMac,
						uint8_t sessionId,
						uint32_t scan_id,
						tDblLinkList *pList,
						eSmeCommandType commandType)
{
	tDblLinkList localList;
	tListElem *pEntry;
	tSmeCmd *pCommand;
	tListElem *pEntryToRemove;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;

	qdf_mem_zero(&localList, sizeof(tDblLinkList));
	if (!QDF_IS_STATUS_SUCCESS(csr_ll_open(pMac->hHdd, &localList))) {
		sme_err("failed to open list");
		return status;
	}

	csr_ll_lock(pList);
	pEntry = csr_ll_peek_head(pList, LL_ACCESS_NOLOCK);

	/*
	 * Have to make sure we don't loop back to the head of the list,
	 * which will happen if the entry is NOT on the list
	 */
	while (pEntry) {
		pEntryToRemove = pEntry;
		pEntry = csr_ll_next(pList, pEntry, LL_ACCESS_NOLOCK);
		pCommand = GET_BASE_ADDR(pEntryToRemove, tSmeCmd, Link);

		if ((pCommand->command == commandType) &&
		    (((commandType == eSmeCommandScan) &&
		    (pCommand->u.scanCmd.scanID == scan_id)) ||
		    (pCommand->sessionId == sessionId))) {
			/* Remove that entry only */
			if (csr_ll_remove_entry(pList, pEntryToRemove,
			    LL_ACCESS_NOLOCK)) {
				csr_ll_insert_tail(&localList, pEntryToRemove,
						   LL_ACCESS_NOLOCK);
				status = QDF_STATUS_SUCCESS;
			}
		}
	}
	csr_ll_unlock(pList);

	while ((pEntry = csr_ll_remove_head(&localList, LL_ACCESS_NOLOCK))) {
		pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
		sme_debug("Sending abort for command ID %d",
			(commandType == eSmeCommandScan) ? pCommand->u.
			scanCmd.scanID : sessionId);
		csr_abort_command(pMac, pCommand, false);
	}

	csr_ll_close(&localList);
	return status;
}

QDF_STATUS csr_scan_abort_scan_for_ssid(tpAniSirGlobal pMac, uint32_t sessionId)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	pMac->scan.fDropScanCmd = true;
	csr_remove_scan_for_ssid_from_pending_list(pMac,
			&pMac->sme.smeScanCmdPendingList, sessionId);
	pMac->scan.fDropScanCmd = false;
	csr_abort_scan_from_active_list(pMac, &pMac->sme.smeScanCmdActiveList,
			sessionId, INVALID_SCAN_ID, eSmeCommandScan,
			eCSR_SCAN_ABORT_SSID_ONLY);
	return status;
}

void csr_remove_scan_for_ssid_from_pending_list(tpAniSirGlobal pMac,
						tDblLinkList *pList,
						uint32_t sessionId)
{
	tDblLinkList localList;
	tListElem *pEntry;
	tSmeCmd *pCommand;
	tListElem *pEntryToRemove;

	qdf_mem_zero(&localList, sizeof(tDblLinkList));
	if (!QDF_IS_STATUS_SUCCESS(csr_ll_open(pMac->hHdd, &localList))) {
		sme_err("failed to open list");
		return;
	}
	csr_ll_lock(pList);
	if (!csr_ll_is_list_empty(pList, LL_ACCESS_NOLOCK)) {
		pEntry = csr_ll_peek_head(pList, LL_ACCESS_NOLOCK);
		/*
		 * Have to make sure we don't loop back to the head of the list,
		 * which will happen if the entry is NOT on the list...
		 */
		while (pEntry) {
			pEntryToRemove = pEntry;
			pEntry = csr_ll_next(pList, pEntry, LL_ACCESS_NOLOCK);
			pCommand = GET_BASE_ADDR(pEntryToRemove, tSmeCmd, Link);

			if (!((eSmeCommandScan == pCommand->command) &&
			    (sessionId == pCommand->sessionId)))
				continue;
			if (eCsrScanForSsid != pCommand->u.scanCmd.reason)
				continue;
			/* Remove that entry only */
			if (csr_ll_remove_entry(pList, pEntryToRemove,
						LL_ACCESS_NOLOCK)) {
				csr_ll_insert_tail(&localList, pEntryToRemove,
						   LL_ACCESS_NOLOCK);
			}
		}
	}
	csr_ll_unlock(pList);
	while ((pEntry = csr_ll_remove_head(&localList, LL_ACCESS_NOLOCK))) {
		pCommand = GET_BASE_ADDR(pEntry, tSmeCmd, Link);
		csr_abort_command(pMac, pCommand, false);
	}
	csr_ll_close(&localList);
}


/**
 * csr_send_scan_abort() -  Sends scan abort command to firmware
 * @mac_ctx: Pointer to Global Mac structure
 * @session_id: CSR session identification
 * @scan_id: scan identifier
 *
 * .Sends scan abort command to firmware
 *
 * Return: None
 */
static void csr_send_scan_abort(tpAniSirGlobal mac_ctx,
	uint32_t session_id, uint32_t scan_id)
{
	tSirSmeScanAbortReq *msg;
	uint16_t msg_len;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	msg_len = (uint16_t)(sizeof(tSirSmeScanAbortReq));
	msg = qdf_mem_malloc(msg_len);
	if (NULL == msg) {
		sme_err("Failed to alloc memory for SmeScanAbortReq");
		return;
	}
	msg->type = eWNI_SME_SCAN_ABORT_IND;
	msg->msgLen = msg_len;
	msg->sessionId = session_id;
	msg->scan_id = scan_id;
	sme_debug(
		"Abort scan sent to Firmware scan_id %d session %d",
		scan_id, session_id);
	status = cds_send_mb_message_to_mac(msg);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		sme_err("Failed to send abort scan.scan_id %d session %d",
			scan_id, session_id);
	}
}

/**
 * csr_abort_scan_from_active_list() -  Remove Scan command from active list
 * @mac_ctx: Pointer to Global Mac structure
 * @list: pointer to scan active list
 * @session_id: CSR session identification
 * @scan_id: scan id
 * @scan_cmd_type: scan command type
 * @abort_reason: abort reason
 *
 * Remove Scan command from active scan list by matching either the scan id
 * or session id.
 *
 * Return: Success - QDF_STATUS_SUCCESS, Failure - error number
 */
QDF_STATUS csr_abort_scan_from_active_list(tpAniSirGlobal mac_ctx,
		tDblLinkList *list, uint32_t session_id, uint32_t scan_id,
		eSmeCommandType scan_cmd_type, eCsrAbortReason abort_reason)
{
	tListElem *entry;
	tSmeCmd *cmd;
	tListElem *entry_remove;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	csr_ll_lock(list);
	if (!csr_ll_is_list_empty(list, LL_ACCESS_NOLOCK)) {
		entry = csr_ll_peek_head(list, LL_ACCESS_NOLOCK);
		while (entry) {
			entry_remove = entry;
			entry = csr_ll_next(list, entry, LL_ACCESS_NOLOCK);
			cmd = GET_BASE_ADDR(entry_remove, tSmeCmd, Link);

			/*skip if abort reason is for SSID*/
			if ((abort_reason == eCSR_SCAN_ABORT_SSID_ONLY) &&
				(eCsrScanForSsid != cmd->u.scanCmd.reason))
				continue;
			/*
			 * Do not skip if command and either session id
			 * or scan id is matched
			 */
			if ((cmd->command == scan_cmd_type) &&
			    ((cmd->u.scanCmd.scanID == scan_id) ||
			    (cmd->sessionId == session_id))) {
					cmd->u.scanCmd.abort_scan_indication =
						abort_reason;

				csr_send_scan_abort(mac_ctx, cmd->sessionId,
						    cmd->u.scanCmd.scanID);

			}
		}
	}
	csr_ll_unlock(list);

	return status;
}

QDF_STATUS csr_scan_abort_mac_scan_not_for_connect(tpAniSirGlobal pMac,
						   uint8_t sessionId)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!csr_is_scan_for_roam_command_active(pMac))
		/*
		 * Only abort the scan if it is not used for other roam/connect
		 * purpose
		 */
		status = csr_scan_abort_mac_scan(pMac, sessionId,
				INVALID_SCAN_ID, eCSR_SCAN_ABORT_DEFAULT);
	return status;
}

static bool csr_roam_is_valid_channel(tpAniSirGlobal pMac, uint8_t channel)
{
	bool fValid = false;
	uint32_t idx_valid_ch;
	uint32_t len = pMac->roam.numValidChannels;

	for (idx_valid_ch = 0; (idx_valid_ch < len); idx_valid_ch++) {
		if (channel == pMac->roam.validChannelList[idx_valid_ch]) {
			fValid = true;
			break;
		}
	}
	return fValid;
}

#ifdef FEATURE_WLAN_SCAN_PNO
QDF_STATUS csr_scan_save_preferred_network_found(tpAniSirGlobal pMac,
						 tSirPrefNetworkFoundInd *
						 pPrefNetworkFoundInd)
{
	uint32_t uLen = 0;
	tpSirProbeRespBeacon parsed_frm;
	struct tag_csrscan_result *pScanResult = NULL;
	tSirBssDescription *pBssDescr = NULL;
	bool fDupBss;
	tDot11fBeaconIEs *local_ie = NULL;
	tAniSSID tmpSsid;
	unsigned long timer = 0;
	QDF_STATUS status;

	tpSirMacMgmtHdr macHeader =
		(tpSirMacMgmtHdr) pPrefNetworkFoundInd->data;
	parsed_frm =
	    (tpSirProbeRespBeacon) qdf_mem_malloc(sizeof(tSirProbeRespBeacon));

	if (NULL == parsed_frm) {
		sme_err("fail to allocate memory for frame");
		return QDF_STATUS_E_NOMEM;
	}
	if (pPrefNetworkFoundInd->frameLength <= SIR_MAC_HDR_LEN_3A) {
		sme_err("Incorrect len: %d",
			pPrefNetworkFoundInd->frameLength);
		qdf_mem_free(parsed_frm);
		return QDF_STATUS_E_FAILURE;
	}
	if (sir_convert_probe_frame2_struct(pMac,
		&pPrefNetworkFoundInd->data[SIR_MAC_HDR_LEN_3A],
		pPrefNetworkFoundInd->frameLength - SIR_MAC_HDR_LEN_3A,
		parsed_frm) != eSIR_SUCCESS
	    || !parsed_frm->ssidPresent) {
		sme_err("Parse error ProbeResponse, length: %d",
			pPrefNetworkFoundInd->frameLength);
		qdf_mem_free(parsed_frm);
		return QDF_STATUS_E_FAILURE;
	}
	/* 24 byte MAC header and 12 byte to ssid IE */
	if (pPrefNetworkFoundInd->frameLength >
	    (SIR_MAC_HDR_LEN_3A + SIR_MAC_B_PR_SSID_OFFSET)) {
		uLen = pPrefNetworkFoundInd->frameLength -
		       (SIR_MAC_HDR_LEN_3A + SIR_MAC_B_PR_SSID_OFFSET);
	}
	pScanResult = qdf_mem_malloc(sizeof(struct tag_csrscan_result) + uLen);
	if (NULL == pScanResult) {
		sme_err("fail to allocate memory for frame");
		qdf_mem_free(parsed_frm);
		return QDF_STATUS_E_NOMEM;
	}
	pBssDescr = &pScanResult->Result.BssDescriptor;
	/*
	 * Length of BSS desription is without length of length itself and
	 * length of pointer that holds the next BSS description
	 */
	pBssDescr->length = (uint16_t)(offsetof(tSirBssDescription, ieFields[0])
					- sizeof(pBssDescr->length) + uLen);
	if (parsed_frm->dsParamsPresent)
		pBssDescr->channelId = parsed_frm->channelNumber;
	else if (parsed_frm->HTInfo.present)
		pBssDescr->channelId = parsed_frm->HTInfo.primaryChannel;
	else {
		/*
		 * If Probe Responce received in PNO indication does not
		 * contain DSParam IE or HT Info IE then add dummy channel
		 * to the received BSS info so that Scan result received as
		 * a part of PNO is updated to the supplicant. Specially
		 * applicable in case of AP configured in 11A only mode.
		 */
		if ((pMac->roam.configParam.bandCapability == eCSR_BAND_ALL) ||
			(pMac->roam.configParam.bandCapability == eCSR_BAND_24))
			pBssDescr->channelId = 1;
		else if (pMac->roam.configParam.bandCapability == eCSR_BAND_5G)
			pBssDescr->channelId = 36;
	}
	if ((pBssDescr->channelId > 0) && (pBssDescr->channelId < 15)) {
		int i;
		/* 11b or 11g packet */
		/* 11g iff extended Rate IE is present or */
		/* if there is an A rate in suppRate IE */
		for (i = 0; i < parsed_frm->supportedRates.numRates; i++) {
			if (sirIsArate(parsed_frm->supportedRates.rate[i]
				       & 0x7f)) {
				pBssDescr->nwType = eSIR_11G_NW_TYPE;
				break;
			}
		}
		if (parsed_frm->extendedRatesPresent)
			pBssDescr->nwType = eSIR_11G_NW_TYPE;
	} else {
		/* 11a packet */
		pBssDescr->nwType = eSIR_11A_NW_TYPE;
	}
	pBssDescr->sinr = 0;
	pBssDescr->rssi = -1 * pPrefNetworkFoundInd->rssi;
	pBssDescr->beaconInterval = parsed_frm->beaconInterval;
	if (!pBssDescr->beaconInterval) {
		sme_warn("Bcn Interval is Zero , default to 100"
			MAC_ADDRESS_STR, MAC_ADDR_ARRAY(pBssDescr->bssId));
		pBssDescr->beaconInterval = 100;
	}
	pBssDescr->timeStamp[0] = parsed_frm->timeStamp[0];
	pBssDescr->timeStamp[1] = parsed_frm->timeStamp[1];
	pBssDescr->capabilityInfo = *((uint16_t *)&parsed_frm->capabilityInfo);
	qdf_mem_copy((uint8_t *) &pBssDescr->bssId,
		     (uint8_t *) macHeader->bssId, sizeof(tSirMacAddr));
	pBssDescr->received_time = (uint64_t)qdf_mc_timer_get_system_time();

	/* MobilityDomain */
	pBssDescr->mdie[0] = 0;
	pBssDescr->mdie[1] = 0;
	pBssDescr->mdie[2] = 0;
	pBssDescr->mdiePresent = false;
	/*
	 * If mdie is present in the probe rsp we fill it in the bss description
	 */
	if (parsed_frm->mdiePresent) {
		pBssDescr->mdiePresent = true;
		pBssDescr->mdie[0] = parsed_frm->mdie[0];
		pBssDescr->mdie[1] = parsed_frm->mdie[1];
		pBssDescr->mdie[2] = parsed_frm->mdie[2];
	}
	sme_debug("mdie=%02x%02x%02x",
		(unsigned int)pBssDescr->mdie[0],
		(unsigned int)pBssDescr->mdie[1],
		(unsigned int)pBssDescr->mdie[2]);

	sme_debug("Bssid= "MAC_ADDRESS_STR" chan= %d, rssi = %d",
		MAC_ADDR_ARRAY(pBssDescr->bssId), pBssDescr->channelId,
		pBssDescr->rssi);
	/* IEs */
	if (uLen) {
		qdf_mem_copy(&pBssDescr->ieFields,
			pPrefNetworkFoundInd->data + (SIR_MAC_HDR_LEN_3A +
			SIR_MAC_B_PR_SSID_OFFSET), uLen);
	}
	local_ie = (tDot11fBeaconIEs *) (pScanResult->Result.pvIes);
	if (NULL == local_ie) {
		status = csr_get_parsed_bss_description_ies(pMac,
				&pScanResult->Result.BssDescriptor, &local_ie);

		if (!(local_ie || QDF_IS_STATUS_SUCCESS(status))) {
			sme_err("Cannot parse IEs");
			csr_free_scan_result_entry(pMac, pScanResult);
			qdf_mem_free(parsed_frm);
			return QDF_STATUS_E_RESOURCES;
		}
	}

	fDupBss = csr_remove_dup_bss_description(pMac,
			&pScanResult->Result.BssDescriptor,
			&tmpSsid, &timer);
	/* Check whether we have reach out limit */
	if (CSR_SCAN_IS_OVER_BSS_LIMIT(pMac)) {
		/* Limit reach */
		sme_err("BSS limit reached");
		/* Free the resources */
		if ((pScanResult->Result.pvIes == NULL) && local_ie)
			qdf_mem_free(local_ie);
		csr_free_scan_result_entry(pMac, pScanResult);
		qdf_mem_free(parsed_frm);
		return QDF_STATUS_E_RESOURCES;
	}

	if ((SIR_MAC_MGMT_FRAME == macHeader->fc.type) &&
	    (SIR_MAC_MGMT_PROBE_RSP == macHeader->fc.subType))
		pScanResult->Result.BssDescriptor.fProbeRsp = 1;

	/* Add to scan cache */
	csr_scan_add_result(pMac, pScanResult, local_ie,
			    pPrefNetworkFoundInd->sessionId);

	if ((pScanResult->Result.pvIes == NULL) && local_ie)
		qdf_mem_free(local_ie);
	qdf_mem_free(parsed_frm);
	return QDF_STATUS_SUCCESS;
}
#endif /* FEATURE_WLAN_SCAN_PNO */

void csr_init_occupied_channels_list(tpAniSirGlobal pMac, uint8_t sessionId)
{
	tListElem *pEntry = NULL;
	struct tag_csrscan_result *pBssDesc = NULL;
	tDot11fBeaconIEs *pIes = NULL;
	tpCsrNeighborRoamControlInfo pNeighborRoamInfo =
		&pMac->roam.neighborRoamInfo[sessionId];

	if (0 != pNeighborRoamInfo->cfgParams.channelInfo.numOfChannels) {
		/*
		 * Ini file contains neighbor scan channel list, hence NO need
		 * to build occupied channel list"
		 */
		sme_debug("Ini contains neighbor scan ch list");
		return;
	}

	if (!csr_neighbor_roam_is_new_connected_profile(pMac, sessionId)) {
		/*
		 * Do not flush occupied list since current roam profile matches
		 * previous
		 */
		sme_debug("Current roam profile matches prev");
		return;
	}

	/* Empty occupied channels here */
	pMac->scan.occupiedChannels[sessionId].numChannels = 0;
	pMac->scan.roam_candidate_count[sessionId] = 0;

	csr_ll_lock(&pMac->scan.scanResultList);
	pEntry = csr_ll_peek_head(&pMac->scan.scanResultList, LL_ACCESS_NOLOCK);
	while (pEntry) {
		pBssDesc = GET_BASE_ADDR(pEntry, struct tag_csrscan_result,
					Link);
		pIes = (tDot11fBeaconIEs *) (pBssDesc->Result.pvIes);
		/* At this time, pBssDescription->Result.pvIes may be NULL */
		if (!pIes && !QDF_IS_STATUS_SUCCESS(
			csr_get_parsed_bss_description_ies(pMac,
				&pBssDesc->Result.BssDescriptor, &pIes))) {
			/* Pick next bss entry before continuing */
			pEntry = csr_ll_next(&pMac->scan.scanResultList, pEntry,
				     LL_ACCESS_NOLOCK);
			continue;
		}
		csr_scan_add_to_occupied_channels(pMac, pBssDesc, sessionId,
				&pMac->scan.occupiedChannels[sessionId], pIes,
				true);
		/*
		 * Free the memory allocated for pIes in
		 * csr_get_parsed_bss_description_ies
		 */
		if ((pBssDesc->Result.pvIes == NULL) && pIes)
			qdf_mem_free(pIes);
		pEntry = csr_ll_next(&pMac->scan.scanResultList, pEntry,
				     LL_ACCESS_NOLOCK);
	} /* while */
	csr_ll_unlock(&pMac->scan.scanResultList);
}

QDF_STATUS csr_scan_create_entry_in_scan_cache(tpAniSirGlobal pMac,
					       uint32_t sessionId,
					       struct qdf_mac_addr bssid,
					       uint8_t channel)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	tDot11fBeaconIEs *pNewIes = NULL;
	tCsrRoamSession *pSession = CSR_GET_SESSION(pMac, sessionId);
	tSirBssDescription *pNewBssDescriptor = NULL;
	uint32_t size = 0;

	if (NULL == pSession) {
		status = QDF_STATUS_E_FAILURE;
		return status;
	}
	sme_debug("Current bssid::"MAC_ADDRESS_STR,
		MAC_ADDR_ARRAY(pSession->pConnectBssDesc->bssId));
	sme_debug("My bssid::"MAC_ADDRESS_STR" channel %d",
		MAC_ADDR_ARRAY(bssid.bytes), channel);

	if (!QDF_IS_STATUS_SUCCESS(csr_get_parsed_bss_description_ies(
					pMac, pSession->pConnectBssDesc,
					&pNewIes))) {
		sme_err("Failed to parse IEs");
		status = QDF_STATUS_E_FAILURE;
		goto free_mem;
	}
	size = pSession->pConnectBssDesc->length +
		sizeof(pSession->pConnectBssDesc->length);
	if (!size) {
		sme_err("length of bss descriptor is 0");
		status = QDF_STATUS_E_FAILURE;
		goto free_mem;
	}
	pNewBssDescriptor = qdf_mem_malloc(size);
	if (NULL == pNewBssDescriptor) {
		sme_err("memory allocation failed");
		status = QDF_STATUS_E_FAILURE;
		goto free_mem;
	}
	qdf_mem_copy(pNewBssDescriptor, pSession->pConnectBssDesc, size);
	/* change the BSSID & channel as passed */
	qdf_mem_copy(pNewBssDescriptor->bssId, bssid.bytes,
			sizeof(tSirMacAddr));
	pNewBssDescriptor->channelId = channel;
	if (NULL == csr_scan_append_bss_description(pMac, pNewBssDescriptor,
						pNewIes, sessionId)) {
		sme_err("csr_scan_append_bss_description failed");
		status = QDF_STATUS_E_FAILURE;
		goto free_mem;
	}
	sme_err("entry successfully added in scan cache");

free_mem:
	if (pNewIes)
		qdf_mem_free(pNewIes);

	if (pNewBssDescriptor)
		qdf_mem_free(pNewBssDescriptor);

	return status;
}

#ifdef FEATURE_WLAN_ESE
/*  Update the TSF with the difference in system time */
void update_cckmtsf(uint32_t *timeStamp0, uint32_t *timeStamp1,
		    uint64_t *incr)
{
	uint64_t timeStamp64 = ((uint64_t) *timeStamp1 << 32) | (*timeStamp0);

	timeStamp64 = (uint64_t)(timeStamp64 + (*incr));
	*timeStamp0 = (uint32_t) (timeStamp64 & 0xffffffff);
	*timeStamp1 = (uint32_t) ((timeStamp64 >> 32) & 0xffffffff);
}
#endif

/**
 * csr_scan_save_roam_offload_ap_to_scan_cache
 * This function parses the received beacon/probe response
 * from the firmware as part of the roam synch indication.
 * The beacon or the probe response is parsed and is also
 * saved into the scan cache
 *
 * @param  pMac Pointer to Global Mac
 * @param  roam_sync_ind_ptr Roam Synch Indication from
 *         firmware which also contains the beacon/probe
 *         response
 * @return Status
 */
#ifdef WLAN_FEATURE_ROAM_OFFLOAD
QDF_STATUS csr_scan_save_roam_offload_ap_to_scan_cache(tpAniSirGlobal pMac,
		roam_offload_synch_ind *roam_sync_ind_ptr,
		tpSirBssDescription  bss_desc_ptr)
{
	uint32_t length = 0;
	bool dup_bss;
	tDot11fBeaconIEs *ies_local_ptr = NULL;
	tAniSSID tmpSsid;
	unsigned long timer = 0;
	struct tag_csrscan_result *scan_res_ptr = NULL;
	uint8_t session_id = roam_sync_ind_ptr->roamedVdevId;

	length = roam_sync_ind_ptr->beaconProbeRespLength -
		(SIR_MAC_HDR_LEN_3A + SIR_MAC_B_PR_SSID_OFFSET);
	scan_res_ptr = qdf_mem_malloc(sizeof(struct tag_csrscan_result) +
				length);
	if (scan_res_ptr == NULL) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
				" fail to allocate memory for frame");
		return QDF_STATUS_E_NOMEM;
	}

	qdf_mem_copy(&scan_res_ptr->Result.BssDescriptor,
			bss_desc_ptr,
			(sizeof(tSirBssDescription) + length));
	ies_local_ptr = (tDot11fBeaconIEs *)(scan_res_ptr->Result.pvIes);
	if (!ies_local_ptr &&
		(!QDF_IS_STATUS_SUCCESS(csr_get_parsed_bss_description_ies(
						pMac, &scan_res_ptr->Result.
						BssDescriptor,
						&ies_local_ptr)))) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
				"%s:Cannot Parse IEs", __func__);
		csr_free_scan_result_entry(pMac, scan_res_ptr);
		return QDF_STATUS_E_RESOURCES;
	}

	dup_bss = csr_remove_dup_bss_description(pMac,
			&scan_res_ptr->Result.BssDescriptor,
			&tmpSsid, &timer);
	if (CSR_SCAN_IS_OVER_BSS_LIMIT(pMac)) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
				"%s:BSS Limit Exceed", __func__);
		if ((scan_res_ptr->Result.pvIes == NULL) && ies_local_ptr)
			qdf_mem_free(ies_local_ptr);

		csr_free_scan_result_entry(pMac, scan_res_ptr);
		return QDF_STATUS_E_RESOURCES;
	}

	sme_debug("LFR3:Add BSSID to scan cache" MAC_ADDRESS_STR,
		MAC_ADDR_ARRAY(scan_res_ptr->Result.BssDescriptor.bssId));
	csr_scan_add_result(pMac, scan_res_ptr, ies_local_ptr, session_id);
	if ((scan_res_ptr->Result.pvIes == NULL) && ies_local_ptr)
		qdf_mem_free(ies_local_ptr);
	return QDF_STATUS_SUCCESS;
}
#endif

/**
 * csr_get_fst_bssdescr_ptr() - This function returns the pointer to first bss
 * description from scan handle
 * @result_handle: an object for the result.
 *
 * Return: first bss descriptor from the scan handle.
 */
tpSirBssDescription csr_get_fst_bssdescr_ptr(tScanResultHandle result_handle)
{
	tListElem *first_element = NULL;
	struct tag_csrscan_result *scan_result = NULL;
	struct scan_result_list *bss_list =
				(struct scan_result_list *)result_handle;

	if (NULL == bss_list) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
			FL("Empty bss_list"));
		return NULL;
	}
	if (csr_ll_is_list_empty(&bss_list->List, LL_ACCESS_NOLOCK)) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
			FL("bss_list->List is empty"));
		return NULL;
	}
	first_element = csr_ll_peek_head(&bss_list->List, LL_ACCESS_NOLOCK);
	if (NULL == first_element) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
			FL("peer head return NULL"));
		return NULL;
	}

	scan_result = GET_BASE_ADDR(first_element, struct tag_csrscan_result,
					Link);

	return &scan_result->Result.BssDescriptor;
}

/**
 * csr_get_bssdescr_from_scan_handle() - This function to extract
 *                                       first bss description from scan handle
 * @result_handle: an object for the result.
 *
 * This function is written to extract first bss from scan handle.
 *
 * Return: first bss descriptor from the scan handle.
 */
tSirBssDescription*
csr_get_bssdescr_from_scan_handle(tScanResultHandle result_handle,
				tSirBssDescription *bss_descr)
{
	tListElem *first_element = NULL;
	struct tag_csrscan_result *scan_result = NULL;
	struct scan_result_list *bss_list =
				(struct scan_result_list *)result_handle;

	if (NULL == bss_list) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
				FL("Empty bss_list"));
		return NULL;
	}
	if (csr_ll_is_list_empty(&bss_list->List, LL_ACCESS_NOLOCK)) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
				FL("bss_list->List is empty"));
		qdf_mem_free(bss_list);
		return NULL;
	}
	first_element = csr_ll_peek_head(&bss_list->List, LL_ACCESS_NOLOCK);
	if (first_element) {
		scan_result = GET_BASE_ADDR(first_element,
				struct tag_csrscan_result,
				Link);
		qdf_mem_copy(bss_descr,
				&scan_result->Result.BssDescriptor,
				sizeof(tSirBssDescription));
	}
	return bss_descr;
}

/**
 * scan_active_list_cmd_timeout_handle() - To handle scan active command timeout
 * @userData: scan context
 *
 * This routine is to handle scan active command timeout
 *
 * Return: None
 */
void csr_scan_active_list_timeout_handle(void *userData)
{
	tSmeCmd *scan_cmd = (tSmeCmd *) userData;
	tHalHandle *hal_ctx = cds_get_context(QDF_MODULE_ID_PE);
	tpAniSirGlobal mac_ctx;
	uint16_t scan_id;
	tSirSmeScanAbortReq *msg;
	uint16_t msg_len;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (scan_cmd == NULL) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
			FL("Scan Timeout: Scan command is NULL"));
		return;
	}
	if (hal_ctx == NULL) {
		QDF_TRACE(QDF_MODULE_ID_SME, QDF_TRACE_LEVEL_ERROR,
			FL("HAL Context is NULL"));
		return;
	}
	mac_ctx = PMAC_STRUCT(hal_ctx);
	scan_id = scan_cmd->u.scanCmd.scanID;
	sme_err("Scan Timeout:Sending abort to Firmware ID %d session %d",
		scan_id, scan_cmd->sessionId);
	msg_len = (uint16_t)(sizeof(tSirSmeScanAbortReq));
	msg = qdf_mem_malloc(msg_len);
	if (NULL == msg) {
		sme_err("Failed to alloc memory for SmeScanAbortReq");
		return;
	}
	msg->type = eWNI_SME_SCAN_ABORT_IND;
	msg->msgLen = msg_len;
	msg->sessionId = scan_cmd->sessionId;
	msg->scan_id = scan_id;
	status = cds_send_mb_message_to_mac(msg);
	if (!QDF_IS_STATUS_SUCCESS(status))
		sme_err("Failed to post message to LIM");

	csr_save_scan_results(mac_ctx, scan_cmd->u.scanCmd.reason,
		scan_cmd->sessionId);
	scan_cmd->u.scanCmd.abort_scan_indication = eCSR_SCAN_ABORT_DEFAULT;
	csr_release_scan_command(mac_ctx, scan_cmd, eCSR_SCAN_FAILURE);
}
