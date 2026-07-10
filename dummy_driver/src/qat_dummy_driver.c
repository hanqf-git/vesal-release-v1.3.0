/*
 * Copyright (c) 2023 ByteDance Inc.
 *
 * This file is part of veSAL.
 *
 * veSAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * veSAL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with veSAL. If not, see <https://www.gnu.org/licenses/>.
 */

#include <errno.h>

#include "dummy_driver_common.h"
#include "quickassist/include/cpa.h"
#include "quickassist/include/cpa_types.h"
#include "quickassist/include/dc/cpa_dc.h"
#include "quickassist/include/lac/cpa_cy_common.h"
#include "quickassist/include/lac/cpa_cy_im.h"
#include "quickassist/include/lac/cpa_cy_sym.h"
#include "quickassist/lookaside/access_layer/include/icp_sal_poll.h"
#include "quickassist/lookaside/access_layer/include/icp_sal_user.h"

int g_driver_load_codec_ok = 0;
int g_driver_load_cypher_ok = 0;

// cpa_dc.h
MAKE_WRAPPER_FUNC_COMMON(cpaDcBufferListGetMetaSize,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         Cpa32U numBuffers,
                         Cpa32U* pSizeInBytes) {
    return FUNC_PTR(cpaDcBufferListGetMetaSize)(instanceHandle, numBuffers, pSizeInBytes);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcGetNumInstances, CpaStatus, Cpa16U* pNumInstances) {
    return FUNC_PTR(cpaDcGetNumInstances)(pNumInstances);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcGetInstances,
                         CpaStatus,
                         Cpa16U numInstances,
                         CpaInstanceHandle* dcInstances) {
    return FUNC_PTR(cpaDcGetInstances)(numInstances, dcInstances);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcQueryCapabilities,
                         CpaStatus,
                         CpaInstanceHandle dcInstance,
                         CpaDcInstanceCapabilities* pInstanceCapabilities) {
    return FUNC_PTR(cpaDcQueryCapabilities)(dcInstance, pInstanceCapabilities);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcInstanceGetInfo2,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         CpaInstanceInfo2* pInstanceInfo2) {
    return FUNC_PTR(cpaDcInstanceGetInfo2)(instanceHandle, pInstanceInfo2);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcSetAddressTranslation,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         CpaVirtualToPhysical virtual2Physical) {
    return FUNC_PTR(cpaDcSetAddressTranslation)(instanceHandle, virtual2Physical);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcStartInstance,
                         CpaStatus,
                         CpaInstanceHandle instanceHandle,
                         Cpa16U numBuffers,
                         CpaBufferList** pIntermediateBuffers) {
    return FUNC_PTR(cpaDcStartInstance)(instanceHandle, numBuffers, pIntermediateBuffers);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcStopInstance, CpaStatus, CpaInstanceHandle instanceHandle) {
    return FUNC_PTR(cpaDcStopInstance)(instanceHandle);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcRemoveSession,
                         CpaStatus,
                         const CpaInstanceHandle dcInstance,
                         CpaDcSessionHandle pSessionHandle) {
    return FUNC_PTR(cpaDcRemoveSession)(dcInstance, pSessionHandle);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcInitSession,
                         CpaStatus,
                         CpaInstanceHandle dcInstance,
                         CpaDcSessionHandle pSessionHandle,
                         CpaDcSessionSetupData* pSessionData,
                         CpaBufferList* pContextBuffer,
                         CpaDcCallbackFn callbackFn) {
    return FUNC_PTR(cpaDcInitSession)(
        dcInstance, pSessionHandle, pSessionData, pContextBuffer, callbackFn);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcGetSessionSize,
                         CpaStatus,
                         CpaInstanceHandle dcInstance,
                         CpaDcSessionSetupData* pSessionData,
                         Cpa32U* pSessionSize,
                         Cpa32U* pContextSize) {
    return FUNC_PTR(cpaDcGetSessionSize)(dcInstance, pSessionData, pSessionSize, pContextSize);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcResetSession,
                         CpaStatus,
                         const CpaInstanceHandle dcInstance,
                         CpaDcSessionHandle pSessionHandle) {
    return FUNC_PTR(cpaDcResetSession)(dcInstance, pSessionHandle);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcSetCrcControlData,
                         CpaStatus,
                         CpaInstanceHandle dcInstance,
                         CpaDcSessionHandle pSessionHandle,
                         CpaCrcControlData* pCrcControlData) {
    return FUNC_PTR(cpaDcSetCrcControlData)(dcInstance, pSessionHandle, pCrcControlData);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcCompressData2,
                         CpaStatus,
                         CpaInstanceHandle dcInstance,
                         CpaDcSessionHandle pSessionHandle,
                         CpaBufferList* pSrcBuff,
                         CpaBufferList* pDestBuff,
                         CpaDcOpData* pOpData,
                         CpaDcRqResults* pResults,
                         void* callbackTag) {
    return FUNC_PTR(cpaDcCompressData2)(
        dcInstance, pSessionHandle, pSrcBuff, pDestBuff, pOpData, pResults, callbackTag);
}

