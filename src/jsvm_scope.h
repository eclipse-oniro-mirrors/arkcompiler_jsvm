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

#ifndef JSVM_SCOPE_H
#define JSVM_SCOPE_H

#include <cstdint>
#include <mutex>

#include "jsvm.h"
#include "dfx/jsvm_misuse_reporter.h"
#include "jsvm_util.h"

namespace v8impl {

// Controls whether an IsolateScope must enter the isolate or can reuse the
// already-current TLS isolate.
enum class IsolateEnterMode {
    ALWAYS_ENTER,      // Always enter the isolate
    ENTER_IF_MISMATCH, // Enter the isolate only if the given isolate is mismatched with TLS isolate
};

// Returns the OS-visible thread id used in diagnostics. The implementation
// caches the value per thread to avoid a syscall on hot scope paths.
uint32_t CurrentThreadId();

// Tracks the thread that owns JSVM-created v8::Isolate::Scope entries for a
// single isolate. It is intentionally small: one owner tid plus a re-entrant
// depth counter protected by a mutex.
//
// This is a JSVM-side misuse detector, not a V8 isolate lock. A successful
// acquire only means JSVM will allow this thread to construct an
// v8::Isolate::Scope; it does not make arbitrary concurrent V8 access safe.
class IsolateOwner final {
public:
    // Acquires ownership for the current thread. Re-entrant acquisition from the
    // same thread is allowed and increments enterDepth_.
    bool TryAcquire(uint32_t currentTid, const char* apiName);

    // Releases one ownership level. Only the owning thread may release it.
    bool TryRelease(uint32_t currentTid, const char* apiName);

    uint32_t GetTid() const {
        return ownerTid_;
    }

    IsolateOwner(const IsolateOwner&) = delete;
    IsolateOwner& operator=(const IsolateOwner&) = delete;

    IsolateOwner() = default;
    ~IsolateOwner() = default;

private:
    // Protects ownerTid_ and enterDepth_. Isolate ownership can be observed from
    // different JSVM API entry threads.
    mutable std::mutex mutex_;

    // OS-visible tid of the thread that opened the outermost JSVM-created
    // Isolate::Scope. Zero means there is currently no owner.
    uint32_t ownerTid_ = 0;

    // Re-entrant ownership depth for ownerTid_. A zero depth means no owner.
    uint32_t enterDepth_ = 0;
};

IsolateOwner* GetIsolateOwner(v8::Isolate* isolate);

struct VmScopePolicy final {
    // Explicit VM scopes must preserve V8 Enter/Exit nesting even when the TLS
    // isolate already matches the target isolate.
    static constexpr IsolateEnterMode enterMode = IsolateEnterMode::ALWAYS_ENTER;
};

struct ApiCompatPolicy final {
    // API compatibility scopes only repair a missing or mismatched TLS isolate.
    // If the target isolate is already current, the fast path does not enter.
    static constexpr IsolateEnterMode enterMode = IsolateEnterMode::ENTER_IF_MISMATCH;
};

// Thin RAII wrapper around v8::Isolate::Scope with JSVM-side owner tracking.
//
// Policy controls whether the wrapper must always enter the isolate or may use
// the current TLS isolate as a fast path. Construction failure is recorded
// internally through entered_/canAccessV8_; regular JSVM API access scopes do
// not propagate a new public error code from this wrapper.
template<typename Policy>
class IsolateScope final {
public:
    explicit IsolateScope(v8::Isolate* isolate, const char* apiName = nullptr)
        : isolate_(isolate),
          owner_(GetIsolateOwner(isolate)),
          apiName_(apiName),
          scopeTid_(CurrentThreadId())
    {
        if (UNLIKELY(isolate_ == nullptr || owner_ == nullptr)) {
            return;
        }

        if (!ShouldEnter()) {
            // Fast path for API compatibility scopes. No new v8::Isolate::Scope
            // is created, but V8 access is valid because TLS already matches.
            tlsReady_ = true;
            return;
        }

        uint32_t currentTid = scopeTid_;
        if (UNLIKELY(!owner_->TryAcquire(currentTid, apiName_))) {
            // Another thread owns this isolate in the JSVM tracker. Do not force
            // v8::Isolate::Enter here; doing so can turn a latent misuse into a
            // deterministic V8 CHECK failure.
            ApiMisuseReporter::Report(ApiMisuseKind::E03_VM_SCOPE_CROSS_THREAD, apiName, owner_->GetTid(), currentTid);
            return;
        }

        // The owner tracker must be acquired before entering V8 so a racing
        // thread can observe and reject cross-thread isolate enter attempts.
        new (ScopeStorage()) v8::Isolate::Scope(isolate_);
        entered_ = true;
        tlsReady_ = true;
    }

    ~IsolateScope()
    {
        if (!entered_) {
            return;
        }
        ScopeStorage()->~Scope();
        uint32_t currentTid = CurrentThreadId();
        owner_->TryRelease(currentTid, apiName_);
        entered_ = false;
    }

    IsolateScope(const IsolateScope&) = delete;
    IsolateScope& operator=(const IsolateScope&) = delete;

    bool Entered() const
    {
        return entered_;
    }

    // Get the tid at the moment the isolate scope is opened
    uint32_t GetOpenTid() const
    {
        return scopeTid_;
    }

    // Get the isolate at the moment the isolate scope is opened
    v8::Isolate* GetOpenIsolate() const
    {
        return isolate_;
    }

    bool IsTlsReady() const
    {
        return tlsReady_;
    }

private:
    using Scope = v8::Isolate::Scope;

    bool ShouldEnter() const
    {
        if constexpr (Policy::enterMode == IsolateEnterMode::ALWAYS_ENTER) {
            return true;
        }

        if constexpr (Policy::enterMode == IsolateEnterMode::ENTER_IF_MISMATCH) {
            return v8::Isolate::TryGetCurrent() != isolate_;
        }

        return false;
    }

    Scope* ScopeStorage()
    {
        return reinterpret_cast<Scope*>(isolateScopeStorage_);
    }

    v8::Isolate* isolate_ = nullptr;

    // Cached owner tracker. The destructor uses this pointer instead of
    // resolving isolate data again, because isolate embedder data may already be
    // changing during shutdown paths.
    IsolateOwner* owner_ = nullptr;

    // API name used only for diagnostics. It is normally a static __func__
    // pointer, so storing it does not allocate.
    const char* apiName_ = nullptr;

    // OS-visible tid that constructed this scope wrapper. Explicit close paths
    // use it to detect cross-thread scope lifecycle misuse.
    uint32_t scopeTid_ = 0;

    // True only when this object actually constructed v8::Isolate::Scope.
    bool entered_ = false;

    // True when the current thread can safely access v8 TLS isolate: either
    // TLS already matched the isolate or this wrapper entered the isolate.
    bool tlsReady_ = false;

    // Inline storage for v8::Isolate::Scope. It is constructed only when
    // entered_ becomes true.
    alignas(Scope) unsigned char isolateScopeStorage_[sizeof(Scope)];
};

} // namespace v8impl

#endif // JSVM_SCOPE_H
