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

/***************************************************************************
 *
 *   BSD LICENSE
 * 
 *   Copyright(c) 2007-2022 Intel Corporation. All rights reserved.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 *  version: QAT20.L.1.1.40-00018
 *
 ***************************************************************************/

/**
 ***************************************************************************
 * @file icp_sal_user.h
 *
 * @ingroup SalUser
 *
 * User space process init and shutdown functions.
 *
 ***************************************************************************/

#ifndef ICP_SAL_USER_H
#define ICP_SAL_USER_H

#include "cpa_dc.h"

#ifndef NO_DYNAMIC_INSTANCE
#define icp_sal_userCyFreeInstances(...) (CPA_STATUS_FAIL)
#define icp_sal_userCyGetAvailableNumDynInstances(...) (CPA_STATUS_FAIL)
#define icp_sal_userCyInstancesAlloc(...) (CPA_STATUS_FAIL)
#define icp_sal_userDcFreeInstances(...) (CPA_STATUS_FAIL)
#define icp_sal_userDcGetAvailableNumDynInstances(...) (CPA_STATUS_FAIL)
#define icp_sal_userDcInstancesAlloc(...) (CPA_STATUS_FAIL)
#endif

/*************************************************************************
 * @ingroup SalUser
 * @description
 *    This function initialises and starts user space service access layer
 *    (SAL) - it registers SAL with ADF and initialises the ADF proxy.
 *    This function must only be called once per user space process.
 *
 * @context
 *      This function is called from the user process context
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @param[in] pProcessName           Process address space name described in
 *                                   the config file for this device
 *
 * @retval CPA_STATUS_SUCCESS        No error
 * @retval CPA_STATUS_FAIL           Operation failed
 *
 *************************************************************************/
CpaStatus icp_sal_userStart(const char *pProcessName);

/*************************************************************************
 * @ingroup SalUser
 * @description
 *    Simple wrapper for the icp_sal_userStart() function
 *
 *    This function is only for backwards compatibility.
 *    New users should use icp_sal_userStart function directly.
 *
 *************************************************************************/
CpaStatus icp_sal_userStartMultiProcess(const char *pProcessName,
                                        CpaBoolean limitDevAccess);

/*************************************************************************
 * @ingroup SalUser
 * @description
 *    This function stops and shuts down user space SAL
 *     - it deregisters SAL with ADF and shuts down ADF proxy
 *
 * @context
 *      This function is called from the user process context
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @retval CPA_STATUS_SUCCESS        No error
 * @retval CPA_STATUS_FAIL           Operation failed
 *
 ************************************************************************/
CpaStatus icp_sal_userStop(void);

/*************************************************************************
 * @ingroup SalUser
 * @description
 *    This function checks if new devices have been started and if so
 *    starts to use them.
 *
 * @context
 *      This function is called from the user process context
 *      in threadless mode
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      No
 *
 * @retval CPA_STATUS_SUCCESS        No error
 * @retval CPA_STATUS_FAIL           Operation failed
 *
 ************************************************************************/
CpaStatus icp_sal_find_new_devices(void);

/*************************************************************************
 * @ingroup SalUser
 * @description
 *    This function polls device events.
 *
 * @context
 *      This function is called from the user process context
 *      in threadless mode
 *
 * @assumptions
 *      None
 * @sideEffects
 *      In case a device has beed stoped or restarted the application
 *      will get restarting/stop/shutdown events
 * @reentrant
 *      No
 * @threadSafe
 *      No
 *
 * @retval CPA_STATUS_SUCCESS        No error
 * @retval CPA_STATUS_FAIL           Operation failed
 *
 ************************************************************************/
CpaStatus icp_sal_poll_device_events(void);

/*
 * icp_adf_check_device
 *
 * @description:
 *  This function checks the status of the firmware/hardware for a given device.
 *  This function is used as part of the heartbeat functionality.
 *
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      None
 * @sideEffects
 *      In case a device is unresponsive the device will
 *      be restarted.
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @param[in] accelId                Device Id
 * @retval CPA_STATUS_SUCCESS        No error
 * @retval CPA_STATUS_FAIL           Operation failed
 */
CpaStatus icp_sal_check_device(Cpa32U accelId);

/*
 * icp_adf_check_all_devices
 *
 * @description:
 *  This function checks the status of the firmware/hardware for all devices.
 *  This function is used as part of the heartbeat functionality.
 *
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      None
 * @sideEffects
 *      In case a device is unresponsive the device will
 *      be restarted.
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @retval CPA_STATUS_SUCCESS        No error
 * @retval CPA_STATUS_FAIL           Operation failed
 */
CpaStatus icp_sal_check_all_devices(void);

#ifdef ICP_HB_FAIL_SIM
/*
 * icp_sal_heartbeat_simulate_failure
 *
 * @description:
 *  This function simulates a heartbeat failure
 *
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      None
 * @sideEffects
 *      This along with a icp_sal_check call will notify the heartbeat
 *      error to user space
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @param[in] accelId                Device Id
 * @retval CPA_STATUS_SUCCESS        No error
 * @retval CPA_STATUS_FAIL           Operation failed
 * @retval CPA_STATUS_UNSUPPORTED    Unsupported device
 */
CpaStatus icp_sal_heartbeat_simulate_failure(Cpa32U accelId);

#endif


/*
 * @ingroup icp_sal_user
 * @description
 *      This is a stub function to send messages to VF
 *
 * @context
 *      None
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      Yes
 * @threadSafe
 *      Yes
 *
 */
