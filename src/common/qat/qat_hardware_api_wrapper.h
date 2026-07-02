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
#pragma once

#include <memory>

#include "cpa_types.h"

extern "C" {
#include <cpa.h>  // CpaStatus
#include <cpa_cy_common.h>
#include <cpa_cy_im.h>
#include <cpa_cy_sym.h>
#include <cpa_dc.h>        // CpaDcSessionHandle, CpaDcOpData, CpaDcRqResults, cpaDcCompressData2
#include <icp_sal_poll.h>  // icp_sal_DcPollInstance
#include <icp_sal_user.h>  // icp_sal_userStart

#ifdef VESAL_ENABLE_QAT_DUMP
CpaStatus dcDumpAllRings(CpaInstanceHandle instanceHandle) __attribute__((weak));
#endif
}

namespace vesal {
namespace qat {

class QatHardwareApiWrapper {
public:
    virtual CpaStatus QAT_icp_sal_userStart(const char* pProcessName) {
        return icp_sal_userStart(pProcessName);
    }

    virtual CpaStatus QAT_cpaDcGetNumInstances(Cpa16U* pNumInstances) {
        return cpaDcGetNumInstances(pNumInstances);
    }

    virtual CpaStatus QAT_cpaDcGetInstances(Cpa16U numInstances, CpaInstanceHandle* dcInstances) {
        return cpaDcGetInstances(numInstances, dcInstances);
    }

    virtual CpaStatus QAT_icpSalUserStop() {
        return icp_sal_userStop();
    }

    virtual CpaStatus QAT_cpaDcQueryCapabilities(CpaInstanceHandle dcInstance,
                                                 CpaDcInstanceCapabilities* pInstanceCapabilities) {
        return cpaDcQueryCapabilities(dcInstance, pInstanceCapabilities);
    }

    virtual CpaStatus QAT_cpaDcInstanceGetInfo2(const CpaInstanceHandle instanceHandle,
                                                CpaInstanceInfo2* pInstanceInfo2) {
        return cpaDcInstanceGetInfo2(instanceHandle, pInstanceInfo2);
    }

    virtual CpaStatus QAT_cpaDcSetAddressTranslation(const CpaInstanceHandle instanceHandle,
                                                     CpaVirtualToPhysical virtual2Physical) {
        return cpaDcSetAddressTranslation(instanceHandle, virtual2Physical);
    }

    virtual CpaStatus QAT_cpaDcStartInstance(CpaInstanceHandle instanceHandle,
                                             Cpa16U numBuffers,
                                             CpaBufferList** pIntermediateBuffers) {
        return cpaDcStartInstance(instanceHandle, numBuffers, pIntermediateBuffers);
    }

    virtual CpaStatus QAT_cpaDcStopInstance(CpaInstanceHandle instanceHandle) {
        return cpaDcStopInstance(instanceHandle);
    }

    virtual CpaStatus QAT_cpaDcCompressData2(CpaInstanceHandle instanceHandle,
                                             CpaDcSessionHandle sessionHandle,
                                             CpaBufferList* srcBufferList,
                                             CpaBufferList* dstBufferList,
                                             CpaDcOpData* pOpData,
                                             CpaDcRqResults* pResults,
                                             void* callbackTag) {
        return cpaDcCompressData2(instanceHandle,
                                  sessionHandle,
                                  srcBufferList,
                                  dstBufferList,
                                  pOpData,
                                  pResults,
                                  callbackTag);
    }

    virtual CpaStatus QAT_cpaDcDecompressData2(CpaInstanceHandle instanceHandle,
                                               CpaDcSessionHandle sessionHandle,
                                               CpaBufferList* srcBufferList,
                                               CpaBufferList* dstBufferList,
                                               CpaDcOpData* pOpData,
                                               CpaDcRqResults* pResults,
                                               void* callbackTag) {
        return cpaDcDecompressData2(instanceHandle,
                                    sessionHandle,
                                    srcBufferList,
                                    dstBufferList,
                                    pOpData,
                                    pResults,
                                    callbackTag);
    }

    virtual CpaStatus QAT_icp_sal_DcPollInstance(CpaInstanceHandle instanceHandle,
                                                 Cpa32U response_quota) {
        return icp_sal_DcPollInstance(instanceHandle, response_quota);
    }

#ifdef VESAL_ENABLE_QAT_DUMP
    virtual CpaStatus QAT_dcDumpAllRings(CpaInstanceHandle instanceHandle) {
        if (dcDumpAllRings == nullptr) {
            return CPA_STATUS_UNSUPPORTED;
        }
        return dcDumpAllRings(instanceHandle);
    }
#endif

    virtual CpaStatus QAT_icp_sal_CyPollInstance(CpaInstanceHandle instanceHandle,
                                                 Cpa32U response_quota) {
        return icp_sal_CyPollInstance(instanceHandle, response_quota);
    }

    virtual CpaStatus QAT_icp_sal_DcGetFileDescriptor(CpaInstanceHandle instanceHandle, int* fd) {
        return icp_sal_DcGetFileDescriptor(instanceHandle, fd);
    }

