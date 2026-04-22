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

#include "jsvm_hidump.h"

#include <unistd.h>
#include <chrono>
#include <sys/syscall.h>

#include "platform/platform.h"
#include "v8-profiler.h"
#include "faultloggerd_client.h"

namespace jsvm {

IsolateRegistry& IsolateRegistry::GetInstance()
{
    static IsolateRegistry instance;
    return instance;
}

void IsolateRegistry::RegisterIsolate(v8::Isolate* isolate)
{
    uint32_t tid = static_cast<uint32_t>(platform::OS::GetTid());
    std::lock_guard<std::mutex> lock(mutex_);
    isolateMap_[tid] = isolate;
}

void IsolateRegistry::UnregisterIsolate(v8::Isolate* isolate)
{
    uint32_t tid = static_cast<uint32_t>(platform::OS::GetTid());
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolateMap_.find(tid);
    if (it != isolateMap_.end()) {
        isolateMap_.erase(it);
    }
}

v8::Isolate* IsolateRegistry::GetIsolateByTid(uint32_t tid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolateMap_.find(tid);
    if (it != isolateMap_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<v8::Isolate*> IsolateRegistry::GetAllIsolates()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<v8::Isolate*> allIsolates;
    for (auto& pair : isolateMap_) {
        allIsolates.push_back(pair.second);
    }
    return allIsolates;
}

std::vector<std::pair<uint32_t, v8::Isolate*>> IsolateRegistry::GetAllIsolatesWithTid()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<uint32_t, v8::Isolate*>> result;
    for (auto& pair : isolateMap_) {
        result.push_back(pair);
    }
    return result;
}

class FdOutputStream : public v8::OutputStream
{
public:
    explicit FdOutputStream(int fd) : fd_(fd) {}
    WriteResult WriteAsciiChunk(char* data, int size) override
    {
        ssize_t written = write(fd_, data, static_cast<size_t>(size));
        return (written == size) ? kContinue : kAbort;
    }
    void EndOfStream() override
    {
        close(fd_);
        fd_ = -1;
    }
private:
    int fd_;
};

static DumpStatus DumpSnapshot(uint32_t tid, v8::Isolate* isolate)
{
    struct FaultLoggerdRequest request = {};
    request.head.clientType = LOG_FILE_DES_CLIENT;
    request.head.clientPid = getpid();
    request.pid = getpid();
    request.type = JSVM_HEAP_SNAPSHOT;
    request.tid = static_cast<int32_t>(tid);
    request.time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int32_t fd = RequestFileDescriptorEx(&request);
    if (fd < 0) {
        return DumpStatus::ERR_FD_REQUEST;
    }
    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope handleScope(isolate);
    FdOutputStream stream(fd);
    auto snapshot = isolate->GetHeapProfiler()->TakeHeapSnapshot();
    if (snapshot == nullptr) {
        return DumpStatus::ERR_SERIALIZE;
    }
    snapshot->Serialize(&stream);
    return DumpStatus::SUCCESS;
}

} // namespace jsvm

extern "C" __attribute__((visibility("default"))) int jsvm_dump_heapsnapshot(uint32_t tid)
{
    auto& registry = jsvm::IsolateRegistry::GetInstance();

    if (tid == 0) {
        // tid == 0: Dump all active isolates
        auto allIsolates = registry.GetAllIsolatesWithTid();
        if (allIsolates.empty()) {
            return static_cast<int>(jsvm::DumpStatus::ERR_NO_ISOLATE);
        }
        for (auto& [isoTid, isolate] : allIsolates) {
            jsvm::DumpStatus status = jsvm::DumpSnapshot(isoTid, isolate);
            if (status != jsvm::DumpStatus::SUCCESS) {
                return static_cast<int>(status);
            }
        }
        return static_cast<int>(jsvm::DumpStatus::SUCCESS);
    }

    v8::Isolate* targetIsolate = registry.GetIsolateByTid(tid);
    if (targetIsolate == nullptr) {
        return static_cast<int>(jsvm::DumpStatus::ERR_NO_ISOLATE);
    }
    return static_cast<int>(jsvm::DumpSnapshot(tid, targetIsolate));
}
