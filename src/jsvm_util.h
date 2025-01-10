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

#ifndef JSVM_UTIL_H
#define JSVM_UTIL_H

#include <array>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// jsvm header
#include "jsvm_dfx.h"
#include "jsvm_log.h"
#include "jsvm_version.h"
#include "platform/platform.h"

// v8 header
#include "v8-inspector.h"
#include "v8-platform.h"
#include "v8-profiler.h"
#include "v8.h"

// Use FORCE_INLINE on functions that have a debug-category-enabled check first
// and then ideally only a single function call following it, to maintain
// performance for the common case (no debugging used).
#ifdef __GNUC__
#define FORCE_INLINE __attribute__((always_inline))
#define COLD_NOINLINE __attribute__((cold, noinline))
#else
#define FORCE_INLINE
#define COLD_NOINLINE
#endif

namespace jsvm {
template<typename T, size_t N>
constexpr size_t ArraySize(const T (&)[N])
{
    return N;
}
} // namespace jsvm

namespace v8impl {
template<typename T>
using Persistent = v8::Global<T>;
} // namespace v8impl
#endif