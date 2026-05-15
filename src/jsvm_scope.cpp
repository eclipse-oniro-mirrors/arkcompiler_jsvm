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

const char* ApiName(const char* apiName)
{
    return apiName != nullptr ? apiName : "unknown";
}

} // namespace

uint32_t CurrentThreadId()
{
    thread_local uint32_t tid = static_cast<uint32_t>(platform::OS::GetTid());
    return tid;
}

bool IsolateOwner::TryAcquire(uint32_t currentTid, const char* apiName)
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

bool IsolateOwner::TryRelease(uint32_t currentTid, const char* apiName)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (UNLIKELY(enterDepth_ == 0)) {
        LOG(Error) << "[JSVM Internal] Release isolate owner without matching acquire in "
                   << ApiName(apiName);
        return false;
    }

    if (UNLIKELY(ownerTid_ != currentTid)) {
        LOG(Error) << "[JSVM Internal] Release isolate owner from non-owner thread in "
                   << ApiName(apiName) << ", owner tid=" << ownerTid_ << ", current tid=" << currentTid;
        return false;
    }

    if (--enterDepth_ == 0) {
        ownerTid_ = 0;
    }
    return true;
}

} // namespace v8impl