MAKE_WRAPPER_FUNC_COMMON(cpaDcDecompressData2,
                         CpaStatus,
                         CpaInstanceHandle dcInstance,
                         CpaDcSessionHandle pSessionHandle,
                         CpaBufferList* pSrcBuff,
                         CpaBufferList* pDestBuff,
                         CpaDcOpData* pOpData,
                         CpaDcRqResults* pResults,
                         void* callbackTag) {
    return FUNC_PTR(cpaDcDecompressData2)(
        dcInstance, pSessionHandle, pSrcBuff, pDestBuff, pOpData, pResults, callbackTag);
}

// icp_sal_user.h
MAKE_WRAPPER_FUNC_COMMON(icp_sal_userStart, CpaStatus, const char* pProcessName) {
    return FUNC_PTR(icp_sal_userStart)(pProcessName);
}

MAKE_WRAPPER_FUNC_COMMON(icp_sal_userStop, CpaStatus, void) {
    return FUNC_PTR(icp_sal_userStop)();
}

MAKE_WRAPPER_FUNC_COMMON(icp_sal_dc_simulate_error, CpaStatus, Cpa8U numErrors, Cpa8S dcError) {
    if (UNLIKELY(!FUNC_PTR(icp_sal_dc_simulate_error))) {
        DLOG("icp_sal_dc_simulate_error not found in qat driver. Might need to check if "
             "libqat_s.so is compiled with error simulation enabled.");
        errno = EOPNOTSUPP;
        return CPA_STATUS_FAIL;
    }
    return FUNC_PTR(icp_sal_dc_simulate_error)(numErrors, dcError);
}

MAKE_WRAPPER_FUNC_COMMON(icp_sal_cnv_simulate_error,
                         CpaStatus,
                         CpaInstanceHandle dcInstance,
                         CpaDcSessionHandle pSessionHandle) {
    return FUNC_PTR(icp_sal_cnv_simulate_error)(dcInstance, pSessionHandle);
}

// icp_sal_poll.h
MAKE_WRAPPER_FUNC_COMMON(icp_sal_DcPollInstance,
                         CpaStatus,
                         CpaInstanceHandle instanceHandle,
                         Cpa32U response_quota) {
    return FUNC_PTR(icp_sal_DcPollInstance)(instanceHandle, response_quota);
}

MAKE_WRAPPER_FUNC_COMMON(icp_sal_DcGetFileDescriptor,
                         CpaStatus,
                         CpaInstanceHandle instanceHandle,
                         int* fd) {
    return FUNC_PTR(icp_sal_DcGetFileDescriptor)(instanceHandle, fd);
}

MAKE_WRAPPER_FUNC_COMMON(icp_sal_DcPutFileDescriptor,
                         CpaStatus,
                         CpaInstanceHandle instanceHandle,
                         int fd) {
    return FUNC_PTR(icp_sal_DcPutFileDescriptor)(instanceHandle, fd);
}

MAKE_WRAPPER_FUNC_COMMON(dcDumpAllRings, CpaStatus, CpaInstanceHandle instanceHandle) {
    if (UNLIKELY(!FUNC_PTR(dcDumpAllRings))) {
        DLOG("dcDumpAllRings not found in qat driver. Might need to check if libqat_s.so is "
             "compiled with QAT ring dump support enabled.");
        errno = EOPNOTSUPP;
        return CPA_STATUS_UNSUPPORTED;
    }
    return FUNC_PTR(dcDumpAllRings)(instanceHandle);
}

