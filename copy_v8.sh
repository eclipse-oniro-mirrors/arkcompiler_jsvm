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
js_engine_url_version=arkcompiler_jsvm_20250527.tar.gz
llvm_version=llvm-linux-19.1.7-x86_64.tar.gz
if [ ! -d "${v8_path}/v8" ]; then
  if [ ! -f "${JSVM_PATH}/${js_engine_url_version}" ]; then
    wget -o ${JSVM_PATH}/wget_download.log -O ${JSVM_PATH}/${js_engine_url_version} https://mirrors.huaweicloud.com/openharmony/compiler/jsvm/${js_engine_url_version}
    cd ${JSVM_PATH}
    tar -zxf ${js_engine_url_version}
  fi
  if [ ! -f "${JSVM_PATH}/${llvm_version}" ]; then
    wget -o ${JSVM_PATH}/wget_download.log -O ${JSVM_PATH}/${llvm_version} https://mirrors.huaweicloud.com/openharmony/compiler/jsvm/${llvm_version}
    cd ${JSVM_PATH}
    tar -zxf ${llvm_version}
  fi
  mkdir -p ${v8_path}
  rm -rf ${v8_path}/*
  cp -r ${JSVM_PATH}/js_engine_url/* ${v8_path}
  cp -r ${JSVM_PATH}/llvm ${v8_path}
fi

cp -u ${v8_path}/v8/${target_cpu}/libv8_shared.so ${TARGET_GEN_DIR}/libv8_shared.so
cp -r ${v8_path}/v8-include/v8-include ${TARGET_GEN_DIR}/v8-include
