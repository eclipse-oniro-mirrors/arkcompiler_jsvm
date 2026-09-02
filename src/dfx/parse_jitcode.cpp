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
#include <fstream>
#include <string>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include "jsvm_log.h"
#include "parse_jitcode.h"

namespace jsvm {
namespace jitparse {
static bool ReadProcessMemory(uint32_t pid, const uint64_t addr, void* data, size_t size)
{
    uint64_t currentAddr = addr;
    if (__builtin_add_overflow(currentAddr, size, &currentAddr)) {
        return false;
    }
    struct iovec remoteIov = {
        .iov_base = reinterpret_cast<void*>(addr),
        .iov_len = size,
    };
    struct iovec dataIov = {
        .iov_base = static_cast<uint8_t*>(data),
        .iov_len = size,
    };
    ssize_t readCount = process_vm_readv(pid, &dataIov, 1, &remoteIov, 1, 0);
    return static_cast<size_t>(readCount) == size;
}

static std::pair<uintptr_t, uintptr_t> FindVMAInProcMaps(uint32_t pid, const std::string& vmaName)
{
    std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream mapsFile(mapsPath);
    std::pair<uintptr_t, uintptr_t> res(0, 0);
    if (!mapsFile.is_open()) {
        LOG(Error) << "can not open map file!";
        return res;
    }
    std::string line;
    while (std::getline(mapsFile, line)) {
        if (line.find(vmaName) != std::string::npos) {
            // parse start address
            size_t dashPos = line.find('-');
            if (dashPos == std::string::npos) {
                continue;
            }
            size_t endPos = line.find(' ');
            std::string startAddrStr = line.substr(0, dashPos);
            std::string endAddrStr = line.substr(dashPos + 1, endPos);
            // convert address string to uint64_t
            constexpr int kNumberBase = 16;
            res.first = static_cast<uintptr_t>(std::stoull(startAddrStr, nullptr, kNumberBase));
            res.second = static_cast<uintptr_t>(std::stoull(endAddrStr, nullptr, kNumberBase));
            return res;
        }
    }
    LOG(Error) << "can not find JSVM JIT symbol!";
    return res;
}

JitSymbolVMA::JitSymbolVMA(uint32_t pid) : isCurrentProcess(static_cast<uint32_t>(getpid()) == pid)
{
    std::pair<uintptr_t, uintptr_t> addressRange = FindVMAInProcMaps(pid, kJitSymbolMapName);
    uintptr_t startMapAddress = addressRange.first;
    uintptr_t endMapAddress = addressRange.second;
    if (startMapAddress == 0 || endMapAddress == 0) {
        return;
    }
    if (endMapAddress <= startMapAddress) {
        return;
    }
    memorySize = endMapAddress - startMapAddress;
    if (memorySize > SHM_SIZE) {
        LOG(Error) << "jit symbol memory is too large!";
        return;
    }
    void* address = nullptr;
    if (isCurrentProcess) {
        startAddress = startMapAddress;
        endAddress = endMapAddress;
    } else {
        address = mmap(nullptr, memorySize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (address == MAP_FAILED) {
            LOG(Error) << "failed mmap symbol memory!";
            return;
        }

        if (!ReadProcessMemory(pid, reinterpret_cast<uint64_t>(startMapAddress), address, memorySize)) {
            LOG(Error) << "failed read dfxAddress to local address.";
            munmap(address, memorySize);
            return;
        }
        startAddress = reinterpret_cast<uintptr_t>(address);
        endAddress = reinterpret_cast<uintptr_t>(address) + memorySize;
    }
    hasPrepared = true;
}

JitSymbolVMA::~JitSymbolVMA()
{
    if (!isCurrentProcess) {
        munmap(reinterpret_cast<void*>(startAddress), memorySize);
    }
}

uintptr_t JitSymbolVMA::GetStartAddress() const
{
    return startAddress;
}

bool JitSymbolVMA::Contains(uintptr_t address) const
{
    if (address >= startAddress && address < endAddress) {
        return true;
    }
    return false;
}

bool JitSymbolVMA::ContainsRange(uintptr_t address, size_t size) const
{
    if (address < startAddress || address > endAddress) {
        return false;
    }
    return size <= static_cast<size_t>(endAddress - address);
}

bool JitSymbolVMA::HasPrepared() const
{
    return hasPrepared;
}

bool JitSymbolVMA::IsCurrentProcess() const
{
    return isCurrentProcess;
}

bool JsSymbolExtractor::IsPidMatch(uint32_t storedPid, uint32_t dfxPid) const
{
    if (jitSymbolVMA->IsCurrentProcess()) {
        return true; // already in current process
    }
    if (storedPid == 1 || storedPid == dfxPid) {
        return true; // arkweb render pid is 1
    }
    return false;
}

bool JsSymbolExtractor::GetHeader(uintptr_t& memoryPointer) const
{
    if (!jitSymbolVMA->ContainsRange(memoryPointer, sizeof(PerfJitHeader))) {
        return false;
    }

    PerfJitHeader header;
    std::memcpy(&header, reinterpret_cast<const void*>(memoryPointer), sizeof(header));
    if (header.magic_ != PerfJitHeader::kMagic ||
        header.version_ != PerfJitHeader::kVersion ||
        header.size_ != sizeof(PerfJitHeader) ||
        !IsPidMatch(header.process_id_, targetPid) ||
        !jitSymbolVMA->ContainsRange(memoryPointer, header.size_)) {
        return false;
    }
    memoryPointer += header.size_;
    return true;
}

bool JsSymbolExtractor::GetJitSymbols(uint32_t& codeID,
                                      uintptr_t& memoryPointer,
                                      std::vector<JitSymbol>& jitSymbols) const
{
    // The producer writes each record as
    // [PerfJitCodeLoad][code name bytes]['\0'], followed by JIT#END at the
    // current end of the stream. A later record overwrites that end marker.
    while (true) {
        if (jitSymbolVMA->ContainsRange(memoryPointer, sizeof(kJitCodeTerminator)) &&
            std::memcmp(reinterpret_cast<const void*>(memoryPointer),
                        kJitCodeTerminator, sizeof(kJitCodeTerminator)) == 0) {
            LOG(Info) << "last jit code block";
            return true;
        }

        if (!jitSymbolVMA->ContainsRange(memoryPointer, sizeof(PerfJitCodeLoad))) {
            // Compat: trailing bytes cannot hold a full record; keep the parsed
            // prefix as the baseline did for "no end tag" dumps.
            return true;
        }

        // Read the fixed-size record header only after confirming that the
        // complete header is inside the mapped shared-memory range.
        PerfJitCodeLoad codeLoad;
        std::memcpy(&codeLoad, reinterpret_cast<const void*>(memoryPointer), sizeof(codeLoad));
        if (codeLoad.size_ < sizeof(PerfJitCodeLoad) + sizeof(kStringTerminator)) {
            return false;
        }
        if (!jitSymbolVMA->ContainsRange(memoryPointer, codeLoad.size_)) {
            // Compat: last record declares more than remains (uncommitted tail);
            // keep the parsed prefix instead of failing the whole dump.
            return true;
        }
        if (!IsPidMatch(codeLoad.process_id_, targetPid) || codeLoad.code_id_ != codeID) {
            LOG(Error) << "codeLoad check failed!";
            return false;
        }

        // code_id_ is generated monotonically by the producer. Advance the
        // expected ID only after the complete record header has been validated.
        codeID++;

        // The variable-size code name starts immediately after the header and
        // must end within the remaining bytes declared by codeLoad.size_.
        const char* codeName = reinterpret_cast<const char*>(memoryPointer + sizeof(PerfJitCodeLoad));
        size_t codeNameCapacity = codeLoad.size_ - sizeof(PerfJitCodeLoad);
        const void* terminator = std::memchr(codeName, '\0', codeNameCapacity);
        if (terminator == nullptr) {
            return false;
        }

        size_t codeNameLength = static_cast<const char*>(terminator) - codeName;
        // Copy the validated name into an owned std::string; jitSymbols must not
        // retain a pointer into the shared-memory mapping.
        jitSymbols.emplace_back(codeLoad.code_address_, codeLoad.code_size_,
                                std::string(codeName, codeNameLength));
        memoryPointer += codeLoad.size_;
    }
}

JsSymbolExtractor::JsSymbolExtractor(uint32_t pid) : targetPid(pid)
{
    jitSymbolVMA = new JitSymbolVMA(pid);
    if (!jitSymbolVMA->HasPrepared()) {
        return;
    }
    uintptr_t memoryPointer = jitSymbolVMA->GetStartAddress();
    uint32_t codeID = 0;
    std::vector<JitSymbol> jitSymbols;
    if (!GetHeader(memoryPointer)) {
        return;
    }
    parser = std::make_unique<ELFParser>();
    if (!GetJitSymbols(codeID, memoryPointer, parser->jitSymbols)) {
        LOG(Error) << "GetJitSymbols error!";
        parser.reset();
        return;
    }
}

ELFParser* JsSymbolExtractor::GetParser() const
{
    return parser.get();
}

bool JsSymbolExtractor::GetInstruction(uintptr_t pc, std::string& codeName) const
{
    return parser->GetInstruction(pc, codeName);
}

bool JsSymbolExtractor::GetInstruction(uintptr_t pc, std::string& codeName, uint32_t& offset) const
{
    return parser->GetInstruction(pc, codeName, offset);
}

bool ELFParser::GetInstruction(uint64_t pc, std::string& codeName) const
{
    for (const auto& jitSymbol : jitSymbols) {
        if (pc >= jitSymbol.vaddr && pc < jitSymbol.vaddr + jitSymbol.codeSize) {
            codeName = jitSymbol.name;
            return true;
        }
    }
    return false;
}

bool ELFParser::GetInstruction(uint64_t pc, std::string& codeName, uint32_t& offset) const
{
    for (const auto& jitSymbol : jitSymbols) {
        if (pc >= jitSymbol.vaddr && pc < jitSymbol.vaddr + jitSymbol.codeSize) {
            codeName = jitSymbol.name;
            offset = pc - jitSymbol.vaddr;
            return true;
        }
    }
    return false;
}

} // namespace jitparse
} // namespace jsvm
