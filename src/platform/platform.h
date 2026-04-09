/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#ifndef JSVM_PLATFORM_H
#define JSVM_PLATFORM_H
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <securec.h>
#include <string>

#ifdef TARGET_OHOS
#include "platform/platform_ohos.h"
#define OHOS_CALL(api_call) api_call
#define OHOS_SELECT(expr_ohos, expr_non_ohos) expr_ohos
#else
#define OHOS_CALL(api_call)
#define OHOS_SELECT(expr_ohos, expr_non_ohos) expr_non_ohos
#endif

// Track first-time API usage per process. When a JSVM API annotated with this
// macro is called for the first time, the API name is reported via HiSysEvent.
// Uses std::call_once for thread safety and zero overhead after first call.
// __func__ is captured outside the lambda to ensure it resolves to the
// enclosing function name (the JSVM API), not the lambda's operator().
#ifdef TARGET_OHOS
#define JSVM_TRACK_API_USE()                                                                             \
    do {                                                                                                 \
        static std::once_flag _jsvm_api_track_flag;                                                      \
        const char* _jsvm_api_name = __func__;                                                           \
        std::call_once(_jsvm_api_track_flag,                                                             \
                       [_jsvm_api_name]() { platform::ohos::WriteAPIUseToHisysevent(_jsvm_api_name); }); \
    } while (0)
#else
#define JSVM_TRACK_API_USE() ((void)0)
#endif

namespace platform {
class OS {
public:
    [[noreturn]] static void Abort();
    static uint64_t GetUid();
    static uint64_t GetPid();
    static uint64_t GetTid();

    enum class LogLevel : uint64_t { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };

    static void Print(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));
    static void PrintString(LogLevel level, const char* string);
};

class RunJsTrace {
public:
    explicit RunJsTrace(bool runJs);
    explicit RunJsTrace(const char* name);

    ~RunJsTrace();

private:
    bool runJs;
};
} // namespace platform

#endif
