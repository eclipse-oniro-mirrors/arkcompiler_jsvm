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
#include <condition_variable>
#include <sys/syscall.h>

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

void IsolateRegistry::UnregisterIsolate(v8::Isolate* isolate)
{
    uint32_t tid = static_cast<uint32_t>(platform::OS::GetTid());
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolatesInThreads_.find(tid);
    if (it != isolatesInThreads_.end() && !it->second.empty()) {
        // Safety check: the top-of-stack should match the isolate being unregistered
        if (it->second.top() == isolate) {
            it->second.pop();
        }
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

std::vector<v8::Isolate*> IsolateRegistry::GetAllIsolates()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<v8::Isolate*> allIsolates;
    for (auto& pair : isolatesInThreads_) {
        if (!pair.second.empty()) {
            allIsolates.push_back(pair.second.top());
        }
    }
    return allIsolates;
}

std::vector<std::pair<uint32_t, v8::Isolate*>> IsolateRegistry::GetAllIsolatesWithTid()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<uint32_t, v8::Isolate*>> result;
    for (auto& pair : isolatesInThreads_) {
        if (!pair.second.empty()) {
            result.emplace_back(pair.first, pair.second.top());
        }
    }
    return result;
}

// ─── FdOutputStream ─────────────────────────────────────────────────────────

class FdOutputStream : public v8::OutputStream
{
public:
    explicit FdOutputStream(int fd) : fd_(fd) {}
    WriteResult WriteAsciiChunk(char* data, int size) override
    {
        ssize_t written = write(fd_, data, static_cast<size_t>(size));
        return (written == static_cast<ssize_t>(size)) ? kContinue : kAbort;
    }
    void EndOfStream() override
    {
        close(fd_);
        fd_ = -1;
    }
private:
    int fd_;
};

// ─── DumpContext: heap-allocated bridge for async RequestInterrupt ───────────
//
// CRITICAL: DumpContext MUST be heap-allocated. RequestInterrupt queues the
// callback asynchronously; the context must outlive the stack frame of the
// caller. A mutex+condition_variable synchronizes the caller and callback.
//
struct DumpContext
{
    v8::Isolate* isolate;
    uint32_t tid;
    DumpFormat format;
    DumpStatus result;
    std::mutex mu;
    std::condition_variable cv;
    bool done;

    DumpContext(v8::Isolate* iso, uint32_t t, DumpFormat fmt)
        : isolate(iso), tid(t), format(fmt),
          result(DumpStatus::SUCCESS), done(false)
    {}
};

// ─── DumpSnapshotCallback: executes on the isolate's owner thread ──────────
//
// Invoked by v8's HandleInterrupts at a safepoint during JS execution.
// The isolate is already entered (no Isolate::Scope needed).
// A HandleScope is already set up by InvokeApiInterruptCallbacks, but we
// create an explicit one for safety.
//
// NOTE: This callback may be called more than once if the interrupt is
// queued and also invoked directly via the same-thread fallback path.
// The idempotency check prevents duplicate work.
//
static void DumpSnapshotCallback(v8::Isolate* isolate, void* data)
{
    auto* ctx = static_cast<DumpContext*>(data);

    // Idempotency guard: prevent double execution
    {
        std::lock_guard<std::mutex> lock(ctx->mu);
        if (ctx->done) {
            return;
        }
    }

    // Request fd from faultloggerd
    struct FaultLoggerdRequest request = {};
    request.head.clientType = LOG_FILE_DES_CLIENT;
    request.head.clientPid = getpid();
    request.pid = getpid();
    request.type = JSVM_HEAP_SNAPSHOT;
    request.tid = static_cast<int32_t>(ctx->tid);
    request.time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int32_t fd = RequestFileDescriptorEx(&request);
    if (fd < 0) {
        ctx->result = DumpStatus::ERR_FD_REQUEST;
    } else {
        v8::HandleScope handleScope(isolate);
        FdOutputStream stream(fd);

        if (ctx->format == DumpFormat::RAW_HEAP) {
            // Raw heap dump format
            // NOTE: DumpRawHeapSnapshot must be available in the v8 build.
            isolate->GetHeapProfiler()->DumpRawHeapSnapshot(&stream);
        } else {
            // Standard heap snapshot format
            const v8::HeapSnapshot* snapshot =
                isolate->GetHeapProfiler()->TakeHeapSnapshot();
            if (snapshot == nullptr) {
                ctx->result = DumpStatus::ERR_SERIALIZE;
            } else {
                snapshot->Serialize(&stream);
                // Release snapshot resources (stream closes fd in EndOfStream)
                const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
            }
        }
    }

    // Signal the caller that we're done
    {
        std::lock_guard<std::mutex> lock(ctx->mu);
        ctx->done = true;
    }
    ctx->cv.notify_all();
}

