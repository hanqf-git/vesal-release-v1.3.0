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

#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>

#include <memory>

#include "log_setting.h"
#include "metrics.h"
#include "vesal/codec.h"
#include "vesal/cypher.h"
#include "vesal/data_flow.h"
#include "vesal/memory_pool.h"

DECLARE_uint32(vesal_codec_qat_max_in_qat_num);

DECLARE_uint32(vesal_qat_channel_buffer_multiplier);

DECLARE_uint32(vesal_metrics_sample_rate);

DECLARE_bool(vesal_metrics_disable_poller_metrics);

// correspond to the device configuration file i.e. /etc/4xxx*.conf
DECLARE_string(vesal_codec_qat_section_name);

DECLARE_string(vesal_data_flow_dsa_quota);

DECLARE_bool(vesal_log_console_output);

DECLARE_int32(vesal_log_level);

DECLARE_uint32(vesal_log_flush_interval_seconds);

// Ha related
DECLARE_uint32(vesal_qat_ha_min_counting_time_window_us);
DECLARE_uint32(vesal_qat_ha_sliding_time_window_sec);
DECLARE_uint32(vesal_qat_ha_trigger_error_num);

// Event mode related
DECLARE_uint32(vesal_codec_qat_shared_mode_poller_op_process_num);
DECLARE_uint32(vesal_codec_qat_shared_mode_poller_num);
DECLARE_uint32(vesal_codec_qat_shared_mode_poller_sleep_time_us);

#ifdef VESAL_ENABLE_ERR_SIM
DECLARE_bool(vesal_enable_err_sim);
#endif

namespace vesal {

struct InitOptions {
    MemoryPoolInitOption mem_pool_init_opt;
    CodecInitOptions codec_init_opt;
    DataFlowInitOptions data_flow_init_opt;
    CypherInitOptions cypher_init_opt;
    LogSettings settings;
    std::shared_ptr<MetricRegistry> registry = std::make_shared<DefaultMetricRegistry>();
};

// Init global resources and settings: engines, logging, metrics.
// Return true if success, otherwise return false. This API guarantees that the global resources
// and settings are initialized atomically. Caller can retry if failed. If called again after
// successfully init, return true but do nothing.
bool Init(const InitOptions& options);

// Information of Qat devices, which can be used to allocate channels on specified numa or Qat
// device
struct DeviceInfo {
    int32_t device_id;
    int32_t numa_id;
    // TODO(Pinnong.Li): add more information, like workload
};
typedef DeviceInfo QatDeviceInfo;  // To be deprecated. Use DeviceInfo instead.

// Available after successful init() with Qat initialized, get information of Qat devices
std::vector<DeviceInfo> GetQatDeviceInfos();  // To be deprecated

// Available after successful init() with Kae initialized, get information of Kae devices
std::vector<DeviceInfo> GetKaeDeviceInfos();

// Clean resources, shutdown logging
bool Uninit();

}  // namespace vesal
