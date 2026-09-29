#!/bin/bash
# Author: Caden
# 测量脚本契约：性能测量不得注入人工阶段等待，输出目标可在 WAV 与 ALSA
# 间选择，基线脚本可记录代码版本。这些不变量一旦回归（例如把
# --stage-delay-ms 写回固定值），阶段耗时就不再反映真实链路。
set -euo pipefail

CASE_NAME=$1
DEPLOY_DIR=$2
TEST_ROOT=$(mktemp -d /tmp/slotnexus-measure-script-test.XXXXXX)
trap 'rm -rf "$TEST_ROOT"' EXIT

SCRIPTS=(start.sh run_baseline.sh run_real_wav_chain.sh run_mic_chain.sh
         run_stability_30.sh)

for script in "${SCRIPTS[@]}"; do
  if [ ! -f "$DEPLOY_DIR/$script" ]; then
    echo "缺少部署脚本: $script" >&2
    exit 1
  fi
  bash -n "$DEPLOY_DIR/$script"
done

# 复制到临时目录：测试只读脚本内容，不改动仓库。
cp "${SCRIPTS[@]/#/$DEPLOY_DIR/}" "$TEST_ROOT/"

case "$CASE_NAME" in
  no_artificial_stage_delay)
    # 每个脚本的 stage delay 都由环境变量给出且默认 0；不得出现写死的
    # 非零延时（那会把人工等待算进模型与链路耗时）。run_baseline.sh 是
    # 基线专用，直接固定 0，不给覆盖入口。
    for script in "${SCRIPTS[@]}"; do
      if grep -Eq -- '--stage-delay-ms[= ]+[1-9]' "$TEST_ROOT/$script"; then
        echo "$script 注入了写死的非零阶段等待" >&2
        exit 1
      fi
    done
    for script in start.sh run_real_wav_chain.sh run_mic_chain.sh \
                  run_stability_30.sh; do
      grep -Fq 'STAGE_DELAY_MS=${SLOTNEXUS_STAGE_DELAY_MS:-0}' \
        "$TEST_ROOT/$script"
    done
    ;;
  baseline_records_code_version)
    # 基线必须能回答“这组数字出自哪份代码”，否则前后版本无法比较。
    grep -Fq 'record_code_version' "$TEST_ROOT/run_baseline.sh"
    grep -Fq 'code_version.txt' "$TEST_ROOT/run_baseline.sh"
    grep -Fq 'source_sha256=' "$TEST_ROOT/run_baseline.sh"
    ;;
  start_selects_output_sink)
    # 实时播放与 WAV 复核是同一份回答的两种输出，不能写死其中一种。
    grep -Fq 'OUTPUT_SINK=${SLOTNEXUS_SINK:-wav}' "$TEST_ROOT/start.sh"
    grep -Fq -- '--sink "$OUTPUT_SINK"' "$TEST_ROOT/start.sh"
    grep -Fq -- '--sink-device "$OUTPUT_DEVICE"' "$TEST_ROOT/start.sh"
    grep -Fq 'OUTPUT_DEVICE=${SLOTNEXUS_SINK_DEVICE:-default}' \
      "$TEST_ROOT/start.sh"
    ;;
  baseline_uses_zero_stage_delay)
    grep -Fq -- '--stage-delay-ms 0' "$TEST_ROOT/run_baseline.sh"
    ;;
  *)
    echo "未知测试场景: $CASE_NAME" >&2
    exit 2
    ;;
esac
