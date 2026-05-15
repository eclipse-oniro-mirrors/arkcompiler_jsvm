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

#include "jsvm_misuse_reporter.h"

#include <cinttypes>
#include <cstdio>

#include "jsvm_log.h"
#include "jsvm_util.h"
#include "platform/platform.h"
#include "platform/platform_ohos.h"

namespace v8impl {

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------

std::atomic<bool> ApiMisuseReporter::eventReportEnabled_ {false};
std::atomic<uint64_t> ApiMisuseReporter::ReportLimiter::seen_[ApiMisuseReporter::ReportLimiter::kMaxNumberKinds] {};
std::atomic<uint64_t> ApiMisuseReporter::EventDedup::reported_ {0};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static constexpr struct {
    const char* tag;     // e.g. "E01"
    const char* message; // developer-visible description
} kindInfo[] = {
    /* E00 */ { "E00", "No error"                                              },
    /* E01 */ { "E01", "API called without an active VM scope"                 },
    /* E02 */ { "E02", "The active VM Scope does not match the target VM"      },
    /* E03 */ { "E03", "VM scope is owned by another thread"                   },
    /* E04 */ { "E04", "OH_JSVM_CloseVMScope called from a different thread"   },
    /* E05 */ { "E05", "OH_JSVM_CloseVMScope called with a different VM"       },
    /* E06 */ { "E06", "OH_JSVM_CloseEnvScope called from a different thread"  },
};

static constexpr size_t kindInfoCount = sizeof(kindInfo) / sizeof(kindInfo[0]);

static_assert(kindInfoCount == static_cast<size_t>(ApiMisuseReporter::Kind::COUNT),
              "KIND_META must have one entry per ApiMisuseKind");

// ---------------------------------------------------------------------------
// KindTag / KindMessage
// ---------------------------------------------------------------------------

const char* ApiMisuseReporter::KindTag(Kind kind)
{
    auto idx = static_cast<size_t>(kind);
    if (UNLIKELY(idx >= kindInfoCount)) {
        return "E??";
    }
    return kindInfo[idx].tag;
}

const char* ApiMisuseReporter::KindMessage(Kind kind)
{
    auto idx = static_cast<size_t>(kind);
    if (UNLIKELY(idx >= kindInfoCount)) {
        return "Unknown misuse kind";
    }
    return kindInfo[idx].message;
}

// ---------------------------------------------------------------------------
// ReportLimiter
// ---------------------------------------------------------------------------

bool ApiMisuseReporter::ReportLimiter::ShouldLog(Kind kind, const char* apiName)
{
#if JSVM_ENABLE_API_ACCESS_AUDIT
    // Audit mode: no suppression, always log.
    return true;
#else
    auto kindIdx = static_cast<size_t>(kind);
    if (UNLIKELY(kindIdx == 0 || kindIdx >= kMaxNumberKinds)) {
        return false;
    }
    size_t bit = ApiBucket(apiName);
    uint64_t mask = UINT64_C(1) << bit;
    // If the bit was already set, this call is suppressed.
    uint64_t prev = seen_[kindIdx].fetch_or(mask, std::memory_order_relaxed);
    return (prev & mask) == 0;
#endif
}

// ---------------------------------------------------------------------------
// EventDedup
// ---------------------------------------------------------------------------

bool ApiMisuseReporter::EventDedup::ShouldReport(Kind kind)
{
    auto kindIdx = static_cast<size_t>(kind);
    if (UNLIKELY(kindIdx == 0 || kindIdx >= 64)) {
        return false;
    }
    uint64_t mask = UINT64_C(1) << kindIdx;
    uint64_t prev = reported_.fetch_or(mask, std::memory_order_relaxed);
    return (prev & mask) == 0;
}

// ---------------------------------------------------------------------------
// EmitLog
// ---------------------------------------------------------------------------

void ApiMisuseReporter::EmitLog(Kind kind, const char* apiName,
                                uint32_t ownerTid, uint32_t currentTid)
{
    const char* tag = KindTag(kind);
    const char* msg = KindMessage(kind);
    const char* name = apiName != nullptr ? apiName : "unknown";

    if (ownerTid != 0 || currentTid != 0) {
        LOG(Error) << "[JSVM API Misuse][" << tag << "] " << name << ": " << msg
                   << " (owner tid=" << ownerTid << ", current tid=" << currentTid << ")";
    } else {
        LOG(Error) << "[JSVM API Misuse][" << tag << "] " << name << ": " << msg;
    }
}

// ---------------------------------------------------------------------------
// EmitTrace
// ---------------------------------------------------------------------------

void ApiMisuseReporter::EmitTrace(Kind kind, const char* apiName,
                                  uint32_t ownerTid, uint32_t currentTid)
{
    const char* tag = KindTag(kind);
    const char* name = apiName != nullptr ? apiName : "unknown";

    // Use a point trace: RAII start+end in the same scope.
    if (ownerTid != 0 || currentTid != 0) {
        platform::RunJsTrace trace("JSVM API Misuse", "kind", tag, "api", name,
                                   "ownerTid", ownerTid, "currentTid", currentTid);
    } else {
        platform::RunJsTrace trace("JSVM API Misuse", "kind", tag, "api", name);
    }
}

// ---------------------------------------------------------------------------
// EmitAuditEvent
// ---------------------------------------------------------------------------

void ApiMisuseReporter::EmitAuditEvent(Kind kind, const char* apiName)
{
#ifdef TARGET_OHOS
    platform::ohos::WriteAPIUseToHisysevent(apiName, static_cast<uint32_t>(kind));
#else
    (void)kind;
    (void)apiName;
#endif
}

// ---------------------------------------------------------------------------
// Report — the public entry point
// ---------------------------------------------------------------------------

void ApiMisuseReporter::Report(Kind kind, const char* apiName,
                               uint32_t ownerTid, uint32_t currentTid)
{
    if (UNLIKELY(kind == Kind::E00_OK)) {
        return;
    }

    // Layer 1: always emit a trace when API trace is globally enabled.
#if JSVM_ENABLE_API_TRACE
    EmitTrace(kind, apiName, ownerTid, currentTid);
#endif

    // Layer 2: rate-limited log + optional event.
    if (!ReportLimiter::ShouldLog(kind, apiName)) {
        return;
    }

    EmitLog(kind, apiName, ownerTid, currentTid);

    // Layer 3: once-per-kind event reporting (only when enabled at runtime).
    if (eventReportEnabled_.load(std::memory_order_relaxed) &&
        EventDedup::ShouldReport(kind)) {
        EmitAuditEvent(kind, apiName);
    }
}

// ---------------------------------------------------------------------------
// EnableEventReport
// ---------------------------------------------------------------------------

void ApiMisuseReporter::EnableEventReport(bool enable)
{
    eventReportEnabled_.store(enable, std::memory_order_relaxed);
}

} // namespace v8impl