// ─── DumpSnapshotAsync: dispatch TakeHeapSnapshot to the owner thread ─────────
//
// Strategy:
//   - Cross-thread (main scenario): queue via RequestInterrupt, then cv.wait().
//     The owner thread hits an interrupt check during JS execution, callback
//     fires at a safepoint, signals done.
//   - Same-thread (safety net): also queue via RequestInterrupt so the interrupt
//     path has a chance to run with full safepoint preparation. Then do a direct
//     call as fallback (safe because we ARE the owner thread). The callback's
//     idempotency guard prevents double execution.
//
static DumpStatus DumpSnapshotAsync(uint32_t tid, v8::Isolate* isolate,
                                    DumpFormat format)
{
    uint32_t currentTid = static_cast<uint32_t>(platform::OS::GetTid());
    bool isSameThread = (currentTid == tid);

    // Heap-allocated context: RequestInterrupt is fully asynchronous
    auto* ctx = new DumpContext(isolate, tid, format);
    isolate->RequestInterrupt(DumpSnapshotCallback, ctx);

    if (isSameThread) {
        // Same thread: directly invoke the callback with proper scoping.
        // This is safe because we ARE the isolate's owner thread.
        // The idempotency guard in the callback prevents double execution
        // if the interrupt queue also fires it.
        v8::Isolate::Scope isolateScope(isolate);
        DumpSnapshotCallback(isolate, ctx);
    } else {
        // Cross-thread: wait for the owner thread to process the interrupt
        std::unique_lock<std::mutex> lock(ctx->mu);
        ctx->cv.wait(lock, [ctx] { return ctx->done; });
    }

    DumpStatus result = ctx->result;
    delete ctx;
    return result;
}

} // namespace jsvm

// ─── Public C API ────────────────────────────────────────────────────────────

extern "C" __attribute__((visibility("default"))) int jsvm_dump_heapsnapshot(
    uint32_t tid, int dumpType)
{
    auto& registry = jsvm::IsolateRegistry::GetInstance();
    jsvm::DumpFormat format = static_cast<jsvm::DumpFormat>(dumpType);

    if (tid == 0) {
        // tid == 0: dump all active isolates (top-of-stack for each tid)
        auto allIsolates = registry.GetAllIsolatesWithTid();
        if (allIsolates.empty()) {
            return static_cast<int>(jsvm::DumpStatus::ERR_NO_ISOLATE);
        }
        for (auto& [isoTid, isolate] : allIsolates) {
            jsvm::DumpStatus status =
                jsvm::DumpSnapshotAsync(isoTid, isolate, format);
            if (status != jsvm::DumpStatus::SUCCESS) {
                return static_cast<int>(status);
            }
        }
        return static_cast<int>(jsvm::DumpStatus::SUCCESS);
    }

    // tid != 0: dump a specific isolate
    v8::Isolate* targetIsolate = registry.GetIsolateByTid(tid);
    if (targetIsolate == nullptr) {
        return static_cast<int>(jsvm::DumpStatus::ERR_NO_ISOLATE);
    }
    return static_cast<int>(jsvm::DumpSnapshotAsync(tid, targetIsolate, format));
}
