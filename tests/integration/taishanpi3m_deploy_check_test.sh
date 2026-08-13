#!/bin/bash
set -euo pipefail

CASE_NAME=$1
CHECK_SCRIPT=$2
TEST_ROOT=$(mktemp -d /tmp/voxorchestra-deploy-check-test.XXXXXX)
trap 'rm -rf "$TEST_ROOT"' EXIT

mkdir -p \
  "$TEST_ROOT/build-taishanpi3m-hw/apps" \
  "$TEST_ROOT/config/taishanpi3m" \
  "$TEST_ROOT/data/knowledge" \
  "$TEST_ROOT/models/asr" \
  "$TEST_ROOT/bin"

for app in edge_gateway unit_manager session_node asr_node llm_node tts_node; do
  mkdir -p "$TEST_ROOT/build-taishanpi3m-hw/apps/$app"
  touch "$TEST_ROOT/build-taishanpi3m-hw/apps/$app/$app"
  chmod +x "$TEST_ROOT/build-taishanpi3m-hw/apps/$app/$app"
done

touch \
  "$TEST_ROOT/data/knowledge/knowledge.jsonl" \
  "$TEST_ROOT/models/asr/tokens.txt" \
  "$TEST_ROOT/models/model.rkllm" \
  "$TEST_ROOT/models/tts.bin"

cat > "$TEST_ROOT/config/taishanpi3m/session.json" <<'EOF'
{
  "knowledge": "data/knowledge/knowledge.jsonl",
  "asr": {"backend": "sherpa_onnx", "model": "models/asr"},
  "llm": {"backend": "rkllm", "model": "models/model.rkllm"},
  "tts": {"backend": "summertts", "model": "models/tts.bin"}
}
EOF

cat > "$TEST_ROOT/bin/ldd" <<'EOF'
#!/bin/bash
if [ "${LDD_UNRESOLVED:-0}" = 1 ] && [[ "$1" == */llm_node ]]; then
  echo 'librkllmrt.so => not found'
else
  echo 'libc.so.6 => /lib/libc.so.6'
fi
EOF
chmod +x "$TEST_ROOT/bin/ldd"
export PATH="$TEST_ROOT/bin:/usr/bin:/bin"

run_check() {
  VOXORCHESTRA_DEPLOY_ROOT="$TEST_ROOT" /bin/bash "$CHECK_SCRIPT"
}

case "$CASE_NAME" in
  valid)
    OUTPUT=$(run_check)
    grep -Fq '部署预检通过' <<< "$OUTPUT"
    ;;
  missing_model)
    rm "$TEST_ROOT/models/model.rkllm"
    set +e
    OUTPUT=$(run_check 2>&1)
    STATUS=$?
    set -e
    [ "$STATUS" -ne 0 ]
    grep -Fq '缺少 LLM 模型' <<< "$OUTPUT"
    ;;
  unresolved_library)
    export LDD_UNRESOLVED=1
    set +e
    OUTPUT=$(run_check 2>&1)
    STATUS=$?
    set -e
    [ "$STATUS" -ne 0 ]
    grep -Fq 'llm_node 存在未解析动态库' <<< "$OUTPUT"
    ;;
  *)
    echo "未知测试场景: $CASE_NAME" >&2
    exit 2
    ;;
esac
