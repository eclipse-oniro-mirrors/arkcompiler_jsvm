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

#ifndef JSVM_COMPAT_H
#define JSVM_COMPAT_H

#include <atomic>
#include <cstdint>
#include <new>

#include "jsvm_env.h"
#include "dfx/jsvm_misuse_reporter.h"
#include "jsvm_scope.h"
#include "jsvm_util.h"
#include "v8.h"

namespace v8impl {

class IsolateAccessScope final {
public:
    IsolateAccessScope(v8::Isolate* isolate, const char* apiName)
    {
        if (UNLIKELY(isolate == nullptr)) {
            return;
        }

        v8::Isolate* current = v8::Isolate::TryGetCurrent();
        if (LIKELY(current == isolate)) {
            return;
        }

        ReportTlsIsolateMismatch(current, apiName);
        CreateGuardScope(isolate, apiName);
    }

    IsolateAccessScope(JSVM_Env env, const char* apiName) : IsolateAccessScope(env->isolate, apiName) {}

    ~IsolateAccessScope()
    {
        if (guarded) {
            GuardScopeStorage()->~GuardScope();
            guarded = false;
        }
    }

    IsolateAccessScope(const IsolateAccessScope&) = delete;
    IsolateAccessScope& operator=(const IsolateAccessScope&) = delete;

    bool IsTlsReady() const
    {
        return !guarded || GuardScopeStorage()->IsTlsReady();
    }

private:
    using GuardScope = IsolateScope<ApiCompatPolicy>;

    void CreateGuardScope(v8::Isolate* isolate, const char* apiName)
    {
        new (GuardScopeStorage()) GuardScope(isolate, apiName);
        guarded = true;
    }

    GuardScope* GuardScopeStorage()
    {
        return reinterpret_cast<GuardScope*>(guardScopeBuf_);
    }

    const GuardScope* GuardScopeStorage() const
    {
        return reinterpret_cast<const GuardScope*>(guardScopeBuf_);
    }

    static void ReportTlsIsolateMismatch(v8::Isolate* current, const char* apiName)
    {
        ApiMisuseKind kind =
            (current == nullptr ? ApiMisuseKind::E01_VM_SCOPE_ABSENT : ApiMisuseKind::E02_VM_SCOPE_MISMATCH);
        ApiMisuseReporter::Report(kind, apiName);
    }

    bool guarded = false;  // GuardScope has been created
    alignas(GuardScope) unsigned char guardScopeBuf_[sizeof(GuardScope)];
};

class ContextAccessScope final {
public:
    ContextAccessScope(JSVM_Env env, const char* apiName) : isolateAccess_(env, apiName)
    {
        if (!isolateAccess_.IsTlsReady()) {
            return;
        }

        new (ContextScopeStorage()) v8::Context::Scope(env->context());
        hasContextScope_ = true;
    }

    ~ContextAccessScope()
    {
        if (hasContextScope_) {
            ContextScopeStorage()->~Scope();
            hasContextScope_ = false;
        }
    }

    ContextAccessScope(const ContextAccessScope&) = delete;
    ContextAccessScope& operator=(const ContextAccessScope&) = delete;

private:
    using Scope = v8::Context::Scope;

    Scope* ContextScopeStorage()
    {
        return reinterpret_cast<Scope*>(contextScopeBuf_);
    }

    IsolateAccessScope isolateAccess_;
    bool hasContextScope_ = false;
    alignas(Scope) unsigned char contextScopeBuf_[sizeof(Scope)];
};

} // namespace v8impl

#define JSVM_COMPAT_CONCAT_INNER(name, line) name##line
#define JSVM_COMPAT_CONCAT(name, line) JSVM_COMPAT_CONCAT_INNER(name, line)

#define JSVM_RAW_ISOLATE_ACCESS(isolate) \
    v8impl::IsolateAccessScope JSVM_COMPAT_CONCAT(isolateAccessScope, __LINE__)((isolate), __func__)

#define JSVM_ISOLATE_ACCESS(env) \
    v8impl::IsolateAccessScope JSVM_COMPAT_CONCAT(isolateAccessScope, __LINE__)((env), __func__)

#define JSVM_CONTEXT_ACCESS(env) \
    v8impl::ContextAccessScope JSVM_COMPAT_CONCAT(contextAccessScope, __LINE__)((env), __func__)

#define JSVM_LOCK_CONTROL_ACCESS(env) static_cast<void>(env)

#define JSVM_NO_V8_ACCESS(env) static_cast<void>(env)

#endif // JSVM_COMPAT_H
