#!/bin/bash
# Copyright (c) 2024 Huawei Device Co., Ltd.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
export TARGET_GEN_DIR=$2
v8_path=$3
target_cpu=$4

JSVM_PATH=$(dirname $(readlink -f "$0"))
if [ ! -d "${v8_path}/v8" ]; then
  wget -o ${JSVM_PATH}/wget_download.log -O ${JSVM_PATH}/arkcompiler_jsvm_20250514.tar.gz https://mirrors.huaweicloud.com/openharmony/compiler/jsvm/arkcompiler_jsvm_20250514.tar.gz
  cd ${JSVM_PATH}
  tar -zxf arkcompiler_jsvm_20250514.tar.gz
  cp -r ${JSVM_PATH}/js_engine_url/v8 ${v8_path}
  mkdir -p "${v8_path}/v8-include"
  cp -r ${JSVM_PATH}/js_engine_url/v8-include ${v8_path}/v8-include/
  mkdir -p "${v8_path}/v8/arm64/lib.unstripped_v8/lib.unstripped"
  cp -r ${v8_path}/v8/arm64/libv8_shared.so ${v8_path}/v8/arm64/lib.unstripped_v8/lib.unstripped
fi

cp -u ${v8_path}/v8/${target_cpu}/libv8_shared.so ${TARGET_GEN_DIR}/libv8_shared.so
cp -r ${v8_path}/v8-include/v8-include ${TARGET_GEN_DIR}/v8-include
