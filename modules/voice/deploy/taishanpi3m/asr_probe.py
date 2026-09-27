#!/usr/bin/env python3
# 板端诊断：REQ 直连 asr_node（真实 sherpa 后端），pcm64 音频上行。
# 逐个存在的固定 WAV 识别，打印节点 ack 全文——隔离 session 侧，定位
# 固定识别文本来源。WAV 通过 wave 模块按 RIFF/fmt/data 块解析，不假定
# 固定 44 字节头；非 16 kHz/16-bit/单声道输入直接报错。
import base64
import json
import os
import sys
import wave

import zmq

ctx = zmq.Context()
s = ctx.socket(zmq.REQ)
s.connect("tcp://127.0.0.1:19201")
s.setsockopt(zmq.RCVTIMEO, 120000)
s.setsockopt(zmq.SNDTIMEO, 5000)


def call(msg):
    s.send_string(json.dumps(msg, ensure_ascii=False))
    return json.loads(s.recv_string())


def read_pcm(path):
    with wave.open(path, "rb") as w:
        channels = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        frames = w.getnframes()
        if channels != 1 or width != 2 or rate != 16000:
            raise ValueError(
                "需要 16000 Hz / 16-bit / mono WAV，实际 %d Hz / %d-bit / %dch"
                % (rate, width * 8, channels)
            )
        return w.readframes(frames)


r = call({"version": 1, "type": "setup", "work_id": "w-d", "request_id": "s-d",
          "payload": {}})
print("setup:", r.get("type"), json.dumps(r.get("payload"), ensure_ascii=False))

fixture_dir = os.environ.get("SLOTNEXUS_VOICE_FIXTURE_DIR", "data/fixtures")
for name in ("demo_zh", "voice2"):
    path = os.path.join(fixture_dir, name + ".wav")
    if not os.path.isfile(path):
        print("== %s: 不存在，跳过 (%s) ==" % (name, path))
        continue
    try:
        pcm = read_pcm(path)
    except (ValueError, wave.Error) as error:
        print("== %s: WAV 非法: %s ==" % (name, error))
        continue
    b64 = base64.b64encode(pcm).decode()
    print("== %s: %d 字节 PCM ==" % (name, len(pcm)))
    r = call({"version": 1, "type": "inference", "work_id": "w-d",
              "request_id": "r-" + name,
              "payload": {"text": "pcm64:" + b64}})
    print(json.dumps(r, ensure_ascii=False)[:1200])

r = call({"version": 1, "type": "exit", "work_id": "w-d", "request_id": "e-d",
          "payload": {}})
print("exit:", r.get("type"))
