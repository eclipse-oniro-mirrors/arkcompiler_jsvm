set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(LLVM_BIN_PATH "/home/cyl/workspace/jsvm_5.1/vendor/default/binary/artifacts/js_engine_url/llvm/bin")

set(CMAKE_C_COMPILER "${LLVM_BIN_PATH}/clang")
set(CMAKE_CXX_COMPILER "${LLVM_BIN_PATH}/clang++")

set(CMAKE_LINKER "${LLVM_BIN_PATH}/ld.lld")

set(SYSROOT_PATH "/home/cyl/workspace/jsvm_5.1/out/rk3568/obj/third_party/musl")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --target=aarch64-linux-ohos --sysroot=${SYSROOT_PATH} -isystem ${LLVM_BIN_PATH}/../include/c++/v1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --target=aarch64-linux-ohos --sysroot=${SYSROOT_PATH} -isystem ${LLVM_BIN_PATH}/../include/c++/v1")

