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

#ifndef JSVM_MISUSE_REPORTER_H
#define JSVM_MISUSE_REPORTER_H

#include <atomic>
#include <cstdint>

// Compile-time switches (can be overridden via -D on the command line).
// Mirror the defaults from jsvm_api_enter.h so this header is self-contained.
#ifndef JSVM_ENABLE_API_TRACE
#define JSVM_ENABLE_API_TRACE 0
#endif

#ifndef JSVM_ENABLE_API_ACCESS_AUDIT
#define JSVM_ENABLE_API_ACCESS_AUDIT 0
#endif

namespace v8impl {

// ============================================================================
// ApiMisuseReporter
//
// Centralised reporter for developer-visible API misuse events.
//
// Design contract:
//   - Kind values are part of the diagnostic surface — treat them like an API.
//     Never renumber existing values; new values must be appended before
//     KIND_COUNT and documented in the developer guide.
//   - Naming rule: E{id:02d}_{DESCRIPTION}.  id == the numeric value.
//   - E00_OK (value 0) is the "absence of error" sentinel; Report() no-ops on it.
//   - Log output:  [JSVM API Misuse][E01] <apiName>: <message>
//   - Rate limiting: (Kind, apiName) key — once per bucket per process.
//     Bypassed when JSVM_ENABLE_API_ACCESS_AUDIT is defined as non-zero.
//   - Trace output: emitted on every Report() call when JSVM_ENABLE_API_TRACE
//     is defined as non-zero; not affected by rate limiting.
//   - Event reporting: controlled by EnableEventReport(); per-Kind once per
//     process; only triggered when the rate limiter allows the log.
// ============================================================================
class ApiMisuseReporter final {
public:
    // -----------------------------------------------------------------------
    // Developer-visible API misuse kinds.
    // id == numeric value; never change existing mappings.
    // -----------------------------------------------------------------------
    enum class Kind : int {
        E00_OK = 0,                       // No misuse; sentinel

        E01_VM_SCOPE_ABSENT = 1,          // API called without an active VM scope
        E02_VM_SCOPE_MISMATCH = 2,        // Active VM scope targets a different VM
        E03_VM_SCOPE_CROSS_THREAD = 3,    // VM scope is owned by another thread
        E04_VM_SCOPE_CLOSE_CROSS_THREAD = 4,  // OH_JSVM_CloseVMScope from wrong thread
        E05_VM_SCOPE_CLOSE_VM_MISMATCH = 5,   // OH_JSVM_CloseVMScope with different VM
        E06_ENV_SCOPE_CLOSE_CROSS_THREAD = 6, // OH_JSVM_CloseEnvScope from wrong thread

        COUNT, // sentinel — must be last
    };

    // Report API misuse. Thread-safe. Rate-limited by (kind, apiName).
    // ownerTid / currentTid: supply for thread-related kinds (E03–E06);
    // leave as 0 for others.
    static void Report(Kind kind, const char* apiName,
                       uint32_t ownerTid = 0, uint32_t currentTid = 0);

    // Enable or disable runtime HiSysEvent reporting.
    // Thread-safe. Disabled by default.
    static void EnableEventReport(bool enable);

    ApiMisuseReporter() = delete;

private:
    // -----------------------------------------------------------------------
    // ReportLimiter
    // Key: (Kind index, apiName bucket)
    // Per-Kind 64-bit bitmask; bit i = hash(apiName pointer) % 64.
    // Different Kinds have independent bitmasks → no cross-Kind suppression.
    // Bypassed entirely when JSVM_ENABLE_API_ACCESS_AUDIT != 0.
    // -----------------------------------------------------------------------
    class ReportLimiter final {
    public:
        static bool ShouldLog(Kind kind, const char* apiName);

    private:
        static constexpr size_t kMaxNumberKinds = static_cast<size_t>(Kind::COUNT);
        static constexpr size_t kApiBuckets = 64;
        static std::atomic<uint64_t> seen_[kMaxNumberKinds];

        static size_t ApiBucket(const char* apiName)
        {
            return (reinterpret_cast<uintptr_t>(apiName) >> 4) % kApiBuckets;
        }
    };

    // -----------------------------------------------------------------------
    // EventDedup
    // Per-Kind once-per-process deduplication for event reporting.
    // Uses a single 64-bit bitmask (bit i = Kind(i) already reported).
    // -----------------------------------------------------------------------
    class EventDedup final {
    public:
        static bool ShouldReport(Kind kind);

    private:
        static std::atomic<uint64_t> reported_;
    };

    // Returns the short tag string, e.g. "E01"
    static const char* KindTag(Kind kind);

    // Returns the human-readable developer-facing message for the given kind
    static const char* KindMessage(Kind kind);

    static void EmitLog(Kind kind, const char* apiName,
                        uint32_t ownerTid, uint32_t currentTid);

    static void EmitTrace(Kind kind, const char* apiName,
                          uint32_t ownerTid, uint32_t currentTid);

    static void EmitAuditEvent(Kind kind, const char* apiName);

    static std::atomic<bool> eventReportEnabled_;
};

// Convenience alias — callers can write ApiMisuseKind::E01_VM_SCOPE_ABSENT
using ApiMisuseKind = ApiMisuseReporter::Kind;

} // namespace v8impl

#endif // JSVM_MISUSE_REPORTER_H
