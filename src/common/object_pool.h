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

#include <stddef.h>  // offsetof()
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <vector>

#include "vesal/log_setting.h"

namespace vesal {
#define KB(x) (static_cast<size_t>(x) << 10)

extern void* DefaultAllocate(size_t size);
extern void DefaultDeallocate(void* addr);

static const size_t FREE_CHUNK_SIZE = 128;         // max size of _full_free_chunks in ThreadPool
static const size_t MIN_CHUNK_SIZE = 2048;         // min chunk size
static const size_t MAX_CHUNK_SIZE = (32 * 1024);  // max chunk size, the max alignment size is 32K,
                                                   // so MAX_CHUNK_SIZE only can be 32K
static const size_t k_bit_shift = 11;  // used to calculate allocated chunk size located in
                                       // k_size_class which one, cannot be modified
static constexpr size_t k_size_class[5] = {KB(2), KB(4), KB(8), KB(16), KB(32)};

template <typename T> struct ChunkMaxSize { static const size_t value = MAX_CHUNK_SIZE; };

template <typename T> struct ObjectMaxNumInChunk { static const size_t value = 256; };

template <typename T> class CalObjectNumInChunk {
    static const size_t N = ChunkMaxSize<T>::value / sizeof(T);

public:
    static const size_t value =
        (N > ObjectMaxNumInChunk<T>::value ? ObjectMaxNumInChunk<T>::value : N);
};

static inline int GetThreadId() {
    thread_local pid_t tid = 0;
    if (tid == 0) {
        tid = ::syscall(SYS_gettid);
    }
    return tid;
}

template <typename T> struct FreeObjectCache {
    FreeObjectCache() : free_num(0) {}

    uint64_t free_num;
    T* free_obj[0];
};

template <typename T> struct LocalChunk {
    static_assert(sizeof(T) >= sizeof(uint64_t), "The local pool object is too small");

    static const size_t OBJECT_SIZE = sizeof(T);

    LocalChunk() : start_addr(nullptr), used_num(0), free_num(0), tid(0) {}

    T* alloc_object() {
        return new (start_addr + (used_num++) * OBJECT_SIZE) T;
    }

    char* start_addr;
    uint16_t used_num;
    uint16_t free_num;
    int32_t tid;
};

// TlsObjectPool is not thead-safe, it can be only used in single thread to get object.
template <typename T> class TlsObjectPool {
public:
    static T* get_local_object() {
        TlsObjectPool* lp = singleton();
        if (VESAL_UNLIKELY(!lp)) {
            VESAL_LOG(ERROR) << "invoke SingleThreadPool get_local_object() at an exiting thread.";
            return nullptr;
        }
        return lp->get_or_new_object();
    }

    static void return_local_object(T* obj) {
        if (VESAL_UNLIKELY(!obj)) {
            return;
        }
        TlsObjectPool* lp = singleton();
        if (VESAL_LIKELY(lp)) {
            lp->return_or_free_object(obj);
        } else {
            recycle_object(obj);
        }
    }

    static void set_object_max_size(size_t max_size) {
        OBJ_SIZE_IN_OBJECT_CACHE = max_size;
    }

private:
    TlsObjectPool() : _current_chunk(nullptr) {
        VESAL_CHECK(sizeof(T) <= (MAX_CHUNK_SIZE - sizeof(LocalChunk<T>)))
            << "object size is lager than MAX_CHUNK_SIZE - sizeof(ChunkHeader)), MAX_CHUNK_SIZE: "
            << MAX_CHUNK_SIZE << ", sizeof(ChunkHeader): " << sizeof(LocalChunk<T>)
            << ", sizeof(T): " << sizeof(T);
        _thread_id = GetThreadId();
        // calculate the actually used chunk size
        int n = (CalObjectNumInChunk<T>::value * sizeof(T) - 1) >> k_bit_shift;
        // find the position of the highest bit of a binary number
        int index = n == 0 ? 0 : (sizeof(n) * 8 - 1) - __builtin_clz(n << 1);
        _chunk_size = k_size_class[index];
        _object_num = (_chunk_size - sizeof(LocalChunk<T>)) / sizeof(T);

        _free_obj_cache = (FreeObjectCache<T>*)malloc(offsetof(FreeObjectCache<T>, free_obj) +
                                                      sizeof(T*) * OBJ_SIZE_IN_OBJECT_CACHE);
        _free_obj_cache->free_num = 0;
    }

    ~TlsObjectPool() {
        g_single_thread_pool_shutdown = true;
        if (_current_chunk && _current_chunk->free_num == _current_chunk->used_num) {
            DefaultDeallocate(_current_chunk);
        }
        for (uint64_t i = 0; i < _free_obj_cache->free_num; ++i) {
            LocalChunk<T>* chunk = address(_free_obj_cache->free_obj[i]);
            if (++chunk->free_num == chunk->used_num) {
                DefaultDeallocate(chunk);
            }
        }
        free(_free_obj_cache);
        for (size_t i = 0; i < _full_free_chunks.size(); ++i) {
            DefaultDeallocate(_full_free_chunks[i]);
        }
    }

    static TlsObjectPool* singleton() {
        if (VESAL_UNLIKELY(g_single_thread_pool_shutdown)) {
            return nullptr;
        }
        static thread_local TlsObjectPool<T> tls_pool;
        return &tls_pool;
    }

    T* get_or_new_object() {
        if (_free_obj_cache->free_num) {
            return _free_obj_cache->free_obj[--_free_obj_cache->free_num];
        }
        if (_current_chunk && _current_chunk->used_num < _object_num) {
            return _current_chunk->alloc_object();
        }
        if (!_full_free_chunks.empty()) {
            _current_chunk = _full_free_chunks.back();
            _full_free_chunks.pop_back();
            return _current_chunk->alloc_object();
        }
        char* mem = reinterpret_cast<char*>(DefaultAllocate(_chunk_size));
        if (VESAL_UNLIKELY(!mem)) {
            return nullptr;
        }
        LocalChunk<T>* chunk = new (mem) LocalChunk<T>;
        chunk->start_addr = mem + sizeof(LocalChunk<T>);
        chunk->tid = GetThreadId();
        _current_chunk = chunk;
        return _current_chunk->alloc_object();
    }

    void return_or_free_object(T* obj) {
        LocalChunk<T>* chunk = address(obj);
        VESAL_CHECK(chunk->tid == _thread_id)
            << "try to free object which is not allocated in current thread, chunk->tid="
            << chunk->tid << ", _thread_id=" << _thread_id;
        if (_free_obj_cache->free_num < OBJ_SIZE_IN_OBJECT_CACHE) {
            _free_obj_cache->free_obj[_free_obj_cache->free_num++] = obj;
            return;
        }
        if (++chunk->free_num == _object_num) {
            chunk->free_num = 0;
            chunk->used_num = 0;
            if (_current_chunk == chunk) {
                return;
            } else if (_full_free_chunks.size() < FREE_CHUNK_SIZE) {
                _full_free_chunks.push_back(chunk);
                return;
            } else {
                DefaultDeallocate(chunk);
                return;
            }
        }
    }

    static void recycle_object(T* obj) {
        LocalChunk<T>* chunk = address(obj);
        VESAL_CHECK(chunk->tid == GetThreadId())
            << "try free object which is not allocated in current thread";
        if (++chunk->free_num == chunk->used_num) {
            DefaultDeallocate(chunk);
        }
    }

    static LocalChunk<T>* address(T* obj) {
        uint64_t base = reinterpret_cast<uint64_t>(obj) & (~(_chunk_size - 1));
        return reinterpret_cast<LocalChunk<T>*>(base);
    }

    static thread_local bool g_single_thread_pool_shutdown;

    // free_obj arrary size in FreeObjectCache
    static size_t OBJ_SIZE_IN_OBJECT_CACHE;

private:
    int32_t _thread_id;
    static size_t _chunk_size;  // allocate chunk size
    static size_t _object_num;  // object number of one chunk
    LocalChunk<T>* _current_chunk;
    FreeObjectCache<T>* _free_obj_cache;
    std::vector<LocalChunk<T>*> _full_free_chunks;
};

template <typename T> size_t TlsObjectPool<T>::_chunk_size(0);

template <typename T> size_t TlsObjectPool<T>::_object_num(0);

template <typename T> size_t TlsObjectPool<T>::OBJ_SIZE_IN_OBJECT_CACHE(8192);

template <typename T> thread_local bool TlsObjectPool<T>::g_single_thread_pool_shutdown = false;

/* Usage:
    Alloc object and return ptr to user. The api is not thread-safe,
    it should be used to get thread local object.
   Note:
    The object size must be in [8bytes ~ (64K-24bytes)], 24 bytes is
    LocalChunk header size.
    After get the object, user need fill its parameters, because
    it is likely to get from cache and parameters keep the last data.
*/
template <typename T> inline T* get_tls_object() {
    return TlsObjectPool<T>::get_local_object();
}

/* Usage:
    Return object which get by get_tls_object to cache. The api is not thread-safe.
   Note:
    Before return the object, user need clear the important parameters of object,
    because the object is likely to return to cache and not destruct.
*/
template <typename T> inline void return_tls_object(T* obj) {
    TlsObjectPool<T>::return_local_object(obj);
}

/* Usage:
    Update object max number of FreeObjectCache in TlsObjectPool. The default value is 16K.
   Note:
    The api need to be called before use get_tls_object or return_tls_object,
    otherwise it will not take effect.
*/
template <typename T> inline void set_tls_pool_object_max_size(size_t max_size) {
    TlsObjectPool<T>::set_object_max_size(max_size);
}

}  // namespace vesal

#define VESAL_OBJECT_POOL_FRIENDS                            \
    template <typename T> friend T* vesal::get_tls_object(); \
    template <typename T> friend void vesal::return_tls_object(T*);
