/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef DFX_FAULTLOGGERD_CLIENT_H
#define DFX_FAULTLOGGERD_CLIENT_H

#include <cstdint>

extern "C" {
constexpr int32_t JSVM_HEAP_SNAPSHOT = 115;
constexpr int8_t LOG_FILE_DES_CLIENT = 0;

typedef struct RequestDataHead {
    /** type of faultlogger client */
    int8_t clientType;
    /** target process id outside sandbox */
    int32_t clientPid;
} __attribute__((packed)) RequestDataHead;

/**
 * @brief  request information
*/
typedef struct FaultLoggerdRequest {
    /** request data head **/
    RequestDataHead head;
    /** process id */
    int32_t pid;
    /** type of resquest */
    int32_t type;
    /** thread id */
    int32_t tid;
    /** time of current request */
    uint64_t time;
} __attribute__((packed)) FaultLoggerdRequest;

/**
 * @brief request file descriptor
 * @param request struct of request information
 * @return if succeed return file descriptor, otherwise return -1
*/
int32_t RequestFileDescriptorEx(struct FaultLoggerdRequest* request);
}

#endif