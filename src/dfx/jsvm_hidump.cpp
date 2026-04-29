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
#include <memory>
#include <sys/wait.h>

#include "jsvm_log.h"
#include "platform/platform.h"
#include "v8-profiler.h"
#include "faultloggerd_client.h"

namespace jsvm {

// ─── IsolateRegistry ────────────────────────────────────────────────────────

IsolateRegistry& IsolateRegistry::GetInstance()
{
    static IsolateRegistry instance;
    return instance;
}

void IsolateRegistry::RegisterIsolate(v8::Isolate* isolate)
{
    uint32_t tid = static_cast<uint32_t>(platform::OS::GetTid());
    std::lock_guard<std::mutex> lock(mutex_);
    isolatesInThreads_[tid].push(isolate);
}

void IsolateRegistry::UnregisterIsolate()
{
    uint32_t tid = static_cast<uint32_t>(platform::OS::GetTid());
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolatesInThreads_.find(tid);
    if (it != isolatesInThreads_.end() && !it->second.empty()) {
        it->second.pop();
        if (it->second.empty()) {
            isolatesInThreads_.erase(it);
        }
    }
}

v8::Isolate* IsolateRegistry::GetIsolateByTid(uint32_t tid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolatesInThreads_.find(tid);
    if (it != isolatesInThreads_.end() && !it->second.empty()) {
        return it->second.top();
    }
    return nullptr;
}

std::vector<std::pair<uint32_t, v8::Isolate*>> IsolateRegistry::GetAllIsolatesWithTid()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<uint32_t, v8::Isolate*>> result;
    result.reserve(isolatesInThreads_.size());
    for (auto& pair : isolatesInThreads_) {
        if (!pair.second.empty()) {
            result.emplace_back(pair.first, pair.second.top());
        }
    }
    return result;
}

// ─── FdOutputStream ─────────────────────────────────────────────────────────

class FdOutputStream : public v8::OutputStream {
public:
    explicit FdOutputStream(int fd) : fd_(fd) {}
    WriteResult WriteAsciiChunk(char* data, int size) override
    {
        ssize_t written = write(fd_, data, static_cast<size_t>(size));
        return (written == static_cast<ssize_t>(size)) ? kContinue : kAbort;
    }
    void EndOfStream() override {}
private:
    int fd_;
};

// ─── DumpContext ────────────────────────────────────────────────────────────
//
// Owns the heap dump context including the output fd. The destructor closes
// the fd so it is never leaked, regardless of which code path exits.
//
class DumpContext
{
public:
    DumpContext(v8::Isolate* iso, uint32_t t, DumpFormat fmt, int fileDescriptor)
        : isolate_(iso), tid_(t), format_(fmt), fd_(fileDescriptor), done_(false)
    {}

    ~DumpContext()
    {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    v8::Isolate* isolate() const { return isolate_; }
    uint32_t tid() const { return tid_; }
    DumpFormat format() const { return format_; }
    int fd() const { return fd_; }

    bool MarkDone()
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (done_) {
            return false;
        }
        done_ = true;
        return true;
    }

private:
    v8::Isolate* isolate_;
    uint32_t tid_;
    DumpFormat format_;
    int fd_;
    std::mutex mu_;
    bool done_;
};

// Request an output fd from faultloggerd. Returns fd on success, negative errno on failure.
// Must be called on the main thread (not inside a v8 interrupt callback).
static int RequestOutputFd(uint32_t targetTid)
{
    struct FaultLoggerdRequest req = {};
    req.head.clientType = LOG_FILE_DES_CLIENT;
    req.head.clientPid = static_cast<int32_t>(platform::OS::GetPid());
    req.pid = req.head.clientPid;
    req.type = JSVM_HEAP_SNAPSHOT;
    req.tid = static_cast<int32_t>(targetTid);
    req.time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return RequestFileDescriptorEx(&req);
}

// ─── DoDump: performs the actual heap snapshot work ─────────────────────────

static void DoDump(DumpContext* ctx)
{
    if (ctx->fd() < 0) {
        LOG(Error) << "jsvm_dump_heapsnapshot: invalid fd=" << ctx->fd();
        return;
    }

    v8::Isolate::Scope isolateScope(ctx->isolate());
    v8::HandleScope handleScope(ctx->isolate());
    FdOutputStream stream(ctx->fd());

    if (ctx->format() == DumpFormat::RAW_HEAP) {
        LOG(Info) << "jsvm_dump_heapsnapshot: tid=" << ctx->tid()
                  << " RAW_HEAP dump start";
        ctx->isolate()->GetHeapProfiler()->DumpRawHeapSnapshot(&stream);
        LOG(Info) << "jsvm_dump_heapsnapshot: tid=" << ctx->tid()
                  << " RAW_HEAP dump done";
    } else {
        LOG(Info) << "jsvm_dump_heapsnapshot: tid=" << ctx->tid()
                  << " HEAP_SNAPSHOT dump start";
        const v8::HeapSnapshot* snapshot =
            ctx->isolate()->GetHeapProfiler()->TakeHeapSnapshot();
        if (snapshot == nullptr) {
            LOG(Error) << "jsvm_dump_heapsnapshot: TakeHeapSnapshot returned nullptr";
            return;
        }
        snapshot->Serialize(&stream);
        const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
        LOG(Info) << "jsvm_dump_heapsnapshot: tid=" << ctx->tid()
                  << " HEAP_SNAPSHOT dump done";
    }
}

// ─── DumpSnapshotCallback: executes on the isolate's owner thread ───────────

static void DumpSnapshotCallback(v8::Isolate* isolate, void* data)
{
    std::unique_ptr<DumpContext> ctx(static_cast<DumpContext*>(data));

    if (!ctx->MarkDone()) {
        return;  // ~DumpContext closes fd
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG(Error) << "jsvm_dump_heapsnapshot: fork failed, errno=" << errno;
        return;  // ~DumpContext closes fd
    }
    if (pid > 0) {
        return;  // Parent: ~DumpContext closes fd
    }

    // Child: done=true was set before fork, proceed directly.
    DoDump(ctx.get());
    ctx.reset();  // ~DumpContext closes fd
    _exit(0);
}

// ─── DumpSnapshotAsync: dispatch TakeHeapSnapshot to the owner thread ─────────

static int DumpSnapshotAsync(v8::Isolate* isolate, DumpFormat format, uint32_t tid)
{
    int fd = RequestOutputFd(tid);
    if (fd < 0) {
        return fd;
    }
    auto* ctx = new DumpContext(isolate, tid, format, fd);
    isolate->RequestInterrupt(DumpSnapshotCallback, ctx);
    return 0;
}

} // namespace jsvm

// ─── C API ────────────────────────────────────────────────────────────

extern "C" __attribute__((visibility("default"))) int jsvm_dump_heapsnapshot(
    uint32_t tid, int dumpType)
{
    jsvm::DumpFormat format = static_cast<jsvm::DumpFormat>(dumpType);

    if (tid == 0) {
        auto allIsolates = jsvm::IsolateRegistry::GetInstance().GetAllIsolatesWithTid();
        if (allIsolates.empty()) {
            return -1;
        }
        for (auto& [isoTid, isolate] : allIsolates) {
            int ret = jsvm::DumpSnapshotAsync(isolate, format, isoTid);
            if (ret != 0) {
                return ret;
            }
        }
        return 0;
    }

    v8::Isolate* targetIsolate =
        jsvm::IsolateRegistry::GetInstance().GetIsolateByTid(tid);
    if (targetIsolate == nullptr) {
        return -1;
    }
    return jsvm::DumpSnapshotAsync(targetIsolate, format, tid);
}