MAKE_WRAPPER_FUNC_COMMON(dcDumpHwRegs, CpaStatus, CpaInstanceHandle instanceHandle) {
    if (UNLIKELY(!FUNC_PTR(dcDumpHwRegs))) {
        DLOG("dcDumpHwRegs not found in qat driver. Might need to check if libqat_s.so is "
             "compiled with QAT register dump support enabled.");
        errno = EOPNOTSUPP;
        return CPA_STATUS_UNSUPPORTED;
    }
    return FUNC_PTR(dcDumpHwRegs)(instanceHandle);
}

MAKE_WRAPPER_FUNC_COMMON(icp_sal_CyPollInstance,
                         CpaStatus,
                         CpaInstanceHandle instanceHandle,
                         Cpa32U response_quota) {
    return FUNC_PTR(icp_sal_CyPollInstance)(instanceHandle, response_quota);
}

// cpa_cy_common.h
MAKE_WRAPPER_FUNC_COMMON(cpaCyGetNumInstances, CpaStatus, Cpa16U* pNumInstances) {
    return FUNC_PTR(cpaCyGetNumInstances)(pNumInstances);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCyGetInstances,
                         CpaStatus,
                         Cpa16U numInstances,
                         CpaInstanceHandle* cyInstances) {
    return FUNC_PTR(cpaCyGetInstances)(numInstances, cyInstances);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCyInstanceGetInfo2,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         CpaInstanceInfo2* pInstanceInfo2) {
    return FUNC_PTR(cpaCyInstanceGetInfo2)(instanceHandle, pInstanceInfo2);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCyBufferListGetMetaSize,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         Cpa32U numBuffers,
                         Cpa32U* pSizeInBytes) {
    return FUNC_PTR(cpaCyBufferListGetMetaSize)(instanceHandle, numBuffers, pSizeInBytes);
}

// cpa_cy_im.h
MAKE_WRAPPER_FUNC_COMMON(cpaCyStartInstance, CpaStatus, CpaInstanceHandle instanceHandle) {
    return FUNC_PTR(cpaCyStartInstance)(instanceHandle);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCyStopInstance, CpaStatus, CpaInstanceHandle instanceHandle) {
    return FUNC_PTR(cpaCyStopInstance)(instanceHandle);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCyQueryCapabilities,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         CpaCyCapabilitiesInfo* pCapInfo) {
    return FUNC_PTR(cpaCyQueryCapabilities)(instanceHandle, pCapInfo);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCySymQueryCapabilities,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         CpaCySymCapabilitiesInfo* pCapInfo) {
    return FUNC_PTR(cpaCySymQueryCapabilities)(instanceHandle, pCapInfo);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCySetAddressTranslation,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         CpaVirtualToPhysical virtual2Physical) {
    return FUNC_PTR(cpaCySetAddressTranslation)(instanceHandle, virtual2Physical);
}

// cpa_cy_sym.h
MAKE_WRAPPER_FUNC_COMMON(cpaCySymSessionCtxGetSize,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         const CpaCySymSessionSetupData* pSessionSetupData,
                         Cpa32U* pSessionCtxSizeInBytes) {
    return FUNC_PTR(cpaCySymSessionCtxGetSize)(
        instanceHandle, pSessionSetupData, pSessionCtxSizeInBytes);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCySymSessionCtxGetDynamicSize,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         const CpaCySymSessionSetupData* pSessionSetupData,
                         Cpa32U* pSessionCtxSizeInBytes) {
    return FUNC_PTR(cpaCySymSessionCtxGetDynamicSize)(
        instanceHandle, pSessionSetupData, pSessionCtxSizeInBytes);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCySymInitSession,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         const CpaCySymCbFunc pSymCb,
                         const CpaCySymSessionSetupData* pSessionSetupData,
                         CpaCySymSessionCtx sessionCtx) {
    return FUNC_PTR(cpaCySymInitSession)(instanceHandle, pSymCb, pSessionSetupData, sessionCtx);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCySymRemoveSession,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         CpaCySymSessionCtx pSessionCtx) {
    return FUNC_PTR(cpaCySymRemoveSession)(instanceHandle, pSessionCtx);
}

