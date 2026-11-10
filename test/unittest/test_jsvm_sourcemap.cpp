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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#define JSVM_EXPERIMENTAL
#include "jsvm.h"
#include "jsvm_types.h"
#include "jsvm_utils.h"

using namespace testing;
using namespace testing::ext;

static const char STACK_TRIGGER_SRC[] = R"JS(
function inner() {
    throw new Error('sourcemap test error');
}
function outer() {
    try {
        inner();
    } catch (e) {
        return e.stack;
    }
}
outer();
)JS";

static const char BASIC_MAP_PATH[] = "sourcemap_jsvm_basic.js.map";
static const char MISSING_MAP_PATH[] = "sourcemap_jsvm_missing.js.map";
static const char IDEM_MAP_PATH[] = "sourcemap_jsvm_idem.js.map";
static const char STORM_MAP_PATH[] = "sourcemap_jsvm_storm.js.map";
static const char RW_MAP_PATH[] = "sourcemap_jsvm_rw.js.map";

static bool WriteMapFile(const char* path, const char* originSource)
{
    FILE* file = fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    int written = fprintf(file, "{\"version\":3,\"sources\":[\"%s\"],\"names\":[],\"mappings\":\"AAAA\"}", originSource);
    fclose(file);
    return written > 0;
}

static JSVM_Status CompileWithOrigin(JSVM_Env env, const char* resourceName, const char* sourceMapUrl,
                                     JSVM_Script* script)
{
    JSVM_Value jsSrc = nullptr;
    JSVM_Status status = OH_JSVM_CreateStringUtf8(env, STACK_TRIGGER_SRC, strlen(STACK_TRIGGER_SRC), &jsSrc);
    if (status != JSVM_OK) {
        return status;
    }
    JSVM_ScriptOrigin origin {
        .sourceMapUrl = sourceMapUrl,
        .resourceName = resourceName,
        .resourceLineOffset = 0,
        .resourceColumnOffset = 0,
    };
    return OH_JSVM_CompileScriptWithOrigin(env, jsSrc, nullptr, 0, true, nullptr, &origin, script);
}

static JSVM_Status CompileWithOptions(JSVM_Env env, const char* resourceName, const char* sourceMapUrl,
                                      JSVM_Script* script)
{
    JSVM_Value jsSrc = nullptr;
    JSVM_Status status = OH_JSVM_CreateStringUtf8(env, STACK_TRIGGER_SRC, strlen(STACK_TRIGGER_SRC), &jsSrc);
    if (status != JSVM_OK) {
        return status;
    }
    JSVM_ScriptOrigin origin {
        .sourceMapUrl = sourceMapUrl,
        .resourceName = resourceName,
        .resourceLineOffset = 0,
        .resourceColumnOffset = 0,
    };
    JSVM_CompileOptions options[] = {
        {.id = JSVM_COMPILE_SCRIPT_ORIGIN, .content.ptr = &origin},
        {.id = JSVM_COMPILE_ENABLE_SOURCE_MAP, .content.boolean = true},
    };
    return OH_JSVM_CompileScriptWithOptions(env, jsSrc, sizeof(options) / sizeof(options[0]), options, script);
}

static bool RunAndGetStack(JSVM_Env env, JSVM_Script script, std::string* stack)
{
    JSVM_Value result = nullptr;
    if (OH_JSVM_RunScript(env, script, &result) != JSVM_OK) {
        JSVM_Value exception = nullptr;
        OH_JSVM_GetAndClearLastException(env, &exception);
        return false;
    }
    bool isString = false;
    if (OH_JSVM_IsString(env, result, &isString) != JSVM_OK || !isString) {
        return false;
    }
    size_t length = 0;
    if (OH_JSVM_GetValueStringUtf8(env, result, nullptr, 0, &length) != JSVM_OK || length == 0) {
        return false;
    }
    stack->assign(length, '\0');
    size_t copied = 0;
    if (OH_JSVM_GetValueStringUtf8(env, result, &(*stack)[0], length + 1, &copied) != JSVM_OK) {
        return false;
    }
    stack->resize(copied);
    return true;
}

