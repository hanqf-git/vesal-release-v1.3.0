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

#include "vesal/vesal.h"

#include <gflags/gflags.h>

#include <vector>

#include "codec/codec_internal.h"
#ifdef VESAL_ENABLE_KAE
#include "codec/kae/kae_codec.h"
#endif
#include "codec/qat/qat_codec.h"
#include "common/metrics_internal.h"
#include "common/qat/qat_unit_manager.h"
#include "common/qat/qat_util.h"
#include "common/scheduler.h"
#include "cypher/cypher.h"
#include "data_flow/data_flow_resource_manager.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#ifdef VESAL_ENABLE_ERR_SIM
#include "common/err_simulation.h"
#include "common/uds_listener.h"
#endif

DEFINE_uint32(vesal_codec_qat_max_in_qat_num,
              8190,
              "Max concurrency number for one QAT-based channel in async mode.");

DEFINE_bool(vesal_metrics_disable_poller_metrics,
            false,
            "disable polling related metrics or not, false by default");

DEFINE_string(
    vesal_codec_qat_section_name,
    "",
    "qat section name. If not empty, use it to startup qat. Otherwise try default names(SSL0 "
    "~ SSL1024, and SSL)");

DEFINE_bool(vesal_enable_err_sim,
            false,
            "Enable error simulation or not. Performance can get affected. Only works if compiled "
            "with VESAL_ENABLE_ERR_SIM enabled");

DEFINE_uint32(vesal_qat_channel_buffer_multiplier,
              4,
              "When polling slower than submitting, vesal channel can buffer # times of hardware "
              "capacity request results. 4 by default.");

namespace vesal {
#ifdef VESAL_ENABLE_ERR_SIM
std::unique_ptr<UdsListener> g_uds_listener = nullptr;
#endif

bool g_vesal_inited = false;
std::mutex g_vesal_init_mutex;

bool UninitImpl();

bool Init(const InitOptions& options) {
    std::lock_guard<std::mutex> lock(g_vesal_init_mutex);
    if (g_vesal_inited) {
        return true;
    }
    auto __g_uninit_all = defer([]() {
        if (!g_vesal_inited) {
            UninitImpl();
        }
    });
    if (InitLogging(options.settings)) {
        return false;
    }
    g_metric_registry = options.registry;
    g_metric_memcpy_throughput = g_metric_registry->RegisterCounter("vesal.memcpy_throughput", {});
    g_periodic_scheduler.Start();

    // Now DSA and QAT must have memory pool initialized.
    if (!options.mem_pool_init_opt.init_mem_pool &&
        (options.codec_init_opt.init_qat || options.cypher_init_opt.init_qat ||
         options.data_flow_init_opt.init_dsa)) {
        VESAL_LOG(ERROR) << "qat or dsa is enabled, but memory pool is not initialized";
        return false;
    }

    if (options.mem_pool_init_opt.init_mem_pool) {
        if (VESAL_UNLIKELY(!MemoryPool::GetInstance()->Init(options.mem_pool_init_opt))) {
            VESAL_LOG(ERROR) << "Failed to init memory pool";
            return false;
        }
    }

    if (options.codec_init_opt.init_qat || options.cypher_init_opt.init_qat) {
        if (!qat::QatUserStart()) {
            return false;
        }
    }

    bool codec_r = Codec::Init(options.codec_init_opt);
    bool data_flow_r =
        data_flow::DataFlowResourceManager::GetInstance()->Init(options.data_flow_init_opt);
    g_cypher = std::make_unique<Cypher>(options.cypher_init_opt.init_qat);
    bool cypher_r = g_cypher->Start();
#ifdef VESAL_ENABLE_ERR_SIM
    // Currently only support qat codec injection.
    if (options.codec_init_opt.init_qat && codec_r) {
        // This must be initialized after qat codec because it relies on valid qat section name.
        std::string uds_path = GetQatUdsSocketPath(qat::g_vesal_codec_qat_section_name);
        g_uds_listener =
            std::make_unique<UdsListener>(uds_path, ErrSimHandler, VESAL_ERR_SIM_UDS_MSG_LEN);
        g_uds_listener->Start();
        VESAL_LOG(INFO) << "Start qat error simulation listener on " << uds_path;
    }
#endif
    VESAL_LOG(INFO) << "Init vesal, codec_r: " << codec_r << ", data_flow_r: " << data_flow_r
                    << ", cypher_r: " << cypher_r;
    g_vesal_inited = codec_r && data_flow_r && cypher_r;
    return g_vesal_inited;
}

std::vector<DeviceInfo> GetQatDeviceInfos() {
    qat::QatUnitManager* mr = nullptr;
    std::vector<DeviceInfo> infos;
    if (g_qat_codec) {
        mr = g_qat_codec->GetUnitManager();
    } else if (g_cypher) {
        mr = g_cypher->GetUnitManager();
    }
    if (mr) {
        infos = mr->GetQatDeviceInfos();
    }
    return infos;
}

std::vector<DeviceInfo> GetKaeDeviceInfos() {
#ifdef VESAL_ENABLE_KAE
    return g_kae_codec->GetDeviceInfos();
#else
    return {};
#endif
}

bool UninitImpl() {
    if (!Codec::Uninit())
        return false;
    // Must stop first because scheduler accesses many resources. Stop first to avoid accessing
    // destroyed resources.
    g_periodic_scheduler.Stop();
    data_flow::DataFlowResourceManager* rm = data_flow::DataFlowResourceManager::GetInstance();
    rm->Uninit();
    if (g_cypher) {
        g_cypher->Stop();
        g_cypher.reset();
    }
#ifdef VESAL_ENABLE_ERR_SIM
    if (g_uds_listener) {
        g_uds_listener->Stop();
        g_uds_listener.reset();
    }
#endif
    if (!qat::QatUserStop()) {
        return false;
    }
    MemoryPool::GetInstance()->Reset();
    ShutdownLogging(true);
    g_periodic_scheduler.Clear();
    g_vesal_inited = false;
    return true;
}

bool Uninit() {
    std::unique_lock<std::mutex> lg(g_vesal_init_mutex);
    if (!g_vesal_inited) {
        return true;
    }
    return UninitImpl();
}

}  // namespace vesal