MAKE_WRAPPER_FUNC_COMMON(cpaCySymPerformOp,
                         CpaStatus,
                         const CpaInstanceHandle instanceHandle,
                         void* pCallbackTag,
                         const CpaCySymOpData* pOpData,
                         const CpaBufferList* pSrcBuffer,
                         CpaBufferList* pDstBuffer,
                         CpaBoolean* pVerifyResult) {
    return FUNC_PTR(cpaCySymPerformOp)(
        instanceHandle, pCallbackTag, pOpData, pSrcBuffer, pDstBuffer, pVerifyResult);
}

static void* handle = NULL;

static void qat_dummy_driver_reset(void) {
    DLOG("qat_dummy_driver_reset() is called\n");
    // cpa_dc.h
    FUNC_PTR(cpaDcBufferListGetMetaSize) = NULL;
    FUNC_PTR(cpaDcGetNumInstances) = NULL;
    FUNC_PTR(cpaDcGetInstances) = NULL;
    FUNC_PTR(cpaDcQueryCapabilities) = NULL;
    FUNC_PTR(cpaDcInstanceGetInfo2) = NULL;
    FUNC_PTR(cpaDcSetAddressTranslation) = NULL;
    FUNC_PTR(cpaDcStartInstance) = NULL;
    FUNC_PTR(cpaDcStopInstance) = NULL;
    FUNC_PTR(cpaDcRemoveSession) = NULL;
    FUNC_PTR(cpaDcInitSession) = NULL;
    FUNC_PTR(cpaDcGetSessionSize) = NULL;
    FUNC_PTR(cpaDcResetSession) = NULL;
    FUNC_PTR(cpaDcSetCrcControlData) = NULL;
    FUNC_PTR(cpaDcCompressData2) = NULL;
    FUNC_PTR(cpaDcDecompressData2) = NULL;
    // icp_sal_user.h
    FUNC_PTR(icp_sal_userStart) = NULL;
    FUNC_PTR(icp_sal_userStop) = NULL;
    FUNC_PTR(icp_sal_dc_simulate_error) = NULL;
    FUNC_PTR(icp_sal_cnv_simulate_error) = NULL;
    // icp_sal_poll.h
    FUNC_PTR(icp_sal_DcPollInstance) = NULL;
    FUNC_PTR(icp_sal_DcGetFileDescriptor) = NULL;
    FUNC_PTR(icp_sal_DcPutFileDescriptor) = NULL;
    FUNC_PTR(dcDumpAllRings) = NULL;
    FUNC_PTR(dcDumpHwRegs) = NULL;
    FUNC_PTR(icp_sal_CyPollInstance) = NULL;
    // cpa_cy_common.h
    FUNC_PTR(cpaCyGetNumInstances) = NULL;
    FUNC_PTR(cpaCyGetInstances) = NULL;
    FUNC_PTR(cpaCyInstanceGetInfo2) = NULL;
    FUNC_PTR(cpaCyBufferListGetMetaSize) = NULL;
    // cpa_cy_im.h
    FUNC_PTR(cpaCyStartInstance) = NULL;
    FUNC_PTR(cpaCyStopInstance) = NULL;
    FUNC_PTR(cpaCyQueryCapabilities) = NULL;
    FUNC_PTR(cpaCySetAddressTranslation) = NULL;
    // cpa_cy_sym.h
    FUNC_PTR(cpaCySymSessionCtxGetSize) = NULL;
    FUNC_PTR(cpaCySymSessionCtxGetDynamicSize) = NULL;
    FUNC_PTR(cpaCySymInitSession) = NULL;
    FUNC_PTR(cpaCySymRemoveSession) = NULL;
    FUNC_PTR(cpaCySymPerformOp) = NULL;
    FUNC_PTR(cpaCySymQueryCapabilities) = NULL;
    g_driver_load_codec_ok = 0;
    g_driver_load_cypher_ok = 0;

    // close handle
    if (handle) {
        int ret = dlclose(handle);
        if (UNLIKELY(ret != 0)) {
            DLOG("Failed to close libibverbs.so\n");
        }
        handle = NULL;
    }
}