struct StartGate {
    std::atomic<int> arrived { 0 };
    std::atomic<bool> go { false };
};

static bool WaitArrived(const StartGate& gate, int expected)
{
    for (int waitedMs = 0; waitedMs < 10000; waitedMs += 10) {
        if (gate.arrived.load() >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return gate.arrived.load() >= expected;
}

static void RunWithWorkerEnv(const std::function<void(JSVM_Env)>& body, std::atomic<int>* failures)
{
    JSVM_VM vm = nullptr;
    JSVM_VMScope vmScope = nullptr;
    JSVM_Env env = nullptr;
    JSVM_EnvScope envScope = nullptr;
    JSVM_HandleScope handleScope = nullptr;
    bool ok = OH_JSVM_CreateVM(nullptr, &vm) == JSVM_OK &&
              OH_JSVM_OpenVMScope(vm, &vmScope) == JSVM_OK &&
              OH_JSVM_CreateEnv(vm, 0, nullptr, &env) == JSVM_OK &&
              OH_JSVM_OpenEnvScope(env, &envScope) == JSVM_OK &&
              OH_JSVM_OpenHandleScope(env, &handleScope) == JSVM_OK;
    if (!ok) {
        failures->fetch_add(1);
    } else {
        body(env);
    }
    if (env != nullptr && handleScope != nullptr) {
        OH_JSVM_CloseHandleScope(env, handleScope);
    }
    if (env != nullptr && envScope != nullptr) {
        OH_JSVM_CloseEnvScope(env, envScope);
    }
    if (env != nullptr) {
        OH_JSVM_DestroyEnv(env);
    }
    if (vmScope != nullptr) {
        OH_JSVM_CloseVMScope(vm, vmScope);
    }
    if (vm != nullptr) {
        OH_JSVM_DestroyVM(vm);
    }
}

class JSVMSourceMapTest : public testing::Test {
public:
    static void SetUpTestCase()
    {
        JSVM_InitOptions initOptions {};
        OH_JSVM_Init(&initOptions);
    }

    static void TearDownTestCase() {}

    void SetUp() override
    {
        ASSERT_EQ(OH_JSVM_CreateVM(nullptr, &vm), JSVM_OK);
        ASSERT_EQ(OH_JSVM_OpenVMScope(vm, &vmScope), JSVM_OK);
        ASSERT_EQ(OH_JSVM_CreateEnv(vm, 0, nullptr, &env), JSVM_OK);
        ASSERT_EQ(OH_JSVM_OpenEnvScope(env, &envScope), JSVM_OK);
        ASSERT_EQ(OH_JSVM_OpenHandleScope(env, &handleScope), JSVM_OK);
    }

    void TearDown() override
    {
        ASSERT_EQ(OH_JSVM_CloseHandleScope(env, handleScope), JSVM_OK);
        ASSERT_EQ(OH_JSVM_CloseEnvScope(env, envScope), JSVM_OK);
        ASSERT_EQ(OH_JSVM_DestroyEnv(env), JSVM_OK);
        ASSERT_EQ(OH_JSVM_CloseVMScope(vm, vmScope), JSVM_OK);
        ASSERT_EQ(OH_JSVM_DestroyVM(vm), JSVM_OK);
    }

protected:
    JSVM_Env env = nullptr;
    JSVM_VM vm = nullptr;
    JSVM_EnvScope envScope = nullptr;
    JSVM_VMScope vmScope = nullptr;
    JSVM_HandleScope handleScope = nullptr;
};

// Register a mapping, then resolve an error stack through it: the stack must
// point at the mapped original source instead of the generated resource.
HWTEST_F(JSVMSourceMapTest, JSVMSourceMapResolution001, TestSize.Level1)
{
    ASSERT_TRUE(WriteMapFile(BASIC_MAP_PATH, "origin_basic.ts"));
    JSVM_Script script = nullptr;
    ASSERT_EQ(CompileWithOrigin(env, "sourcemap_jsvm_basic.js", BASIC_MAP_PATH, &script), JSVM_OK);
    std::string stack;
    ASSERT_TRUE(RunAndGetStack(env, script, &stack));
    EXPECT_NE(stack.find("origin_basic.ts:1:1"), std::string::npos);
    EXPECT_NE(stack.find("inner"), std::string::npos);
    unlink(BASIC_MAP_PATH);
}

// A mapping whose .map file does not exist must fall back to the plain stack
// instead of crashing or returning garbage.
HWTEST_F(JSVMSourceMapTest, JSVMSourceMapResolution002, TestSize.Level1)
{
    unlink(MISSING_MAP_PATH);
    JSVM_Script script = nullptr;
    ASSERT_EQ(CompileWithOrigin(env, "sourcemap_jsvm_missing.js", MISSING_MAP_PATH, &script), JSVM_OK);
    std::string stack;
    ASSERT_TRUE(RunAndGetStack(env, script, &stack));
    EXPECT_NE(stack.find("inner"), std::string::npos);
    EXPECT_EQ(stack.find("origin_missing.ts"), std::string::npos);
}

// Registering the same (resourceName, sourceMapUrl) pair repeatedly through
// both compile API paths must stay JSVM_OK (idempotent registration).
HWTEST_F(JSVMSourceMapTest, JSVMSourceMapRegister001, TestSize.Level1)
{
    ASSERT_TRUE(WriteMapFile(IDEM_MAP_PATH, "origin_idem.ts"));
    JSVM_Script scriptByOrigin = nullptr;
    JSVM_Script scriptByOriginAgain = nullptr;
    JSVM_Script scriptByOptions = nullptr;
    ASSERT_EQ(CompileWithOrigin(env, "sourcemap_jsvm_idem.js", IDEM_MAP_PATH, &scriptByOrigin), JSVM_OK);
    ASSERT_EQ(CompileWithOrigin(env, "sourcemap_jsvm_idem.js", IDEM_MAP_PATH, &scriptByOriginAgain), JSVM_OK);
    ASSERT_EQ(CompileWithOptions(env, "sourcemap_jsvm_idem.js", IDEM_MAP_PATH, &scriptByOptions), JSVM_OK);
    std::string stack;
    ASSERT_TRUE(RunAndGetStack(env, scriptByOptions, &stack));
    EXPECT_NE(stack.find("origin_idem.ts:1:1"), std::string::npos);
    unlink(IDEM_MAP_PATH);
}

// Concurrent registration from several worker threads (each with its own VM).
// Hundreds of distinct keys force rehashes of the shared mapping table; after
// the storm the registrations must still resolve correctly.
HWTEST_F(JSVMSourceMapTest, JSVMSourceMapConcurrentWriters001, TestSize.Level1)
{
    constexpr int THREAD_COUNT = 4;
    constexpr int WRITES_PER_THREAD = 100;
    ASSERT_TRUE(WriteMapFile(STORM_MAP_PATH, "origin_storm.ts"));
    std::atomic<int> failures = 0;
    StartGate gate;
    std::vector<std::thread> threads;
    for (int t = 0; t < THREAD_COUNT; t++) {
        threads.emplace_back([t, &gate, &failures]() {
            RunWithWorkerEnv([t, &gate, &failures](JSVM_Env env) {
                gate.arrived.fetch_add(1);
                while (!gate.go.load()) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < WRITES_PER_THREAD; i++) {
                    std::string name = "storm_w_" + std::to_string(t) + "_" + std::to_string(i) + ".js";
                    JSVM_Script script = nullptr;
                    if (CompileWithOrigin(env, name.c_str(), STORM_MAP_PATH, &script) != JSVM_OK) {
                        failures.fetch_add(1);
                    }
                }
            }, &failures);
        });
    }
    EXPECT_TRUE(WaitArrived(gate, THREAD_COUNT));
    gate.go.store(true);
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);

    JSVM_Script script = nullptr;
    ASSERT_EQ(CompileWithOrigin(env, "storm_w_0_0.js", STORM_MAP_PATH, &script), JSVM_OK);
    std::string stack;
    ASSERT_TRUE(RunAndGetStack(env, script, &stack));
    EXPECT_NE(stack.find("origin_storm.ts:1:1"), std::string::npos);
    unlink(STORM_MAP_PATH);
}

