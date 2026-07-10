#!/usr/bin/env bash
# 泰山派 3M 只读体检脚本
#
# 用途：采集板端系统、NPU 与音频设备基线，供版本链核对与目标板迁移参考。
# 约束：全程只读，不修改任何文件；不要求 root；单个探测失败不影响后续采集。
#
# 用法：
#   bash scripts/check_taishanpi3m.sh | tee /tmp/taishanpi3m-check.txt
#
# 注意：不同 BSP 的 RKNPU 节点位置可能不同，脚本对每个节点都做了容错，
#       不存在时只记录"未发现"，不判定硬件损坏。

set -u

echo "===== 泰山派 3M 只读体检 $(date -u +%F\ %T\ UTC) ====="

section() { echo; echo "===== $1 ====="; }

section "系统与内核"
uname -a
cat /etc/os-release 2>/dev/null

section "CPU"
lscpu

section "内存"
free -h

section "磁盘"
df -h

section "RKNPU（debugfs，可能不存在或需要 root）"
if [ -d /sys/kernel/debug/rknpu ]; then
  cat /sys/kernel/debug/rknpu/version 2>/dev/null && echo "-- version 读取成功 --" || echo "-- version 读取失败（可能无 root） --"
  cat /sys/kernel/debug/rknpu/load 2>/dev/null && echo "-- load 读取成功 --" || echo "-- load 读取失败（可能无 root） --"
else
  echo "未发现 /sys/kernel/debug/rknpu（常见原因：未挂载 debugfs / 无权限 / 内核未启用节点）"
fi

section "RKNPU（设备节点，可能不存在）"
ls -la /dev/rknpu* 2>/dev/null || echo "未发现 /dev/rknpu* 设备节点"
ls -la /dev/dri/ 2>/dev/null | head -5

section "NPU 内核消息（最近相关行）"
dmesg 2>/dev/null | grep -i -E "rknpu|rknn" | tail -5 || echo "dmesg 不可读或无相关行"

section "ALSA 设备（音频）"
command -v aplay >/dev/null && aplay -l || echo "aplay 不存在"
command -v arecord >/dev/null && arecord -l || echo "arecord 不存在"
ls -la /dev/snd/ 2>/dev/null || echo "未发现 /dev/snd/（无音频设备）"

section "系统负载与运行时长"
uptime
cat /proc/loadavg

echo
echo "===== 体检结束：只记录事实，未做任何修改 ====="
