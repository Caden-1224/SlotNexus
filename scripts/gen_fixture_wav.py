#!/usr/bin/env python3
# 生成固定输入 WAV fixture：16kHz 单声道 16-bit，1 秒 500 Hz 正弦波。
#
# 用途： 的固定 WAV → Fake PCM 链路需要确定性的输入音频。
# Fake ASR 阶段只关心帧数不关心内容，因此默认生成纯音；学习者可把
# 自己的录音（16kHz 单声道 16-bit WAV）替换 data/fixtures/voice.wav，
# 内容不影响 Mock 阶段结果，接入真实 sherpa-onnx 后才会体现。
#
# 用法：python3 scripts/gen_fixture_wav.py [输出路径]（默认 data/fixtures/voice.wav）
import math
import struct
import sys
import wave

def main():
   out = sys.argv[1] if len(sys.argv) > 1 else "data/fixtures/voice.wav"
   rate = 16000
   seconds = 1.0
   amplitude = 4000  # 约 -11 dBFS，人耳可清晰听见
   freq = 500.0

   with wave.open(out, "wb") as w:
       w.setnchannels(1)
       w.setsampwidth(2)
       w.setframerate(rate)
       frames = []
       for i in range(int(rate * seconds)):
           sample = int(amplitude * math.sin(2.0 * math.pi * freq * i / rate))
           frames.append(struct.pack("<h", sample))
       w.writeframes(b"".join(frames))
   print("已生成 %s：%d Hz 单声道 16-bit，%.1f 秒" % (out, rate, seconds))

if __name__ == "__main__":
   main()
