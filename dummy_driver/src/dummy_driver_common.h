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

#include <dlfcn.h>
#include <stdio.h>

#define LIKELY(x) __builtin_expect((x), 1)
#define UNLIKELY(x) __builtin_expect((x), 0)

#define STRINGIFY(x) #x

// convert operation name to function pointer name
#define FUNC_PTR(_func) internal_##_func

#define MAKE_WRAPPER_FUNC_COMMON(_func, _ret, ...)      \
    static _ret (*FUNC_PTR(_func))(__VA_ARGS__) = NULL; \
    extern _ret _func(__VA_ARGS__)

// macro to load symbol, assume teardown label exists
#define LOAD_SYM_COMMON(_handle, _func)                                        \
    do {                                                                       \
        *(void**)(&FUNC_PTR(_func)) = _load_symbol(_handle, STRINGIFY(_func)); \
        if (FUNC_PTR(_func) == NULL)                                           \
            goto teardown;                                                     \
    } while (0)

// macro to load symbol, but do not teardown when sym is not found
#define LOAD_SYM_OPTIONAL_COMMON(_handle, _func) \
    *(void**)(&FUNC_PTR(_func)) = _load_symbol(_handle, STRINGIFY(_func))

#ifndef NDEBUG
#define DLOG(...)                                       \
    do {                                                \
        fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__);                   \
        fflush(stderr);                                 \
    } while (0)
#else
// no-ops when NDEBUG is defined
#define DLOG(...) \
    do {          \
    } while (0)
#endif

// QAT driver has no versioning
static inline void* _load_symbol(void* handle, const char* symbol) {
    void* ptr = dlsym(handle, symbol);
    if (ptr == NULL)
        DLOG("Warn: %s not found\n", symbol);
    return ptr;
}