// Readers resolve error stacks (GetSourceMapFromFileName) while writers keep
// registering new mappings (SetFileToSourceMapMapping) on other threads: the
// exact concurrent read/write scenario the mapping mutex guards against.
HWTEST_F(JSVMSourceMapTest, JSVMSourceMapConcurrentReadWrite001, TestSize.Level1)
{
    constexpr int WRITER_COUNT = 4;
    constexpr int READER_COUNT = 2;
    constexpr int WRITES_PER_THREAD = 100;
    constexpr int MAX_READ_ATTEMPTS = 2000;
    ASSERT_TRUE(WriteMapFile(RW_MAP_PATH, "origin_rw.ts"));
    std::atomic<int> failures = 0;
    std::atomic<int> mappedReads = 0;
    std::atomic<int> writersCompleted = 0;
    StartGate gate;
    std::vector<std::thread> threads;
    for (int t = 0; t < WRITER_COUNT; t++) {
        threads.emplace_back([t, &gate, &failures, &writersCompleted]() {
            RunWithWorkerEnv([t, &gate, &failures, &writersCompleted](JSVM_Env env) {
                gate.arrived.fetch_add(1);
                while (!gate.go.load()) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < WRITES_PER_THREAD; i++) {
                    std::string name = "rw_w_" + std::to_string(t) + "_" + std::to_string(i) + ".js";
                    JSVM_Script script = nullptr;
                    if (CompileWithOrigin(env, name.c_str(), RW_MAP_PATH, &script) != JSVM_OK) {
                        failures.fetch_add(1);
                    }
                }
                writersCompleted.fetch_add(1);
            }, &failures);
        });
    }
    for (int t = 0; t < READER_COUNT; t++) {
        threads.emplace_back([t, &gate, &failures, &mappedReads, &writersCompleted]() {
            RunWithWorkerEnv([t, &gate, &failures, &mappedReads, &writersCompleted](JSVM_Env env) {
                std::string name = "rw_r_" + std::to_string(t) + ".js";
                JSVM_Script script = nullptr;
                if (CompileWithOrigin(env, name.c_str(), RW_MAP_PATH, &script) != JSVM_OK) {
                    failures.fetch_add(1);
                    return;
                }
                gate.arrived.fetch_add(1);
                while (!gate.go.load()) {
                    std::this_thread::yield();
                }
                int localMapped = 0;
                for (int attempt = 0; attempt < MAX_READ_ATTEMPTS; attempt++) {
                    if (writersCompleted.load() >= WRITER_COUNT) {
                        break;
                    }
                    std::string stack;
                    if (!RunAndGetStack(env, script, &stack)) {
                        failures.fetch_add(1);
                        break;
                    }
                    if (stack.find("origin_rw.ts:1:1") == std::string::npos) {
                        failures.fetch_add(1);
                        break;
                    }
                    localMapped++;
                }
                mappedReads.fetch_add(localMapped);
            }, &failures);
        });
    }
    EXPECT_TRUE(WaitArrived(gate, WRITER_COUNT + READER_COUNT));
    gate.go.store(true);
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(mappedReads.load(), 0);
    unlink(RW_MAP_PATH);
}
