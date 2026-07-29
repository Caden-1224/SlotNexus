# SummerTTS vits 后端（板端 aarch64；仅 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 时构建）。
#
# 上游源码不随仓库分发（third_party/README.md 约定），构建时由
# VOXORCHESTRA_SUMMERTTS_ROOT 指向板端源码目录（如 ~/upstream_tts/tts，
# 含 src/ include/ eigen-3.4.0/）。模型文件（single_speaker_fast.bin）不入库，
# 路径经 config/taishanpi3m/session.json::tts.model 或 tts_node --model 传入。
#
# 编译单元与两处 GCC 13 适配复刻 artifacts/upstream-baseline/CMakeLists.smoke.txt
# （板端 Ubuntu 24.04, g++ 13.3）：
#   - 上游若干头（pinyinmap.h / hanzi2phoneid.h / Hanz2Piny.h）用 uint16_t
#     未自给自足 <cstdint>：目标级 -include cstdint 补齐，不改上游源码；
#   - engipa/ipa.cpp（定义全局 const wchar_t* Ipa[]）作者原 CMakeLists 漏入，
#     本库补入该编译单元，否则链接报 undefined reference to Ipa。
# -O3 与门禁基线一致（Eigen vits 推理在非优化构建下退化到不可用量级）。
set(VOXORCHESTRA_SUMMERTTS_ROOT ""
    CACHE PATH "SummerTTS 上游源码根目录（含 src/ include/ eigen-3.4.0/）")
if(NOT VOXORCHESTRA_SUMMERTTS_ROOT)
  message(FATAL_ERROR
    "VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 需要 "
    "-DVOXORCHESTRA_SUMMERTTS_ROOT=<SummerTTS 源码根目录>")
endif()

find_package(OpenMP REQUIRED)

add_library(voxorchestra_summer_tts STATIC
  src/summer_tts_backend.cpp
  # 文本正则化前端（glog / gflags / openfst）
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/glog/src/demangle.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/glog/src/logging.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/glog/src/raw_logging.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/glog/src/symbolize.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/glog/src/utilities.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/glog/src/vlog_is_on.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/glog/src/signalhandler.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/gflags/src/gflags.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/gflags/src/gflags_reporting.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/gflags/src/gflags_completions.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/compat.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/flags.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/fst.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/fst-types.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/mapped-file.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/properties.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/symbol-table.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/symbol-table-ops.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/util.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/weight.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/processor.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/token_parser.cc
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/utf8_string.cc
  # 文本前端（中英文本到音素 id）
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/engipa/EnglishText2Id.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/engipa/InitIPASymbols.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/engipa/ipa.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/engipa/alphabet.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/hz2py/hanzi2phoneid.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/hz2py/Hanz2Piny.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/hz2py/pinyinmap.cpp
  # vits 算子
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_conv1d.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_softmax.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_layer_norm.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_relu.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_gelu.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_tanh.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_flip.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_cumsum.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_softplus.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_clamp_min.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_sigmoid.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_conv1d_transposed.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/nn_op/nn_leaky_relu.cpp
  # 平台层 + 工具
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/platform/tts_file_io.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/platform/tts_logger.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/utils/utils.cpp
  # vits 模块
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/iStft.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/hann.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/attention_encoder.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/multi_head_attention.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/ffn.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/ConvFlow.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/DDSConv.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/ElementwiseAffine.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/random_gen.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/ResidualCouplingLayer.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/ResBlock1.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/WN.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/modules/pqmf.cpp
  # vits 模型
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/TextEncoder.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/StochasticDurationPredictor.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/FixDurationPredictor.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/DurationPredictor_base.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/ResidualCouplingBlock.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/Generator_base.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/Generator_hifigan.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/Generator_MS.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/Generator_Istft.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/Generator_MBB.cpp
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/models/SynthesizerTrn.cpp
)
add_library(voxorchestra::summer_tts ALIAS voxorchestra_summer_tts)

target_compile_options(voxorchestra_summer_tts PRIVATE -O3 -include cstdint)
# 契约头（backend_event.hpp / i_tts_backend.hpp）是本库的公共 API，必须
# 向消费者传播（summer_tts_test / tts_node 都要 include）；OpenMP 仅为
# 内部编译依赖，保持 PRIVATE。
target_link_libraries(voxorchestra_summer_tts PUBLIC voxorchestra::backend)
target_link_libraries(voxorchestra_summer_tts PRIVATE OpenMP::OpenMP_CXX)
target_include_directories(voxorchestra_summer_tts PRIVATE
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/eigen-3.4.0
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/include
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/header
  ${VOXORCHESTRA_SUMMERTTS_ROOT}/src/tn/header
)
target_include_directories(voxorchestra_summer_tts PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(voxorchestra_summer_tts PUBLIC cxx_std_17)
