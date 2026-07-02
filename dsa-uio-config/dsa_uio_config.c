#include "idxd.h"
#include "user_device.h"
#include "common.h"
#include "dsa_uio_config.h"

struct log_ctx log_ctx;
int InitDsaConfig()
{
    static bool init=false;
    if(init)return 0;
    init=true;
    log_init(&log_ctx, "dsa_perf_micros", "DSA_PERF_MICROS_LOG_LEVEL");
    struct tcfg cfg;
    cfg.nb_user_eng=-1;
    return user_driver_init(&cfg);
}

bool GetNextWorkQueue(struct UioWqInfo* info) {
    if (info) {
        return ud_wq_get_next(info);
    } else {
        ERR("info is NULL");
        return false;
    }
}
void ReturnWorkQueue(struct UioWqInfo* info) {
    if (info && info->portal) {
        ud_wq_unmap(info->portal);
    } else {
        ERR("info is NULL or info->portal is NULL");
    }
}

int EnableDsaWQs()
{
    int enabled_wq_num = 0;
    while(true)
    {
        void* result = ud_wq_get(NULL, -1, 0, -1, false);
        if(!result)break;
        ++enabled_wq_num;
    }
    return enabled_wq_num;
}

void FillBatch(uintptr_t desc_ptr,
                   size_t desc_list_size,
                   uintptr_t desc_list_addr,
                   uintptr_t completion_record_addr) {
        struct dsa_hw_desc* desc = (struct dsa_hw_desc*)desc_ptr;
        desc->opcode = DSA_OPCODE_BATCH;
        desc->flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV;
        desc->desc_count = desc_list_size;
        desc->desc_list_addr = desc_list_addr;
        desc->completion_addr = completion_record_addr;
};

void FillCrc(uintptr_t desc_ptr,
                uintptr_t src,
                uint64_t len,
                uint32_t seed,
                uintptr_t completion_record_addr) {
    struct dsa_hw_desc* desc = (struct dsa_hw_desc*)desc_ptr;
    desc->opcode = DSA_OPCODE_CRCGEN;
    desc->flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV;
    desc->xfer_size = len;
    desc->src_addr = src;
    desc->crc_seed = seed;
    desc->completion_addr = completion_record_addr;
};

void FillCopy(uintptr_t desc_ptr,
                uintptr_t src,
                uint64_t len,
                uintptr_t dst,
                uintptr_t completion_record_addr) {
    struct dsa_hw_desc* desc = (struct dsa_hw_desc*)desc_ptr;
    desc->opcode = DSA_OPCODE_MEMMOVE;
    // IDXD_OP_FLAG_RCR:
    // Request a completion – since we poll on status, this flag
    // must be 1 for status to be updated on successful completion
    // IDXD_OP_FLAG_CRAV:
    // Completion Record Address is Valid
    // IDXD_OP_FLAG_CC:
    // Hint to direct data writes to CPU cache
    // This hint does not affect writing to the completion record,
    // which is always directed to cache
    desc->flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_CC;
    desc->xfer_size = len;
    desc->src_addr = src;
    desc->dst_addr = dst;
    desc->completion_addr = completion_record_addr;
};

void FillCopyWithCrc(uintptr_t desc_ptr,
                        uintptr_t src,
                        uint64_t len,
                        uintptr_t dst,
                        uint32_t seed,
                        uintptr_t completion_record_addr) {
    struct dsa_hw_desc* desc = (struct dsa_hw_desc*)desc_ptr;
    desc->opcode = DSA_OPCODE_COPY_CRC;
    desc->flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_CC;
    desc->xfer_size = len;
    desc->src_addr = src;
    desc->dst_addr = dst;
    desc->crc_seed = seed;
    desc->completion_addr = completion_record_addr;
};

void FillFence(uintptr_t desc_ptr, uintptr_t completion_record_addr) {
    struct dsa_hw_desc* desc = (struct dsa_hw_desc*)desc_ptr;
    desc->opcode = DSA_OPCODE_NOOP;
    desc->flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_FENCE;
    desc->completion_addr = completion_record_addr;
};

size_t GetDsaHwDescSize()
{
    return sizeof(struct dsa_hw_desc);
}

size_t GetDsaCompletionRecordSize()
{
    return sizeof(struct dsa_completion_record);
}

uint32_t GetCrcInDsaCompletionRecord(uintptr_t completion_record_ptr)
{
    struct dsa_completion_record* cr=(struct dsa_completion_record*)completion_record_ptr;
    return (uint32_t)(cr->crc_val);
}

int DsaCheckCompletion(uintptr_t completion_record_ptr)
{
    struct dsa_completion_record* cr=(struct dsa_completion_record*)completion_record_ptr;
    if(cr->status == DSA_COMP_NONE)return DSA_TASK_WIP;
    if(cr->status == DSA_COMP_SUCCESS)return DSA_TASK_SUCCESS;
    return DSA_TASK_FAILURE;
}