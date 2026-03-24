/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
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

#ifndef JSVM_TASK_H
#define JSVM_TASK_H

#include <future>

namespace v8impl {

template <typename T>
class JSVMOptionalTask : public v8::Task {
public:
    explicit JSVMOptionalTask(std::function<T()> func)
        : v8::Task(), func_(std::move(func)), result(std::make_shared<T>(nullptr))
    {}
    std::shared_ptr<T> GetResult()
    {
        return result;
    }
    void Run() override
    {
        *result = func_();
    };

private:
    std::shared_ptr<T> result;
    std::function<T()> func_;
};

using JSVMConsumeCodeCacheTask = JSVMOptionalTask<v8::ScriptCompiler::ConsumeCodeCacheTask *>;
std::unique_ptr<JSVMConsumeCodeCacheTask> CreateConsumeCodeCacheTask(
    v8::Isolate *isolate, const uint8_t *cachedData, size_t cachedDataLength)
{
    return std::make_unique<JSVMConsumeCodeCacheTask>([isolate, cachedData, cachedDataLength]() {
        std::unique_ptr<v8::ScriptCompiler::CachedData> cached_data =
            std::make_unique<v8::ScriptCompiler::CachedData>(cachedData, cachedDataLength);
        v8::ScriptCompiler::ConsumeCodeCacheTask *task =
            v8::ScriptCompiler::StartConsumingCodeCacheOnBackground(isolate, std::move(cached_data));
        task->Run();
        return task;
    });
}

}  // namespace v8impl

struct JSVM_DeserializeResult__ final {
    std::shared_ptr<v8::ScriptCompiler::ConsumeCodeCacheTask *> result;
};

#endif