void LoadDC() {
    // cpa_dc.h
    LOAD_SYM_COMMON(handle, cpaDcBufferListGetMetaSize);
    LOAD_SYM_COMMON(handle, cpaDcGetNumInstances);
    LOAD_SYM_COMMON(handle, cpaDcGetInstances);
    LOAD_SYM_COMMON(handle, cpaDcQueryCapabilities);
    LOAD_SYM_COMMON(handle, cpaDcInstanceGetInfo2);
    LOAD_SYM_COMMON(handle, cpaDcSetAddressTranslation);
    LOAD_SYM_COMMON(handle, cpaDcStartInstance);
    LOAD_SYM_COMMON(handle, cpaDcStopInstance);
    LOAD_SYM_COMMON(handle, cpaDcRemoveSession);
    LOAD_SYM_COMMON(handle, cpaDcInitSession);
    LOAD_SYM_COMMON(handle, cpaDcGetSessionSize);
    LOAD_SYM_COMMON(handle, cpaDcResetSession);
    // This is a must for dc, but not block here because older QAT not support it. Need to give it a
    // go.
    // TODO(...): split dc ok and cy ok, make dc not ok if cpaDcSetCrcControlData is not loaded.
    LOAD_SYM_OPTIONAL_COMMON(handle, cpaDcSetCrcControlData);
    LOAD_SYM_COMMON(handle, cpaDcCompressData2);
    LOAD_SYM_COMMON(handle, cpaDcDecompressData2);
    // icp_sal_user.h
    LOAD_SYM_COMMON(handle, icp_sal_userStart);
    LOAD_SYM_COMMON(handle, icp_sal_userStop);
    // Simulation are optional because vesal implement its own simulation
    LOAD_SYM_OPTIONAL_COMMON(handle, icp_sal_dc_simulate_error);
    LOAD_SYM_OPTIONAL_COMMON(handle, icp_sal_cnv_simulate_error);
    // icp_sal_poll.h
    LOAD_SYM_COMMON(handle, icp_sal_DcPollInstance);
    LOAD_SYM_COMMON(handle, icp_sal_DcGetFileDescriptor);
    LOAD_SYM_COMMON(handle, icp_sal_DcPutFileDescriptor);
    LOAD_SYM_OPTIONAL_COMMON(handle, dcDumpAllRings);
    LOAD_SYM_OPTIONAL_COMMON(handle, dcDumpHwRegs);
    g_driver_load_codec_ok = 1;
    return;
teardown:
    g_driver_load_codec_ok = 0;
}

void LoadCY() {
    // icp_sal_poll.h
    LOAD_SYM_COMMON(handle, icp_sal_CyPollInstance);
    // cpa_cy_common.h
    LOAD_SYM_COMMON(handle, cpaCyGetNumInstances);
    LOAD_SYM_COMMON(handle, cpaCyGetInstances);
    LOAD_SYM_COMMON(handle, cpaCyInstanceGetInfo2);
    LOAD_SYM_COMMON(handle, cpaCyBufferListGetMetaSize);
    // cpa_cy_im.h
    LOAD_SYM_COMMON(handle, cpaCyStartInstance);
    LOAD_SYM_COMMON(handle, cpaCyStopInstance);
    LOAD_SYM_COMMON(handle, cpaCyQueryCapabilities);
    LOAD_SYM_COMMON(handle, cpaCySetAddressTranslation);
    // cpa_cy_sym.h
    LOAD_SYM_COMMON(handle, cpaCySymSessionCtxGetSize);
    LOAD_SYM_COMMON(handle, cpaCySymSessionCtxGetDynamicSize);
    LOAD_SYM_COMMON(handle, cpaCySymInitSession);
    LOAD_SYM_COMMON(handle, cpaCySymRemoveSession);
    LOAD_SYM_COMMON(handle, cpaCySymPerformOp);
    LOAD_SYM_COMMON(handle, cpaCySymQueryCapabilities);
    g_driver_load_cypher_ok = 1;
    return;
teardown:
    g_driver_load_cypher_ok = 0;
}

// constructor
static __attribute__((constructor)) void qat_dummy_driver_init(void) {
    DLOG("qat_dummy_driver_init() is called\n");
    handle = dlopen("libqat_s.so", RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        DLOG("Failed to open libqat_s.so\n");
        return;
    }
    // load symbols
    LoadDC();
    LoadCY();

    if (!g_driver_load_codec_ok && !g_driver_load_cypher_ok) {
        DLOG("qat_dummy_driver_init() failed\n");
        qat_dummy_driver_reset();
    }
}
