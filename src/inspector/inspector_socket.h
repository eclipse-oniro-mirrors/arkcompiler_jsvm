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

#ifndef SRC_INSPECTOR_SOCKET_H_
#define SRC_INSPECTOR_SOCKET_H_

#include <string>
#include <vector>

#include "inspector_utils.h"
#include "uv.h"

namespace jsvm {
namespace inspector {

class ProtocolHandler;

// HTTP Wrapper around a uv_tcp_t
class InspectorSocket {
public:
    class Delegate {
    public:
        virtual void OnHttpGet(const std::string& host, const std::string& path) = 0;
        virtual void OnSocketUpgrade(const std::string& host,
                                     const std::string& path,
                                     const std::string& acceptKey) = 0;
        virtual void OnWsFrame(const std::vector<char>& frame) = 0;
        virtual ~Delegate() = default;
    };

    using DelegatePointer = std::unique_ptr<Delegate>;
    using Pointer = std::unique_ptr<InspectorSocket>;

    static Pointer Accept(uv_stream_t* server, DelegatePointer delegate);

    ~InspectorSocket();

    void AcceptUpgrade(const std::string& acceptKey);
    void CancelHandshake();
    void Write(const char* data, size_t len);
    void SwitchProtocol(ProtocolHandler* handler);
    std::string GetHost();

    InspectorSocket(const InspectorSocket&) = delete;
    InspectorSocket& operator=(const InspectorSocket&) = delete;

private:
    static void Shutdown(ProtocolHandler* handler);
    InspectorSocket() = default;

    DeleteFnPtr<ProtocolHandler, Shutdown> protocolHandler;
};

} // namespace inspector
} // namespace jsvm

#endif // SRC_INSPECTOR_SOCKET_H_
