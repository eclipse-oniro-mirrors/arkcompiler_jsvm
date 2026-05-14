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

#ifndef JSVM_API_ENTER_H
#define JSVM_API_ENTER_H

#include "jsvm.h"
#include "jsvm_env.h"
#include "jsvm_compat.h"
#include "jsvm_util.h"

// ============================================================================
// Compile switches (can be overridden via -D on the command line)
// ============================================================================

#ifndef JSVM_ENABLE_API_ACCESS_SCOPE
#define JSVM_ENABLE_API_ACCESS_SCOPE 1
#endif

#ifndef JSVM_ENABLE_API_TRACE
#define JSVM_ENABLE_API_TRACE 0
#endif

#ifndef JSVM_ENABLE_API_ACCESS_AUDIT
#define JSVM_ENABLE_API_ACCESS_AUDIT 0
#endif

// ============================================================================
// AccessKind enum
// ============================================================================

enum JsvmApiAccessKind {
    K_JSVM_ACCESS_NO_V8,              // pure C/C++ metadata, no V8 touch
    K_JSVM_ACCESS_V8_GLOBAL,          // calls V8 global/static but no TLS isolate
    K_JSVM_ACCESS_V8_NO_TLS_ISOLATE,  // touches V8 data, but must not use TLS isolate
    K_JSVM_ACCESS_LOCK_CONTROL,       // manages JSVM explicit lock state
    K_JSVM_ACCESS_SCOPE_LIFECYCLE,    // opens/closes JSVM/V8 scope lifetime objects
    K_JSVM_ACCESS_V8_ISOLATE,         // accesses V8 isolate/handle, no context required
    K_JSVM_ACCESS_V8_CONTEXT,         // accesses V8 isolate + context, no JS preamble
    K_JSVM_ACCESS_JS_RUNTIME,         // full JS runtime preamble (replaces JSVM_PREAMBLE)
};

// ============================================================================
// Trace (no-op by default)
// ============================================================================

