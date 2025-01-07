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

#include "platform.h"

// v8 header
#include "v8.h"

// OHOS API header
#include "hilog/log.h"
#include "hitrace_meter.h"
#include "init_param.h"
#include "res_sched_client.h"
#include "unistd.h"
#ifdef ENABLE_HISYSEVENT
#include "hisysevent.h"
#endif

namespace platform {
void OS::Abort()
{
    std::abort();
}

uint64_t OS::GetUid()
{
    return static_cast<uint64_t>(getuid());
}

uint64_t OS::GetPid()
{
    return static_cast<uint64_t>(getprocpid());
}

uint64_t OS::GetTid()
{
    return static_cast<uint64_t>(getproctid());
}

#ifdef LOG_DOMAIN
#undef LOG_DOMAIN
#endif
#ifdef LOG_TAG
#undef LOG_TAG
#endif

#define LOG_DOMAIN 0xD003900
#define LOG_TAG "JSVM"

void OS::PrintString(LogLevel level, const char* string)
{
    // convert platform defined LogLevel to hilog LogLevel
    static constexpr ::LogLevel convertArray[] = { ::LogLevel::LOG_DEBUG, ::LogLevel::LOG_INFO, ::LogLevel::LOG_WARN,
                                                   ::LogLevel::LOG_ERROR, ::LogLevel::LOG_FATAL };
    static_assert(sizeof(convertArray) / sizeof(::LogLevel) == static_cast<uint64_t>(OS::LogLevel::LOG_FATAL) + 1);

    HiLogPrint(LOG_APP, convertArray[static_cast<uint64_t>(level)], LOG_DOMAIN, LOG_TAG, "%{public}s", string);
}

void OS::Print(LogLevel level, const char* format, ...)
{
    constexpr size_t maxStringSize = 1024;
    char string[maxStringSize];
    va_list arguments;
    va_start(arguments, format);
    int len = vsnprintf_s(string, sizeof(string), sizeof(string) - 1, format, arguments);
    va_end(arguments);

    if (len < 0) {
        PrintString(LogLevel::LOG_ERROR, "vsnprintf_s failed");
        return;
    }
    PrintString(level, string);
}

#define JSVM_HITRACE_TAG HITRACE_TAG_OHOS

RunJsTrace::RunJsTrace(bool runJs) : runJs(runJs)
{
    if (runJs) {
        StartTrace(JSVM_HITRACE_TAG, "PureJS");
    } else {
        FinishTrace(JSVM_HITRACE_TAG);
    }
}

RunJsTrace::RunJsTrace(const char* name) : runJs(true)
{
    StartTrace(JSVM_HITRACE_TAG, name);
}

RunJsTrace::~RunJsTrace()
{
    if (runJs) {
        FinishTrace(JSVM_HITRACE_TAG);
    } else {
        StartTrace(JSVM_HITRACE_TAG, "PureJS");
    }
}

namespace ohos {
void ReportKeyThread(ThreadRole role)
{
    static_assert(static_cast<int64_t>(ThreadRole::IMPORTANT_DISPLAY) ==
                  static_cast<int64_t>(OHOS::ResourceSchedule::ResType::IMPORTANT_DISPLAY));
    static_assert(static_cast<int64_t>(ThreadRole::USER_INTERACT) ==
                  static_cast<int64_t>(OHOS::ResourceSchedule::ResType::USER_INTERACT));

    uint64_t uid = OS::GetUid();
    uint64_t tid = OS::GetTid();
    uint64_t pid = OS::GetPid();
    std::unordered_map<std::string, std::string> payLoad = { { "uid", std::to_string(uid) },
                                                             { "pid", std::to_string(pid) },
                                                             { "tid", std::to_string(tid) },
                                                             { "role", std::to_string(role) } };
    OHOS::ResourceSchedule::ResSchedClient::GetInstance().ReportData(
        OHOS::ResourceSchedule::ResType::RES_TYPE_REPORT_KEY_THREAD,
        OHOS::ResourceSchedule::ResType::ReportChangeStatus::CREATE, payLoad);
}

inline bool ReadSystemXpmState()
{
    constexpr size_t argBuffSize = 32;
    char buffer[argBuffSize] = { 0 };
    uint32_t buffSize = sizeof(buffer);

    if (SystemGetParameter("ohos.boot.advsecmode.state", buffer, &buffSize) == 0 && strcmp(buffer, "0") != 0) {
        return true;
    }
    return false;
}

void SetSecurityMode()
{
    constexpr size_t secArgCnt = 2;
    if (ReadSystemXpmState()) {
        int secArgc = secArgCnt;
        constexpr bool removeFlag = false;
        const char* secArgv[secArgCnt] = { "jsvm", "--jitless" };
        v8::V8::SetFlagsFromCommandLine(&secArgc, const_cast<char**>(reinterpret_cast<const char**>(secArgv)),
                                        removeFlag);
    }
}

constexpr int MAX_FILE_LENGTH = 32 * 1024 * 1024;

bool LoadStringFromFile(const std::string& filePath, std::string& content)
{
    std::ifstream file(filePath.c_str());
    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    const long fileLength = file.tellg();
    if (fileLength > MAX_FILE_LENGTH) {
        return false;
    }

    content.clear();
    file.seekg(0, std::ios::beg);
    std::copy(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(), std::back_inserter(content));
    return true;
}

bool ProcessBundleName(std::string& bundleName)
{
    int pid = getprocpid();
    std::string filePath = "/proc/" + std::to_string(pid) + "/cmdline";
    if (!LoadStringFromFile(filePath, bundleName)) {
        return false;
    }
    if (bundleName.empty()) {
        return false;
    }
    auto pos = bundleName.find(":");
    if (pos != std::string::npos) {
        bundleName = bundleName.substr(0, pos);
    }
    bundleName = bundleName.substr(0, strlen(bundleName.c_str()));
    return true;
}

void WriteHisysevent()
{
#ifdef ENABLE_HISYSEVENT
    std::string bundleName;
    if (!ProcessBundleName(bundleName)) {
        bundleName = "INVALID_BUNDLE_NAME";
    }
    HiSysEventWrite(OHOS::HiviewDFX::HiSysEvent::Domain::JSVM_RUNTIME, "APP_STATS",
                    OHOS::HiviewDFX::HiSysEvent::EventType::STATISTIC, "BUNDLE_NAME", bundleName);
#endif
}
} // namespace ohos

} // namespace platform
