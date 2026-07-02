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

#include "common/object_pool.h"

#include <gtest/gtest.h>
#include <valgrind/valgrind.h>

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace vesal {

struct TestLocalObject {
    int64_t a;
    int32_t b;
    TestLocalObject* ptr;
    int32_t c[5];
    char d[10];
};

static TestLocalObject* (*GetTlsObject)();
static void (*FreeTlsObject)(TestLocalObject*);
// only check alloc TestObject addr with cache addr at the first time
static bool need_check = true;

size_t GetPageSize() {
    int n =
        (vesal::CalObjectNumInChunk<TestLocalObject>::value * sizeof(TestLocalObject) - 1) >> 11;
    int index = n == 0 ? 0 : (sizeof(n) * 8 - 1) - __builtin_clz(n << 1);
    size_t pg_size = vesal::k_size_class[index];
    return pg_size;
}

/* page size is 16k, TestObject size is 56bytes, every chunk allocate object num is 292.
   chunk1~chunk4 used to test object cache and current chunk, from (1) to (4).
   chunk5 used to test return chunk to free chunk cache and get object from chunk cache, for (6)(7).
   chunk6 used to test get object from current chunk, for (7).
   test is aim to FreeObjectCache size is 1024
*/
void LocalPoolObjectTest() {
    GetTlsObject = vesal::get_tls_object<TestLocalObject>;
    FreeTlsObject = vesal::return_tls_object<TestLocalObject>;
    vesal::set_tls_pool_object_max_size<TestLocalObject>(1024);

    size_t pg_size = GetPageSize();
    int32_t cache_size = 1024;
    int32_t num = (pg_size - sizeof(vesal::LocalChunk<TestLocalObject>)) / sizeof(TestLocalObject);
    std::vector<TestLocalObject*> local_obj;
    std::vector<TestLocalObject*> check_cache_obj;
    std::vector<TestLocalObject*> check_free_chunk_obj;
    std::vector<TestLocalObject*> check_current_chunk_obj;
    // (1) get object from create new chunk
    for (int32_t i = 0; i < cache_size; ++i) {
        TestLocalObject* ptr = GetTlsObject();
        EXPECT_NE(ptr, nullptr);
        local_obj.push_back(ptr);
    }
    // (2) return object to cache
    for (int32_t i = 0; i < cache_size; ++i) {
        FreeTlsObject(local_obj[i]);
        check_cache_obj.push_back(local_obj[i]);
    }
    local_obj.clear();
    // (3) get object from cache
    int32_t index = check_cache_obj.size() - 1;
    for (int32_t i = 0; i < cache_size; ++i) {
        TestLocalObject* ptr = GetTlsObject();
        if (need_check) {
            EXPECT_EQ(ptr, check_cache_obj[index--]);
        } else {
            EXPECT_NE(ptr, nullptr);
        }
        local_obj.push_back(ptr);
    }

    check_cache_obj.clear();
    // (4) get object from current chunk
    for (int32_t i = cache_size; i < 4 * num; ++i) {
        TestLocalObject* ptr = GetTlsObject();
        EXPECT_NE(ptr, nullptr);
        local_obj.push_back(ptr);
    }
    // (5) get object from create new chunk
    for (int32_t i = 0; i < 2 * num; ++i) {
        TestLocalObject* ptr = GetTlsObject();
        EXPECT_NE(ptr, nullptr);
        local_obj.push_back(ptr);
    }
    EXPECT_EQ(local_obj.size(), 6 * num);
    // (6) return object to cache; after cache is full,
    // return object to chunk and return chunk to chunk cache
    for (int32_t i = 0; i < cache_size; ++i) {
        FreeTlsObject(local_obj[i]);
        check_cache_obj.push_back(local_obj[i]);
    }
    for (int32_t i = cache_size; i < 4 * num; ++i) {
        FreeTlsObject(local_obj[i]);
    }
    for (int32_t i = 4 * num; i < 5 * num; ++i) {
        FreeTlsObject(local_obj[i]);
        check_free_chunk_obj.push_back(local_obj[i]);
    }
    for (int32_t i = 5 * num; i < 6 * num; ++i) {
        FreeTlsObject(local_obj[i]);
        check_current_chunk_obj.push_back(local_obj[i]);
    }
    local_obj.clear();
    // (7) get object from chunk which is saved in chunk cache
    index = cache_size - 1;
    for (int32_t i = 0; i < cache_size; ++i) {
        TestLocalObject* ptr = GetTlsObject();
        if (need_check) {
            EXPECT_EQ(ptr, check_cache_obj[index--]);
        } else {
            EXPECT_NE(ptr, nullptr);
        }
        local_obj.push_back(ptr);
    }
    for (int32_t i = 0; i < num; ++i) {
        TestLocalObject* ptr = GetTlsObject();
        if (need_check) {
            EXPECT_EQ(ptr, check_current_chunk_obj[i]);
        } else {
            EXPECT_NE(ptr, nullptr);
        }
        local_obj.push_back(ptr);
    }
    for (int32_t i = 0; i < num; ++i) {
        TestLocalObject* ptr = GetTlsObject();
        if (need_check) {
            EXPECT_EQ(ptr, check_free_chunk_obj[i]);
        } else {
            EXPECT_NE(ptr, nullptr);
        }
        local_obj.push_back(ptr);
    }
    // (8) return all object to check no memory leak when thread exit
    for (size_t i = 0; i < local_obj.size(); ++i) {
        FreeTlsObject(local_obj[i]);
    }
    local_obj.clear();
    check_cache_obj.clear();
    check_free_chunk_obj.clear();
    check_current_chunk_obj.clear();
}

TEST(ObjectPoolTest, single_thread_local_pool) {
    LocalPoolObjectTest();
}

TEST(ObjectPoolTest, multiple_thread_local_pool) {
    std::vector<std::unique_ptr<std::thread>> tids;
    int32_t num = 5;
    for (int32_t i = 0; i < num; ++i) {
        std::unique_ptr<std::thread> tid(new std::thread(LocalPoolObjectTest));
        tids.push_back(std::move(tid));
    }
    for (size_t i = 0; i < tids.size(); ++i) {
        tids[i]->join();
    }
    need_check = false;
}

TEST(ObjectPoolTest, InvokeWhileShutdown) {
    TlsObjectPool<uint64_t>::g_single_thread_pool_shutdown = true;
    uint64_t* p = get_tls_object<uint64_t>();
    EXPECT_EQ(p, nullptr);
    return_tls_object<uint64_t>(p);
    TlsObjectPool<uint64_t>::g_single_thread_pool_shutdown = false;
}

TEST(ObjectPoolTest, TryFreeObjFromOtherThread) {
#if defined(RUNNING_ON_VALGRIND)
    GTEST_SKIP() << "Skipping EXPECT_DEATH under Valgrind";
#else
    uint64_t* p = get_tls_object<uint64_t>();
    EXPECT_NE(p, nullptr);
    std::thread t([&p]() { EXPECT_DEATH(return_tls_object<uint64_t>(p), ".*"); });
    t.join();
    return_tls_object<uint64_t>(p);
#endif
}

static const int kSize = 8000;
struct Obj {
    int arr[kSize];
};
// Test cross thread using is legal, as long as the object is allocated and freed in the same
// thread.
TEST(ObjectPoolTest, CrossThreadUsing) {
    int thread_num = 64;
    std::vector<std::thread> threads;
    std::vector<Obj*> objs;
    for (int i = 0; i < thread_num; ++i) {
        objs.push_back(get_tls_object<Obj>());
    }
    for (int i = 0; i < thread_num; ++i) {
        threads.emplace_back([&objs, i]() {
            for (int j = 0; j < kSize; ++j) {
                objs[i]->arr[j] = j;
            }
        });
    }
    for (int i = 0; i < thread_num; ++i) {
        threads[i].join();
    }
    for (int i = 0; i < thread_num; ++i) {
        for (int j = 0; j < kSize; ++j) {
            EXPECT_EQ(objs[i]->arr[j], j);
        }
    }
    for (int i = 0; i < thread_num; ++i) {
        return_tls_object(objs[i]);
    }
}

}  // namespace vesal
