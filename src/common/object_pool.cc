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

#include <stdlib.h>

#include <cerrno>

#include "vesal/log_setting.h"

namespace vesal {

void* DefaultAllocate(size_t size) {
    void* vaddr = nullptr;
    int r = posix_memalign(&vaddr, size, size);
    VESAL_DCHECK(r == 0) << "posix_memalign failed, errno=" << errno;
    return vaddr;
}

void DefaultDeallocate(void* addr) {
    free(addr);
}

}  // namespace vesal
