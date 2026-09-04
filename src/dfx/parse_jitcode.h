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

#ifndef JSVM_PERF_PARSE_JITCODE_H_
#define JSVM_PERF_PARSE_JITCODE_H_

#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <sstream>
#include <unordered_map>
#include <vector>

#define SHM_SIZE (100 * 1024 * 1024)

namespace jsvm {
namespace jitparse {

constexpr char kStringTerminator[] = { '\0' };
constexpr char kJitCodeTerminator[] = "JIT#END"; // JitCodeTerminator
constexpr char kJitSymbolMapName[] = "JS_JIT_symbol";

struct PerfJitHeader {
    uint32_t magic_;
    uint32_t version_;
    uint32_t size_;
    uint32_t elf_mach_target_;
    uint32_t reserved_;
    uint32_t process_id_;
    uint64_t time_stamp_;
    uint64_t flags_;

    static const uint32_t kMagic = 0x4A695444;
    static const uint32_t kVersion = 1;
};

struct PerfJitCodeLoad {
    uint64_t code_address_;
    uint32_t process_id_;
    uint32_t size_;
    uint32_t code_size_;
    uint32_t code_id_;
};

struct JitSymbol {
    uint64_t vaddr;
    uint32_t codeSize;
    std::string name;

    JitSymbol(uint64_t vaddr, uint32_t codeSize, const std::string& name) : vaddr(vaddr), codeSize(codeSize), name(name)
    {}
};

class ELFParser {
public:
    bool GetInstruction(uint64_t pc, std::string& codeName) const;
    bool GetInstruction(uint64_t pc, std::string& codeName, uint32_t& offset) const;
    std::vector<JitSymbol> jitSymbols;
};

class JitSymbolVMA {
public:
    explicit JitSymbolVMA(uint32_t pid);
    uintptr_t GetStartAddress() const;
    bool Contains(uintptr_t address) const;
    bool ContainsRange(uintptr_t address, size_t size) const;
    bool HasPrepared() const;
    bool IsCurrentProcess() const;
    ~JitSymbolVMA();

private:
    uintptr_t startAddress = 0;
    uintptr_t endAddress = 0;
    uint64_t memorySize = 0;
    bool hasPrepared = false;
    const bool isCurrentProcess;
};

class JsSymbolExtractor {
public:
    explicit JsSymbolExtractor(uint32_t pid);

    ~JsSymbolExtractor();

    bool GetHeader(uintptr_t& memoryPointer) const;
    bool GetJitSymbols(uint32_t& codeID, uintptr_t& memoryPointer, std::vector<JitSymbol>& jitSymbols) const;

    std::unique_ptr<ELFParser> GetParser() const;

    bool GetInstruction(uintptr_t pc, std::string& codeName) const;
    bool GetInstruction(uintptr_t pc, std::string& codeName, uint32_t& offset) const;
    bool IsPidMatch(uint32_t storedPid, uint32_t dfxPid) const;

private:
    std::unique_ptr<ELFParser> parser = nullptr;
    std::unique_ptr<JitSymbolVMA> jitSymbolVMA = nullptr;
    const uint32_t targetPid;
};

} // namespace jitparse
} // namespace jsvm

#endif
