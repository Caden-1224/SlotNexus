#!/bin/bash
# Author: Caden
# 测量与部署脚本契约：性能测量不得注入人工阶段等待，输出目标可在 WAV 与
# ALSA 间选择，基线脚本可记录代码版本，六个服务的启动参数只有一处。
# 这些不变量一旦回归（例如把 --stage-delay-ms 写回固定值，或又复制一份
# 六进程启动块），阶段耗时就不再反映真实链路，端口也会改不全。
set -euo pipefail

CASE_NAME=$1
DEPLOY_DIR=$2
TEST_ROOT=$(mktemp -d /tmp/slotnexus-measure-script-test.XXXXXX)
trap 'rm -rf "$TEST_ROOT"' EXIT

SCRIPTS=(build.sh check_deployment.sh common.sh run.sh start.sh stop.sh)

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
    # 非零延时（那会把人工等待算进模型与链路耗时）。run.sh 的 baseline
    # 场景是基线专用，直接固定 0，不给覆盖入口。
    for script in "${SCRIPTS[@]}"; do
      if grep -Eq -- '--stage-delay-ms[= ]+[1-9]' "$TEST_ROOT/$script"; then
        echo "$script 注入了写死的非零阶段等待" >&2
        exit 1
      fi
    done
    grep -Fq 'STAGE_DELAY_MS=${SLOTNEXUS_STAGE_DELAY_MS:-0}' "$TEST_ROOT/common.sh"
    ;;
  baseline_records_code_version)
    # 基线必须能回答“这组数字出自哪份代码”，否则前后版本无法比较。
    grep -Fq 'record_code_version' "$TEST_ROOT/run.sh"
    grep -Fq 'code_version.txt' "$TEST_ROOT/run.sh"
    grep -Fq 'source_sha256=' "$TEST_ROOT/common.sh"
    ;;
  start_selects_output_sink)
    # 实时播放与 WAV 复核是同一份回答的两种输出，不能写死其中一种。
    grep -Fq 'SINK=${SLOTNEXUS_SINK:-wav}' "$TEST_ROOT/common.sh"
    grep -Fq -- '--sink "$SINK"' "$TEST_ROOT/common.sh"
    grep -Fq -- '--sink-device "$SINK_DEVICE"' "$TEST_ROOT/common.sh"
    grep -Fq 'SINK_DEVICE=${SLOTNEXUS_SINK_DEVICE:-default}' "$TEST_ROOT/common.sh"
    ;;
  baseline_uses_zero_stage_delay)
    grep -Fq 'STAGE_DELAY_MS=0' "$TEST_ROOT/run.sh"
    ;;
  run_lists_all_scenarios)
    # 单一入口要真的覆盖原先那些独立脚本，否则合并只是删掉了能力。
    for scenario in baseline wav mic stability inject llm mock; do
      grep -Eq "^  ${scenario}\\) shift;" "$TEST_ROOT/run.sh" || {
        echo "run.sh 缺少场景: $scenario" >&2
        exit 1
      }
    done
    ;;
  single_service_launch_path)
    # 六进程启动参数只允许有一处：再出现一份就意味着改端口要改两遍。
    for endpoint in '--listen tcp://127.0.0.1:19201' \
                    '--listen tcp://127.0.0.1:19203' \
                    '--listen tcp://127.0.0.1:19310'; do
      count=$(grep -Fc -- "$endpoint" "$TEST_ROOT"/*.sh | awk -F: '{s+=$2} END {print s}')
      if [ "$count" -ne 1 ]; then
        echo "$endpoint 出现 $count 次（应为 1 次）" >&2
        exit 1
      fi
    done
    ;;
  *)
    echo "未知测试场景: $CASE_NAME" >&2
    exit 2
    ;;
esac