    virtual CpaStatus QAT_icp_sal_DcPutFileDescriptor(CpaInstanceHandle instanceHandle, int fd) {
        return icp_sal_DcPutFileDescriptor(instanceHandle, fd);
    }

    virtual CpaStatus QAT_cpaCyGetNumInstances(Cpa16U* pNumInstances) {
        return cpaCyGetNumInstances(pNumInstances);
    }

    virtual CpaStatus QAT_cpaCyGetInstances(Cpa16U numInstances, CpaInstanceHandle* cyInstances) {
        return cpaCyGetInstances(numInstances, cyInstances);
    }
    virtual CpaStatus QAT_cpaCyInstanceGetInfo2(const CpaInstanceHandle instanceHandle,
                                                CpaInstanceInfo2* pInstanceInfo2) {
        return cpaCyInstanceGetInfo2(instanceHandle, pInstanceInfo2);
    }

    virtual CpaStatus QAT_cpaDcBufferListGetMetaSize(const CpaInstanceHandle instanceHandle,
                                                     Cpa32U numBuffers,
                                                     Cpa32U* pSizeInBytes) {
        return cpaDcBufferListGetMetaSize(instanceHandle, numBuffers, pSizeInBytes);
    }

    virtual CpaStatus QAT_cpaCyBufferListGetMetaSize(const CpaInstanceHandle instanceHandle,
                                                     Cpa32U numBuffers,
                                                     Cpa32U* pSizeInBytes) {
        return cpaCyBufferListGetMetaSize(instanceHandle, numBuffers, pSizeInBytes);
    }

    virtual CpaStatus QAT_cpaCyStartInstance(CpaInstanceHandle instanceHandle) {
        return cpaCyStartInstance(instanceHandle);
    }

    virtual CpaStatus QAT_cpaCyStopInstance(CpaInstanceHandle instanceHandle) {
        return cpaCyStopInstance(instanceHandle);
    }

    virtual CpaStatus QAT_cpaCyQueryCapabilities(const CpaInstanceHandle instanceHandle,
                                                 CpaCyCapabilitiesInfo* pCapInfo) {
        return cpaCyQueryCapabilities(instanceHandle, pCapInfo);
    }

    virtual CpaStatus QAT_cpaCySymQueryCapabilities(const CpaInstanceHandle instanceHandle,
                                                    CpaCySymCapabilitiesInfo* pCapInfo) {
        return cpaCySymQueryCapabilities(instanceHandle, pCapInfo);
    }

    virtual CpaStatus QAT_cpaCySetAddressTranslation(const CpaInstanceHandle instanceHandle,
                                                     CpaVirtualToPhysical virtual2Physical) {
        return cpaCySetAddressTranslation(instanceHandle, virtual2Physical);
    }

    virtual CpaStatus QAT_cpaCySymSessionCtxGetSize(
        const CpaInstanceHandle instanceHandle,
        const CpaCySymSessionSetupData* pSessionSetupData,
        Cpa32U* pSizeInBytes) {
        return cpaCySymSessionCtxGetSize(instanceHandle, pSessionSetupData, pSizeInBytes);
    }

    virtual CpaStatus QAT_cpaCySymSessionCtxGetDynamicSize(
        const CpaInstanceHandle instanceHandle,
        const CpaCySymSessionSetupData* pSessionSetupData,
        Cpa32U* pSessionCtxSizeInBytes) {
        return cpaCySymSessionCtxGetDynamicSize(
            instanceHandle, pSessionSetupData, pSessionCtxSizeInBytes);
    }

    virtual CpaStatus QAT_cpaCySymInitSession(const CpaInstanceHandle instanceHandle,
                                              const CpaCySymCbFunc pSymCb,
                                              const CpaCySymSessionSetupData* pSessionSetupData,
                                              CpaCySymSessionCtx sessionCtx) {
        return cpaCySymInitSession(instanceHandle, pSymCb, pSessionSetupData, sessionCtx);
    }

    virtual CpaStatus QAT_cpaCySymRemoveSession(const CpaInstanceHandle instanceHandle,
                                                CpaCySymSessionCtx pSessionCtx) {
        return cpaCySymRemoveSession(instanceHandle, pSessionCtx);
    }

    virtual CpaStatus QAT_cpaCySymPerformOp(const CpaInstanceHandle instanceHandle,
                                            void* pCallbackTag,
                                            const CpaCySymOpData* pOpData,
                                            const CpaBufferList* pSrcBuffer,
                                            CpaBufferList* pDstBuffer,
                                            CpaBoolean* pVerifyResult) {
        return cpaCySymPerformOp(
            instanceHandle, pCallbackTag, pOpData, pSrcBuffer, pDstBuffer, pVerifyResult);
    }

    virtual ~QatHardwareApiWrapper() = default;
};

inline QatHardwareApiWrapper* GetQatApiWrapper() {
    extern std::unique_ptr<QatHardwareApiWrapper> g_qat_api_wrapper;
    return g_qat_api_wrapper.get();
}

}  // namespace qat
}  // namespace vesal
