#ifndef __DSA_UIO_CONFIG_H__
#define __DSA_UIO_CONFIG_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "uio_wq_info.h"

#define DSA_TASK_WIP 0
#define DSA_TASK_SUCCESS 1
#define DSA_TASK_FAILURE -1

int InitDsaConfig();
// return true if success, and info is filled
// otherwise no more work queue available
bool GetNextWorkQueue(struct UioWqInfo* info);
void ReturnWorkQueue(struct UioWqInfo* info);
int EnableDsaWQs();

void FillBatch(uintptr_t desc,
                   size_t desc_list_size,
                   uintptr_t desc_list_addr,
                   uintptr_t completion_record_addr);

void FillCrc(uintptr_t desc,
              uintptr_t src,
              uint64_t len,
              uint32_t seed,
              uintptr_t completion_record_addr);

void FillCopy(uintptr_t desc,
              uintptr_t src,
              uint64_t len,
              uintptr_t dst,
              uintptr_t completion_record_addr);

void FillCopyWithCrc(uintptr_t desc,
                      uintptr_t src,
                      uint64_t len,
                      uintptr_t dst,
                      uint32_t seed,
                      uintptr_t completion_record_addr);

void FillFence(uintptr_t desc, uintptr_t completion_record_addr);

size_t GetDsaHwDescSize();

size_t GetDsaCompletionRecordSize();

uint32_t GetCrcInDsaCompletionRecord(uintptr_t completion_record_ptr);

int DsaCheckCompletion(uintptr_t dsa_completion_record_ptr);

#endif