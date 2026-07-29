# 板端音频基线：板载麦克风录放验证

> 日期：2026-07-29（基线补齐：此前仅枚举设备，未做实际录放）

## 设备

- `arecord -l`：card0 `rockchipes8388`（ES8323 codec，capture）+ card1 `rockchiphdmi`（capture 显示但为 HDMI 回采）。
- `aplay -l`：card0 es8388（playback，3.5mm）/ card1 HDMI / card2 `rockchipdp0`（SPDIF）。
- 板载麦克风通路：`Main Mic Switch` / `ADC Data Select`（amixer -c 0，es8388）。

## 录放流程（官方指南：板载麦克风）

1. 拔掉 3.5mm 耳机（避免走耳机 mic 通路）。
2. `arecord -D hw:0,0 -f cd -r 44100 -c 2 -V stereo -d 10 board_mic.wav`，对板载麦克风说话。
3. 插回 3.5mm 耳机，`aplay -D plughw:0,0 board_mic.wav` 回放。

## 实测结果

| 项 | 值 |
|---|---|
| 文件 | `board_mic.wav`：RIFF/WAVE，Microsoft PCM，16-bit，stereo，44100 Hz |
| 采样帧 | 882000（20 s，两次 10 s 会话写入） |
| 峰值 | 1091（约 -29.5 dBFS） |
| 均方根 | 63.8 |
| 听音确认 | **用户回放确认：录音内容清晰可辨（板载麦克风通路正常）** |

## 观察 / 待优化（不阻塞）

- VU 表显示 0%，峰值仅 -29.5 dBFS：板载麦克风电平偏低（环境噪声级附近），
  对话内容经回放确认可辨。现场 ASR 接入前可能需要调增益
  （`amixer` Main Mic 增益 / 录音前校准），或录音后归一化。
- 板载麦克风为输入首选（用户确认优先板载，不用耳机 mic）。

## 结论

板载麦克风 → es8388 → 16-bit 44100 stereo WAV 通路可用，回放经 3.5mm 耳机确认，
音频基线补齐（原枚举记录见 `board-health-check.txt`）。