CpaStatus icp_sal_userSendMsgToVf(Cpa32U accelId, Cpa32U vfNum, Cpa32U message);

/*
 * @ingroup icp_sal_user
 * @description
 *      This is a stub function to send messages to PF
 *
 * @context
 *      None
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      Yes
 * @threadSafe
 *      Yes
 *
 */
CpaStatus icp_sal_userSendMsgToPf(Cpa32U accelId, Cpa32U message);

/*
 * @ingroup icp_sal_user
 * @description
 *      This is a stub function to get messages from VF
 *
 * @context
 *      None
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      Yes
 * @threadSafe
 *      Yes
 *
 */
CpaStatus icp_sal_userGetMsgFromVf(Cpa32U accelId,
                                   Cpa32U vfNum,
                                   Cpa32U *message,
                                   Cpa32U *messageCounter);

/*
 * @ingroup icp_sal_user
 * @description
 *      This is a stub function to get messages from PF
 *
 * @context
 *      None
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      Yes
 * @threadSafe
 *      Yes
 *
 */
CpaStatus icp_sal_userGetMsgFromPf(Cpa32U accelId,
                                   Cpa32U *message,
                                   Cpa32U *messageCounter);

/*
 * @ingroup icp_sal_user
 * @description
 *      This is a stub function to get pfvf comms status
 *
 * @context
 *      None
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      Yes
 * @threadSafe
 *      Yes
 *
 */
CpaStatus icp_sal_userGetPfVfcommsStatus(CpaBoolean *unreadMessage);

/*
 * @ingroup icp_sal_user
 * @description
 *      This is a stub function to reset the device
 *
 * @context
 *     None
 *
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      Yes
 * @threadSafe
 *      Yes
 *
 */
CpaStatus icp_sal_reset_device(Cpa32U accelId);

/*
 * icp_sal_userIsQatAvailable
 *
 * @description:
 *  This function returns CPA_TRUE if at least one QAT device
 *  is present in the system and available
 *
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      Yes
 * @threadSafe
 *      Yes
 *
 * @retval CPA_TRUE     QAT device available
 * @retval CPA_FALSE    QAT device not available
 *
 */
CpaBoolean icp_sal_userIsQatAvailable(void);

#ifdef ICP_DC_ERROR_SIMULATION
/*
 * icp_sal_dc_simulate_error
 *
 * @description:
 *  This function injects a simulated compression error for a defined
 *  number of compression requests
 *
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      No
 *
 * @param[in] numErrors              Num DC Errors
 *                                   0 - No Error injection
 *                                   1-0xFE - Num Errors to Inject
 *                                   0xFF - Always inject Error
 * @param[in] dcError                DC Error Type
 * @retval CPA_STATUS_SUCCESS        No error
 * @retval CPA_STATUS_FAIL           Operation failed
 */
CpaStatus icp_sal_dc_simulate_error(Cpa8U numErrors, Cpa8S dcError);
#endif

/*
 * icp_sal_cnv_simulate_error
 *
 * @description:
 *  This function enables the CnVError injection for the
 *  session passed in. All Compression requests sent within
 *  the session are injected with CnV errors. This error injection
 *  is for the duration of the session. Resetting the session
 *  results in setting being cleared.
 *  CnV error injection does not apply to Data Plane API.
 *
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      The session has been initialized via cpaDcInitSession function
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      No
 *
 * @param[in] dcInstance             Instance Handle
 * @param[in] pSessionHandle         Session Handle
 *
 * @retval CPA_STATUS_UNSUPPORTED    Unsupported feature
 * @retval CPA_STATUS_INVALID_PARAM  Invalid parameter passed in
 * @retval CPA_STATUS_SUCCESS        No error
 *
 */
CpaStatus icp_sal_cnv_simulate_error(CpaInstanceHandle dcInstance,
                                     CpaDcSessionHandle pSessionHandle);

/*
 * icp_sal_ns_cnv_simulate_error
 *
 * @description:
 *  This function enables the CnVError injection for the
 *  sessionless case. All Compression requests sent
 *  to the dcInstance that is  passed in as a parameter,
 *  are injected with CnV errors. This CnV error injection
 *  does not apply to Data Plane API.
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      No
 *
 * @param[in] dcInstance             Instance Handle
 *
 * @retval CPA_STATUS_UNSUPPORTED    Unsupported feature
 * @retval CPA_STATUS_INVALID_PARAM  Invalid parameter passed in
 * @retval CPA_STATUS_SUCCESS        No error
 *
 */
CpaStatus icp_sal_ns_cnv_simulate_error(CpaInstanceHandle dcInstance);

/*
 * icp_sal_ns_cnv_reset_error
 *
 * @description:
 *  This function resets the CnVError injection for the
 *  specific dcInstance that is  passed in as a parameter.
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      No
 *
 * @param[in] dcInstance             Instance Handle
 *
 * @retval CPA_STATUS_UNSUPPORTED    Unsupported feature
 * @retval CPA_STATUS_INVALID_PARAM  Invalid parameter passed in
 * @retval CPA_STATUS_SUCCESS        No error
 *
 */
CpaStatus icp_sal_ns_cnv_reset_error(CpaInstanceHandle dcInstance);

/*
 * icp_sal_get_dc_errors
 *
 * @description:
 *  This function returns the occurrences of compression errors specified
 *  in the input parameter
 *
 * @context
 *      This function is called from the user process context
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      No
 * @param[in] dcError                DC Error Type
 *
 * returns                           Number of failing requests of type dcError
 */
Cpa64U icp_sal_get_dc_error(Cpa8S dcError);

#endif
