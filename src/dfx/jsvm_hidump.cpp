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

#include "jsvm_dfx.h"
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

std::vector<uint32_t> IsolateRegistry::GetAllTids()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> result;
    result.reserve(isolatesInThreads_.size());
    for (auto& pair : isolatesInThreads_) {
        if (!pair.second.empty()) {
            result.emplace_back(pair.first);
        }
    }
    return result;
}

// ─── RequestGc: low-memory GC request ───────────────────────────────────────

static void LowMemoryNotificationAll(const std::vector<v8::Isolate*>& isolates)
{
    for (v8::Isolate* isolate : isolates) {
        // Only the top-of-stack isolate is the thread's current isolate; V8
        // collects an isolate only as the calling thread's current isolate,
        // so enter each target for the duration of its GC.
        v8::Isolate::Scope isolateScope(isolate);
        isolate->LowMemoryNotification();
    }
}

// Runs on the isolate owner's thread at its next interrupt check, where every
// scope it opened (and every isolate snapshotted with it) is still alive.
static void GcInterruptCallback(v8::Isolate* isolate, void* data)
{
    std::unique_ptr<std::vector<v8::Isolate*>> isolates(
        static_cast<std::vector<v8::Isolate*>*>(data));
    // V8 runs the callback on the owner thread with the interrupt target as
    // the current isolate; a mismatch is a context anomaly, not a repairable
    // situation — fail fast instead of GC'ing a foreign isolate.
    CHECK(v8::Isolate::TryGetCurrent() == isolate);
    LowMemoryNotificationAll(*isolates);
}

std::vector<v8::Isolate*> IsolateRegistry::SnapshotIsolatesLocked(uint32_t tid)
{
    std::vector<v8::Isolate*> isolates;
    auto it = isolatesInThreads_.find(tid);
    if (it == isolatesInThreads_.end() || it->second.empty()) {
        return isolates;
    }
    std::stack<v8::Isolate*> tempStack = it->second;
    while (!tempStack.empty()) {
        isolates.push_back(tempStack.top());
        tempStack.pop();
    }
    return isolates;
}

bool IsolateRegistry::RequestGc(uint32_t tid)
{
    // Own thread: the isolate stack is only mutated by the thread itself, so
    // the snapshot stays valid and the (potentially long) GC runs outside the
    // lock.
    if (tid == static_cast<uint32_t>(platform::OS::GetTid())) {
        std::vector<v8::Isolate*> isolates;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            isolates = SnapshotIsolatesLocked(tid);
        }
        if (isolates.empty()) {
            return false;
        }
        LowMemoryNotificationAll(isolates);
        return true;
    }

    // Another thread: V8 requires the GC to run on the isolate's owner thread,
    // so the request is delivered via RequestInterrupt and the GC executes at
    // the owner's next JS safepoint (same delivery model as
    // RequestDumpInterrupt). Snapshot and delivery share one critical section,
    // so the Isolate* cannot race with an UnregisterIsolate. Only the
    // top-of-stack isolate (snapshot front) can be executing JS, so only it
    // has an interrupt check point to receive the request.
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<v8::Isolate*> isolates = SnapshotIsolatesLocked(tid);
    if (isolates.empty()) {
        return false;
    }
    auto pending = std::make_unique<std::vector<v8::Isolate*>>(std::move(isolates));
    pending->front()->RequestInterrupt(GcInterruptCallback, pending.release());
    return true;
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
class DumpContext {
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

// ─── RequestDumpInterrupt: lookup + interrupt delivery in one critical section ─────────
//
// Security note: this replaces the old "GetIsolateByTid returns a raw pointer,
// caller uses it after the lock is released" pattern. Looking the isolate up
// and calling RequestInterrupt() must not interleave with an
// UnregisterIsolate() from a scope close: both take mutex_, so either the
// interrupt is fully delivered before the unregister, or the unregister has
// already completed and the lookup here fails. The output fd is requested by
// the caller before entering this function (outside the registry lock); on
// lookup failure the caller keeps ownership and closes it.

bool IsolateRegistry::RequestDumpInterrupt(uint32_t tid, DumpFormat format, int fd)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolatesInThreads_.find(tid);
    if (it == isolatesInThreads_.end() || it->second.empty()) {
        return false;
    }
    v8::Isolate* isolate = it->second.top();
    auto ctx = std::make_unique<DumpContext>(isolate, tid, format, fd);
    isolate->RequestInterrupt(DumpSnapshotCallback, ctx.get());
    ctx.release();
    return true;
}

} // namespace jsvm

// ─── C API ────────────────────────────────────────────────────────────

extern "C" __attribute__((visibility("default"))) int jsvm_dump_heapsnapshot(
    uint32_t tid, int dumpType)
{
    jsvm::DumpFormat format = static_cast<jsvm::DumpFormat>(dumpType);

    if (tid == 0) {
        auto allTids = jsvm::IsolateRegistry::GetInstance().GetAllTids();
        if (allTids.empty()) {
            return -1;
        }
        for (uint32_t isoTid : allTids) {
            int fd = jsvm::RequestOutputFd(isoTid);
            if (fd < 0) {
                return fd;
            }
            if (!jsvm::IsolateRegistry::GetInstance().RequestDumpInterrupt(isoTid, format, fd)) {
                close(fd);
                return -1;
            }
        }
        return 0;
    }

    int fd = jsvm::RequestOutputFd(tid);
    if (fd < 0) {
        return fd;
    }
    if (!jsvm::IsolateRegistry::GetInstance().RequestDumpInterrupt(tid, format, fd)) {
        close(fd);
        return -1;
    }
    return 0;
}
