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

#include <dlfcn.h>
#include <string>

#include "jsvm.h"
#include "jsvm_dfx_api.h"
#include "jsvm_utils.h"

using namespace testing;
using namespace testing::ext;

constexpr size_t K_EXCEPTION_BUF = 256;
constexpr uintptr_t K_TIGHT_STACK_RESERVE = 16 * 1024;
const char* K_SHALLOW_RECURSION = R"JS(
function rec(n) {
    if (n <= 0) {
        return 1;
    }
    return rec(n - 1) + 1;
}
rec(40);
)JS";
const char* K_DEEP_RECURSION = R"JS(
function rec(n) {
    if (n <= 0) {
        return 1;
    }
    return rec(n - 1) + 1;
}
rec(10000);
)JS";

class JSVMStackLimitTest : public testing::Test {
public:
    static void SetUpTestCase()
    {
        JSVM_InitOptions initOptions {};
        OH_JSVM_Init(&initOptions);
    }

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
    bool RunScript(const char* src, std::string* exceptionMessage)
    {
        JSVM_Value jsSrc = nullptr;
        if (OH_JSVM_CreateStringUtf8(env, src, JSVM_AUTO_LENGTH, &jsSrc) != JSVM_OK) {
            return false;
        }
        JSVM_Script script = nullptr;
        if (OH_JSVM_CompileScript(env, jsSrc, nullptr, 0, true, nullptr, &script) != JSVM_OK) {
            ClearPendingException(exceptionMessage);
            return false;
        }
        JSVM_Value result = nullptr;
        JSVM_Status runStatus = OH_JSVM_RunScript(env, script, &result);
        bool pending = false;
        OH_JSVM_IsExceptionPending(env, &pending);
        bool hasException = ClearPendingException(exceptionMessage);
        return runStatus == JSVM_OK && !pending && !hasException;
    }

    bool ClearPendingException(std::string* exceptionMessage)
    {
        JSVM_Value exception = nullptr;
        OH_JSVM_GetAndClearLastException(env, &exception);
        bool isUndefined = true;
        OH_JSVM_IsUndefined(env, exception, &isUndefined);
        if (isUndefined) {
            return false;
        }
        if (exceptionMessage != nullptr) {
            JSVM_Value messageVal = nullptr;
            OH_JSVM_GetNamedProperty(env, exception, "message", &messageVal);
            char buf[K_EXCEPTION_BUF] = { 0 };
            OH_JSVM_GetValueStringUtf8(env, messageVal, buf, sizeof(buf), nullptr);
            *exceptionMessage = buf;
        }
        return true;
    }

    JSVM_Env env = nullptr;
    JSVM_VM vm = nullptr;
    JSVM_EnvScope envScope = nullptr;
    JSVM_VMScope vmScope = nullptr;
    JSVM_HandleScope handleScope = nullptr;
};

HWTEST_F(JSVMStackLimitTest, RejectsInvalidArguments, TestSize.Level1)
{
    uintptr_t limit = 0;
    ASSERT_EQ(JsvmGetStackLimit(nullptr, &limit), -1);
    ASSERT_EQ(JsvmGetStackLimit(vm, nullptr), -1);
    ASSERT_EQ(JsvmSetStackLimit(nullptr, 0x1000), -1);
    ASSERT_EQ(JsvmSetStackLimit(vm, 0), -1);
}

HWTEST_F(JSVMStackLimitTest, GetSetRoundTripAndRestore, TestSize.Level1)
{
    uintptr_t saved = 0;
    ASSERT_EQ(JsvmGetStackLimit(vm, &saved), 0);
    ASSERT_NE(saved, 0U);

    uintptr_t updated = saved + 0x1000U;
    ASSERT_EQ(JsvmSetStackLimit(vm, updated), 0);

    uintptr_t readBack = 0;
    ASSERT_EQ(JsvmGetStackLimit(vm, &readBack), 0);
    ASSERT_EQ(readBack, updated);

    ASSERT_EQ(JsvmSetStackLimit(vm, saved), 0);
    uintptr_t restored = 0;
    ASSERT_EQ(JsvmGetStackLimit(vm, &restored), 0);
    ASSERT_EQ(restored, saved);
}

HWTEST_F(JSVMStackLimitTest, Case1SetThenRestoreAllowsShallowRecursion, TestSize.Level1)
{
    uintptr_t saved = 0;
    ASSERT_EQ(JsvmGetStackLimit(vm, &saved), 0);
    ASSERT_EQ(JsvmSetStackLimit(vm, saved), 0);

    std::string exception;
    ASSERT_TRUE(RunScript(K_SHALLOW_RECURSION, &exception)) << exception;

    ASSERT_EQ(JsvmSetStackLimit(vm, saved), 0);
}

HWTEST_F(JSVMStackLimitTest, Case2TightLimitCausesNaturalOverflow, TestSize.Level1)
{
    uintptr_t saved = 0;
    ASSERT_EQ(JsvmGetStackLimit(vm, &saved), 0);

    volatile char stackMarker = 0;
    uintptr_t tightLimit = reinterpret_cast<uintptr_t>(&stackMarker) - K_TIGHT_STACK_RESERVE;
    ASSERT_NE(tightLimit, 0U);
    ASSERT_EQ(JsvmSetStackLimit(vm, tightLimit), 0);

    std::string exception;
    bool ok = RunScript(K_DEEP_RECURSION, &exception);
    ASSERT_EQ(JsvmSetStackLimit(vm, saved), 0);
    ASSERT_FALSE(ok);
    ASSERT_NE(exception.find("Maximum call stack size exceeded"), std::string::npos) << exception;
}

HWTEST_F(JSVMStackLimitTest, SymbolsAreExported, TestSize.Level1)
{
    ASSERT_NE(dlsym(RTLD_DEFAULT, "JsvmGetStackLimit"), nullptr);
    ASSERT_NE(dlsym(RTLD_DEFAULT, "JsvmSetStackLimit"), nullptr);
}
