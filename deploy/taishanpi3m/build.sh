#!/usr/bin/env bash
# 泰山派 3M 板端原生构建（aarch64）+ 全量 CTest。
#
# 用法：bash deploy/taishanpi3m/build.sh
# 产物：build-taishanpi3m/（Release；nlohmann-json 缺失时自动回退
#       third_party 单头文件，见 CMakeLists）。
set -u
cd "$(dirname "$0")/../.."

echo "== 依赖检查 =="
for cmd in cmake g++; do
  if ! command -v "$cmd" >/dev/null; then
    echo "缺少 $cmd：请先安装（sudo apt install -y cmake g++ libzmq3-dev nlohmann-json3-dev）"
    exit 1
  fi
done
if ! pkg-config --exists libzmq 2>/dev/null && [ ! -e /usr/include/zmq.hpp ]; then
  echo "缺少 libzmq3-dev：请先安装（sudo apt install -y libzmq3-dev）"
  exit 1
fi
echo "cmake/g++/libzmq 就绪"

echo "== 配置（Release）=="
cmake -S . -B build-taishanpi3m -DCMAKE_BUILD_TYPE=Release

echo "== 构建 =="
cmake --build build-taishanpi3m -j"$(nproc)"

echo "== 全量测试 =="
ctest --test-dir build-taishanpi3m --output-on-failure

echo "== 无硬件依赖验收 =="
bash scripts/check_no_hw_deps.sh build-taishanpi3m
