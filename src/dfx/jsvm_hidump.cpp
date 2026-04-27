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

void IsolateRegistry::UnregisterIsolate(v8::Isolate* isolate)
{
    uint32_t tid = static_cast<uint32_t>(platform::OS::GetTid());
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolatesInThreads_.find(tid);
    if (it != isolatesInThreads_.end() && !it->second.empty()) {
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

// ─── DumpArgs ───────────────────────────────────────────────────────────────
//
// Passed as void* to RequestInterrupt. The heap-allocated DumpArgs outlives
// the stack frame of jsvm_dump_heapsnapshot, and is shared between the main
// thread (sets result after wait), the JS thread (sets done in callback),
// and the forked child process (reads done to begin dump).
//
struct DumpArgs
{
    v8::Isolate* isolate;
    uint32_t tid;
    DumpFormat format;
    DumpStatus result;
    std::mutex mu;
    std::condition_variable cv;
    bool done;

    DumpArgs(v8::Isolate* iso, uint32_t t, DumpFormat fmt)
        : isolate(iso), tid(t), format(fmt),
          result(DumpStatus::SUCCESS), done(false)
    {}
};

// ─── DoDump: performs the actual heap snapshot work ─────────────────────────
//
// Called in the forked child process. The child waits for done=true before
// calling this, so TakeHeapSnapshot runs at a safepoint with no JS frame
// on the stack.
//
static void DoDump(DumpArgs* args)
{
    pid_t parentPid = getppid();
    struct FaultLoggerdRequest request = {};
    request.head.clientType = LOG_FILE_DES_CLIENT;
    request.head.clientPid = parentPid;
    request.pid = parentPid;
    request.type = JSVM_HEAP_SNAPSHOT;
    request.tid = static_cast<int32_t>(args->tid);
    request.time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int32_t fd = RequestFileDescriptorEx(&request);
    if (fd < 0) {
        args->result = DumpStatus::ERR_FD_REQUEST;
        LOG(Error) << "jsvm_dump_heapsnapshot: RequestFileDescriptorEx failed, fd=" << fd;
        return;
    }

    v8::Isolate::Scope isolateScope(args->isolate);
    v8::HandleScope handleScope(args->isolate);
    FdOutputStream stream(fd);

    if (args->format == DumpFormat::RAW_HEAP) {
        LOG(Info) << "jsvm_dump_heapsnapshot: tid=" << args->tid
                  << " RAW_HEAP dump start";
        args->isolate->GetHeapProfiler()->DumpRawHeapSnapshot(&stream);
        LOG(Info) << "jsvm_dump_heapsnapshot: tid=" << args->tid
                  << " RAW_HEAP dump done";
    } else {
        LOG(Info) << "jsvm_dump_heapsnapshot: tid=" << args->tid
                  << " HEAP_SNAPSHOT dump start";
        const v8::HeapSnapshot* snapshot =
            args->isolate->GetHeapProfiler()->TakeHeapSnapshot();
        if (snapshot == nullptr) {
            args->result = DumpStatus::ERR_SERIALIZE;
            LOG(Error) << "jsvm_dump_heapsnapshot: TakeHeapSnapshot returned nullptr";
        } else {
            snapshot->Serialize(&stream);
            const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
            LOG(Info) << "jsvm_dump_heapsnapshot: tid=" << args->tid
                      << " HEAP_SNAPSHOT dump done";
        }
    }
}

// ─── DumpSnapshotCallback: executes on the isolate's owner thread ───────────
//
// Invoked by v8's HandleInterrupts at a safepoint during JS execution.
// This callback is EXTREMELY minimal: it only sets done and forks, then
// returns immediately so the JS thread is NOT blocked. The forked child
// process waits for done, then calls DoDump.
//
// Design rationale:
//   - Fork IN the callback (on the JS thread) so the child process inherits
//     the JS thread's safepoint state: TakeHeapSnapshot is safe to call.
//   - The callback does NOT call DoDump or any slow I/O; it only forks and
//     returns, so JS execution resumes promptly.
//   - The child process (fork parent) does the actual dump in wait → DoDump,
//     which runs fully in the forked process's address space, not touching
//     the JS heap at all.
//
static void DumpSnapshotCallback(v8::Isolate* isolate, void* data)
{
    auto* args = static_cast<DumpArgs*>(data);

    // Idempotency guard: prevent double execution
    {
        std::lock_guard<std::mutex> lock(args->mu);
        if (args->done) {
            return;
        }
        args->done = true;
    }
    args->cv.notify_all();

    // Fork the dump work. Child (pid=0) waits for done and performs the dump.
    // Parent (callback, running on the JS thread) returns immediately so the
    // JS thread is not blocked.
    pid_t pid = fork();
    if (pid < 0) {
        LOG(Error) << "jsvm_dump_heapsnapshot: fork failed, errno=" << errno;
        return;
    }
    if (pid > 0) {
        // Parent (JS thread): fork succeeded, child is doing the dump.
        // Return immediately so JS execution resumes. The child will be
        // reaped by init when it exits.
        return;
    }

    // Child process: wait for done=true, then dump, then exit.
    // NOTE: we do NOT call notify_all() here. The JS thread already notified
    // before forking. This wait just waits for that notification to be visible
    // in this process's memory (which is a copy of the parent's memory).
    {
        std::unique_lock<std::mutex> lock(args->mu);
        args->cv.wait(lock, [args] { return args->done; });
    }

    DoDump(args);
    _exit(args->result == DumpStatus::SUCCESS ? 0 : 1);
}

// ─── DumpSnapshotAsync: dispatch TakeHeapSnapshot to the owner thread ─────────
//
// Main flow:
//   1. Main thread: register callback via RequestInterrupt, then return.
//   2. JS thread: at the next safepoint, HandleInterrupts fires, executing
//      DumpSnapshotCallback.
//   3. Callback: sets done, notifies waiters, forks — then returns immediately.
//   4. JS thread: resumes JS execution, not blocked.
//   5. Fork child: waits for done, then calls DoDump, exits.
//
// The main thread returns SUCCESS as soon as RequestInterrupt is queued.
// This means the caller knows the dump has been initiated (not completed).
// This is the non-blocking guarantee.
//
// Same-thread fallback: if the caller IS the owner thread, we still use
// RequestInterrupt so the interrupt machinery has a chance to run with full
// safepoint preparation. The callback fires synchronously in that case.
//
static DumpStatus DumpSnapshotAsync(uint32_t tid, v8::Isolate* isolate,
                                    DumpFormat format)
{
    auto* args = new DumpArgs(isolate, tid, format);
    isolate->RequestInterrupt(DumpSnapshotCallback, args);
    // Main thread returns immediately. The JS thread does the rest.
    return DumpStatus::SUCCESS;
}

} // namespace jsvm

// ─── Public C API ────────────────────────────────────────────────────────────

extern "C" __attribute__((visibility("default"))) int jsvm_dump_heapsnapshot(
    uint32_t tid, int dumpType)
{
    jsvm::DumpFormat format = static_cast<jsvm::DumpFormat>(dumpType);

    if (tid == 0) {
        // tid == 0: dump all active isolates (top-of-stack for each tid)
        auto allIsolates = jsvm::IsolateRegistry::GetInstance().GetAllIsolatesWithTid();
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
    v8::Isolate* targetIsolate =
        jsvm::IsolateRegistry::GetInstance().GetIsolateByTid(tid);
    if (targetIsolate == nullptr) {
        return static_cast<int>(jsvm::DumpStatus::ERR_NO_ISOLATE);
    }
    return static_cast<int>(
        jsvm::DumpSnapshotAsync(tid, targetIsolate, format));
}