#if JSVM_ENABLE_API_TRACE
#define JSVM_API_TRACE(name, ...) \
    platform::RunJsTrace JSVM_COMPAT_CONCAT(traceScope, __LINE__)((name), ##__VA_ARGS__)
#define JSVM_API_TRACE_ENTER(apiName, ...) \
    JSVM_API_TRACE((apiName), ##__VA_ARGS__)
#define JSVM_API_TRACE_STATE(phase, ...)                                       \
    do {                                                                       \
        JSVM_API_TRACE(__func__, "phase", (phase), ##__VA_ARGS__);             \
    } while (0)
#else
#define JSVM_API_TRACE(name, ...) static_cast<void>(0)
#define JSVM_API_TRACE_ENTER(apiName, ...) static_cast<void>(0)
#define JSVM_API_TRACE_STATE(phase, ...) static_cast<void>(0)
#endif

// ============================================================================
// Traits
// ============================================================================

namespace v8impl {

// Forward declaration — full definition is in js_native_api_v8.h (after this header).
class TryCatch;

class JsvmNoTryCatch final {
public:
    explicit JsvmNoTryCatch(JSVM_Env) {}
    bool HasCaught() const
    {
        return false;
    }
};

template<JsvmApiAccessKind Kind>
struct JsvmApiEnterTraits {
    using TryCatchType = JsvmNoTryCatch;
    static constexpr bool NEEDS_ENV = true;
    static constexpr bool NEEDS_VM = false;
};

template<>
struct JsvmApiEnterTraits<K_JSVM_ACCESS_JS_RUNTIME> {
    using TryCatchType = TryCatch;
    static constexpr bool NEEDS_ENV = true;
    static constexpr bool NEEDS_VM = false;
};

// ============================================================================
// Precheck — preserves old JSVM_PREAMBLE ordering for JS_RUNTIME
// ============================================================================

template<JsvmApiAccessKind Kind>
inline JSVM_Status JsvmApiEnterPrecheck(JSVM_Env env)
{
    static_cast<void>(env);
    return JSVM_OK;
}

template<>
inline JSVM_Status JsvmApiEnterPrecheck<K_JSVM_ACCESS_JS_RUNTIME>(JSVM_Env env)
{
    RETURN_STATUS_IF_FALSE(env, env->lastException.IsEmpty(), JSVM_PENDING_EXCEPTION);
    RETURN_STATUS_IF_FALSE(env, env->CanCallIntoJS(),
                           (env->GetVersion() == JSVM_VERSION_EXPERIMENTAL ? JSVM_CANNOT_RUN_JS
                                                                            : JSVM_PENDING_EXCEPTION));
    ClearLastError(env);
    return JSVM_OK;
}

// ============================================================================
// Enter scope classes
// ============================================================================

template<JsvmApiAccessKind Kind>
class JsvmApiEnterScope final {
public:
    JsvmApiEnterScope(JSVM_Env env, const char* apiName)
    {
        static_cast<void>(env);
        static_cast<void>(apiName);
    }

    JsvmApiEnterScope(const JsvmApiEnterScope&) = delete;
    JsvmApiEnterScope& operator=(const JsvmApiEnterScope&) = delete;
};

#if JSVM_ENABLE_API_ACCESS_SCOPE
template<>
class JsvmApiEnterScope<K_JSVM_ACCESS_V8_ISOLATE> final {
public:
    JsvmApiEnterScope(JSVM_Env env, const char* apiName) : scope_(env, apiName) {}

    JsvmApiEnterScope(const JsvmApiEnterScope&) = delete;
    JsvmApiEnterScope& operator=(const JsvmApiEnterScope&) = delete;

private:
    IsolateAccessScope scope_;
};

template<>
class JsvmApiEnterScope<K_JSVM_ACCESS_V8_CONTEXT> final {
public:
    JsvmApiEnterScope(JSVM_Env env, const char* apiName) : scope_(env, apiName) {}

    JsvmApiEnterScope(const JsvmApiEnterScope&) = delete;
    JsvmApiEnterScope& operator=(const JsvmApiEnterScope&) = delete;

private:
    ContextAccessScope scope_;
};

template<>
class JsvmApiEnterScope<K_JSVM_ACCESS_JS_RUNTIME> final {
public:
    JsvmApiEnterScope(JSVM_Env env, const char* apiName) : scope_(env, apiName) {}

    JsvmApiEnterScope(const JsvmApiEnterScope&) = delete;
    JsvmApiEnterScope& operator=(const JsvmApiEnterScope&) = delete;

private:
    ContextAccessScope scope_;
};
#endif // JSVM_ENABLE_API_ACCESS_SCOPE

template<JsvmApiAccessKind Kind>
class JsvmApiVmEnterScope final {
public:
    JsvmApiVmEnterScope(JSVM_VM vm, const char* apiName)
    {
        static_assert(Kind != K_JSVM_ACCESS_V8_CONTEXT, "VM API cannot use context access");
        static_assert(Kind != K_JSVM_ACCESS_JS_RUNTIME, "VM API cannot use JS runtime access");
        static_cast<void>(vm);
        static_cast<void>(apiName);
    }

    JsvmApiVmEnterScope(const JsvmApiVmEnterScope&) = delete;
    JsvmApiVmEnterScope& operator=(const JsvmApiVmEnterScope&) = delete;
};

#if JSVM_ENABLE_API_ACCESS_SCOPE
template<>
class JsvmApiVmEnterScope<K_JSVM_ACCESS_V8_ISOLATE> final {
public:
    JsvmApiVmEnterScope(JSVM_VM vm, const char* apiName)
        : scope_(reinterpret_cast<v8::Isolate*>(vm), apiName)
    {}

    JsvmApiVmEnterScope(const JsvmApiVmEnterScope&) = delete;
    JsvmApiVmEnterScope& operator=(const JsvmApiVmEnterScope&) = delete;

private:
    IsolateAccessScope scope_;
};
#endif // JSVM_ENABLE_API_ACCESS_SCOPE

template<JsvmApiAccessKind Kind>
class JsvmApiGlobalEnterScope final {
public:
    explicit JsvmApiGlobalEnterScope(const char* apiName)
    {
        static_assert(Kind == K_JSVM_ACCESS_NO_V8 || Kind == K_JSVM_ACCESS_V8_GLOBAL,
                      "Global API can only use no-v8 or V8 global access");
        static_cast<void>(apiName);
    }
};

} // namespace v8impl

// ============================================================================
// Public entry macros
// NOTE: JSVM_API_ENTER is not wrapped in do..while — tryCatch must have
// function scope so that CHECK_*_WITH_PREAMBLE and GET_RETURN_STATUS work.
// ============================================================================

// env-based API entry
#define JSVM_API_ENTER(env, accessKind, ...)                                                                    \
    if (UNLIKELY((env) == nullptr)) {                                                                           \
        return JSVM_INVALID_ARG;                                                                                \
    }                                                                                                           \
    JSVM_API_TRACE_ENTER(__func__, "env", (env), ##__VA_ARGS__);                                               \
    do {                                                                                                        \
        JSVM_Status jsvmApiEnterStatus = v8impl::JsvmApiEnterPrecheck<accessKind>((env));                       \
        if (UNLIKELY(jsvmApiEnterStatus != JSVM_OK)) {                                                          \
            return jsvmApiEnterStatus;                                                                          \
        }                                                                                                       \
    } while (0);                                                                                                \
    v8impl::JsvmApiEnterScope<accessKind> JSVM_COMPAT_CONCAT(jsvmApiEnterScope, __LINE__)((env), __func__);     \
    typename v8impl::JsvmApiEnterTraits<accessKind>::TryCatchType tryCatch((env))

// vm-based API entry
#define JSVM_API_ENTER_VM(vm, accessKind, ...)                                                                  \
    if (UNLIKELY((vm) == nullptr)) {                                                                            \
        return JSVM_INVALID_ARG;                                                                                \
    }                                                                                                           \
    JSVM_API_TRACE_ENTER(__func__, "vm", (vm), ##__VA_ARGS__);                                                 \
    v8impl::JsvmApiVmEnterScope<accessKind> JSVM_COMPAT_CONCAT(jsvmApiVmEnterScope, __LINE__)((vm), __func__)

// global API entry (no env/vm)
#define JSVM_API_ENTER_GLOBAL(accessKind, ...)                                                                  \
    JSVM_API_TRACE_ENTER(__func__, ##__VA_ARGS__);                                                             \
    v8impl::JsvmApiGlobalEnterScope<accessKind> JSVM_COMPAT_CONCAT(jsvmApiGlobalEnterScope, __LINE__)(__func__)

#endif // JSVM_API_ENTER_H
