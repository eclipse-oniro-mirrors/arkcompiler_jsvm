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
#include <unordered_map>
#include <vector>
#include "v8-isolate.h"

namespace jsvm {

enum class DumpStatus : int {
    SUCCESS = 0,
    ERR_FD_REQUEST = 1,
    ERR_NO_ISOLATE = 2,
    ERR_SERIALIZE = 3,
};

class IsolateRegistry
{
public:
    static IsolateRegistry& GetInstance();

    void RegisterIsolate(v8::Isolate* isolate);
    void UnregisterIsolate(v8::Isolate* isolate);

    v8::Isolate* GetIsolateByTid(uint32_t tid);
    std::vector<v8::Isolate*> GetAllIsolates();
    std::vector<std::pair<uint32_t, v8::Isolate*>> GetAllIsolatesWithTid();

private:
    IsolateRegistry() = default;
    ~IsolateRegistry() = default;

    std::mutex mutex_;
    std::unordered_map<uint32_t, v8::Isolate*> isolateMap_;
};

} // namespace jsvm

extern "C" __attribute__((visibility("default"))) int jsvm_dump_heapsnapshot(uint32_t tid);

#endif // JSVM_HIDUMP_H
