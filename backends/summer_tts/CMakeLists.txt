# SummerTTS vits 后端（板端 aarch64；仅 SLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON 时构建）。
#
# 上游源码不随仓库分发（third_party/README.md 约定），构建时由
# SLOTNEXUS_SUMMERTTS_ROOT 指向板端源码目录（如 ~/upstream_tts/tts，
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
set(SLOTNEXUS_SUMMERTTS_ROOT ""
    CACHE PATH "SummerTTS 上游源码根目录（含 src/ include/ eigen-3.4.0/）")
if(NOT SLOTNEXUS_SUMMERTTS_ROOT)
  message(FATAL_ERROR
    "SLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON 需要 "
    "-DSLOTNEXUS_SUMMERTTS_ROOT=<SummerTTS 源码根目录>")
endif()

find_package(OpenMP REQUIRED)

add_library(slotnexus_summer_tts STATIC
  src/summer_tts_backend.cpp
  # 文本正则化前端（glog / gflags / openfst）
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/glog/src/demangle.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/glog/src/logging.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/glog/src/raw_logging.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/glog/src/symbolize.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/glog/src/utilities.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/glog/src/vlog_is_on.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/glog/src/signalhandler.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/gflags/src/gflags.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/gflags/src/gflags_reporting.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/gflags/src/gflags_completions.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/compat.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/flags.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/fst.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/fst-types.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/mapped-file.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/properties.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/symbol-table.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/symbol-table-ops.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/util.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/openfst/src/lib/weight.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/processor.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/token_parser.cc
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/utf8_string.cc
  # 文本前端（中英文本到音素 id）
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/engipa/EnglishText2Id.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/engipa/InitIPASymbols.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/engipa/ipa.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/engipa/alphabet.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/hz2py/hanzi2phoneid.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/hz2py/Hanz2Piny.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/hz2py/pinyinmap.cpp
  # vits 算子
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_conv1d.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_softmax.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_layer_norm.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_relu.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_gelu.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_tanh.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_flip.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_cumsum.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_softplus.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_clamp_min.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_sigmoid.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_conv1d_transposed.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/nn_op/nn_leaky_relu.cpp
  # 平台层 + 工具
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/platform/tts_file_io.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/platform/tts_logger.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/utils/utils.cpp
  # vits 模块
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/iStft.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/hann.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/attention_encoder.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/multi_head_attention.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/ffn.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/ConvFlow.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/DDSConv.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/ElementwiseAffine.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/random_gen.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/ResidualCouplingLayer.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/ResBlock1.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/WN.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/modules/pqmf.cpp
  # vits 模型
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/TextEncoder.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/StochasticDurationPredictor.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/FixDurationPredictor.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/DurationPredictor_base.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/ResidualCouplingBlock.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/Generator_base.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/Generator_hifigan.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/Generator_MS.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/Generator_Istft.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/Generator_MBB.cpp
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/models/SynthesizerTrn.cpp
)
add_library(slotnexus::summer_tts ALIAS slotnexus_summer_tts)

target_compile_options(slotnexus_summer_tts PRIVATE -O3 -include cstdint)
# 契约头（backend_event.hpp / i_tts_backend.hpp）是本库的公共 API，必须
# 向消费者传播（summer_tts_test / tts_node 都要 include）；OpenMP 仅为
# 内部编译依赖，保持 PRIVATE。
target_link_libraries(slotnexus_summer_tts PUBLIC slotnexus::backend)
target_link_libraries(slotnexus_summer_tts PRIVATE OpenMP::OpenMP_CXX)
target_include_directories(slotnexus_summer_tts PRIVATE
  ${SLOTNEXUS_SUMMERTTS_ROOT}/eigen-3.4.0
  ${SLOTNEXUS_SUMMERTTS_ROOT}/include
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/header
  ${SLOTNEXUS_SUMMERTTS_ROOT}/src/tn/header
)
target_include_directories(slotnexus_summer_tts PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(slotnexus_summer_tts PUBLIC cxx_std_17)
