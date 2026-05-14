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

#include "jsvm_scope.h"

#include "jsvm_log.h"
#include "platform/platform.h"

namespace v8impl {
namespace {

constexpr const char* SCOPE_LOG_PREFIX = "[JSVMScope] ";

const char* ApiName(const char* apiName)
{
    return apiName != nullptr ? apiName : "unknown";
}

} // namespace

uint64_t CurrentThreadId()
{
    thread_local uint64_t tid = platform::OS::GetTid();
    return tid;
}

void ReportScopeError(ScopeErrorKind kind, const char* apiName, uint64_t ownerTid, uint64_t currentTid)
{
    switch (kind) {
        case ScopeErrorKind::CROSS_THREAD_ISOLATE_ENTER:
            LOG(Error) << SCOPE_LOG_PREFIX << "Reject cross-thread isolate enter in " << ApiName(apiName)
                       << ", owner tid=" << ownerTid << ", current tid=" << currentTid;
            return;
        case ScopeErrorKind::VM_SCOPE_OPEN_FAILED:
            LOG(Error) << SCOPE_LOG_PREFIX << "Open VM scope failed in " << ApiName(apiName);
            return;
        case ScopeErrorKind::TLS_ISOLATE_NULL:
            LOG(Error) << SCOPE_LOG_PREFIX << "TLS isolate is null in " << ApiName(apiName);
            return;
        case ScopeErrorKind::TLS_ISOLATE_MISMATCH:
            LOG(Error) << SCOPE_LOG_PREFIX << "TLS isolate mismatch in " << ApiName(apiName);
            return;
        case ScopeErrorKind::RELEASE_WITHOUT_ACQUIRE:
            LOG(Error) << SCOPE_LOG_PREFIX << "Release isolate owner without matching acquire in "
                       << ApiName(apiName);
            return;
        case ScopeErrorKind::RELEASE_FROM_NON_OWNER_THREAD:
            LOG(Error) << SCOPE_LOG_PREFIX << "Release isolate owner from non-owner thread in " << ApiName(apiName)
                       << ", owner tid=" << ownerTid << ", current tid=" << currentTid;
            return;
        case ScopeErrorKind::VM_SCOPE_CLOSE_ON_DIFFERENT_THREAD:
            LOG(Error) << SCOPE_LOG_PREFIX << "Close VM scope from a different thread in " << ApiName(apiName)
                       << ", owner tid=" << ownerTid << ", current tid=" << currentTid;
            return;
        case ScopeErrorKind::VM_SCOPE_CLOSE_WITH_DIFFERENT_VM:
            LOG(Error) << SCOPE_LOG_PREFIX << "Close VM scope with a different VM in " << ApiName(apiName);
            return;
        case ScopeErrorKind::ENV_SCOPE_CLOSE_ON_DIFFERENT_THREAD:
            LOG(Error) << SCOPE_LOG_PREFIX << "Close env scope from a different thread in " << ApiName(apiName)
                       << ", owner tid=" << ownerTid << ", current tid=" << currentTid;
            return;
    }
    LOG(Error) << SCOPE_LOG_PREFIX << "Unknown scope error in " << ApiName(apiName);
}

bool IsolateOwner::TryAcquire(uint64_t currentTid, const char* apiName)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (enterDepth_ == 0) {
        ownerTid_ = currentTid;
        enterDepth_ = 1;
        return true;
    }

    if (LIKELY(ownerTid_ == currentTid)) {
        ++enterDepth_;
        return true;
    }

    return false;
}

bool IsolateOwner::TryRelease(uint64_t currentTid, const char* apiName)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (UNLIKELY(enterDepth_ == 0)) {
        ReportScopeError(ScopeErrorKind::RELEASE_WITHOUT_ACQUIRE, apiName);
        return false;
    }

    if (UNLIKELY(ownerTid_ != currentTid)) {
        ReportScopeError(ScopeErrorKind::RELEASE_FROM_NON_OWNER_THREAD, apiName, ownerTid_, currentTid);
        return false;
    }

    if (--enterDepth_ == 0) {
        ownerTid_ = 0;
    }
    return true;
}

} // namespace v8impl
