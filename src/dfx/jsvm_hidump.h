/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef JSVM_HIDUMP_H
#define JSVM_HIDUMP_H

#include <mutex>
#include <stack>
#include <unordered_map>
#include <vector>
#include "v8-isolate.h"

namespace jsvm {

enum class DumpFormat : int {
    HEAP_SNAPSHOT = 0,    // Standard heap snapshot (default)
    RAW_HEAP = 1,          // Raw heap dump
};

class IsolateRegistry {
public:
    static IsolateRegistry& GetInstance();

    // Register an isolate for the current thread (called from OpenVMScope).
    // Pushes onto the per-thread stack. Multiple isolates per thread
    // are supported; the top-of-stack isolate is considered "active".
    void RegisterIsolate(v8::Isolate* isolate);

    // Unregister the current thread's top-of-stack isolate.
    void UnregisterIsolate();

    // Does not hand out an Isolate* to callers: looks the isolate up and
    // delivers the dump interrupt inside the same critical section, so the
    // lookup-to-RequestInterrupt sequence cannot interleave with an
    // UnregisterIsolate from a scope close. The caller must have obtained
    // the output fd beforehand (outside the registry lock) and owns the fd
    // until this returns; on failure the caller must close it.
    bool RequestDumpInterrupt(uint32_t tid, DumpFormat format, int fd);

    // Returns the tids that currently have an active isolate. The batch dump
    // path copies only tids, never Isolate pointers, out of the lock.
    std::vector<uint32_t> GetAllTids();

private:
    IsolateRegistry() = default;
    ~IsolateRegistry() = default;
    IsolateRegistry(const IsolateRegistry&) = delete;
    IsolateRegistry& operator=(const IsolateRegistry&) = delete;

    std::mutex mutex_;
    // Per-thread stack of isolates, top = active isolate
    std::unordered_map<uint32_t, std::stack<v8::Isolate*>> isolatesInThreads_;
};

} // namespace jsvm

#endif // JSVM_HIDUMP_H
