#!/usr/bin/env python3
# 板端诊断：REQ 直连 asr_node（真实 sherpa 后端），pcm64 音频上行
# 逐个文件识别，打印节点 ack 全文——隔离 session 侧，定位固定识别文本来源。
import base64
import json
import sys
import zmq

ctx = zmq.Context()
s = ctx.socket(zmq.REQ)
s.connect("tcp://127.0.0.1:19201")
s.setsockopt(zmq.RCVTIMEO, 120000)
s.setsockopt(zmq.SNDTIMEO, 5000)


def call(msg):
    s.send_string(json.dumps(msg, ensure_ascii=False))
    return json.loads(s.recv_string())


r = call({"version": 1, "type": "setup", "work_id": "w-d", "request_id": "s-d",
          "payload": {}})
print("setup:", r.get("type"), json.dumps(r.get("payload"), ensure_ascii=False))

for name, path in [("demo_zh", "data/fixtures/demo_zh.wav"),
                   ("voice2", "data/fixtures/voice2.wav")]:
    with open(path, "rb") as f:
        wav = f.read()
    pcm = wav[44:]  # 跳过 RIFF/WAVE 头
    b64 = base64.b64encode(pcm).decode()
    print("== %s: %d 字节 PCM ==" % (name, len(pcm)))
    r = call({"version": 1, "type": "inference", "work_id": "w-d",
              "request_id": "r-" + name,
              "payload": {"text": "pcm64:" + b64}})
    print(json.dumps(r, ensure_ascii=False)[:800])

r = call({"version": 1, "type": "exit", "work_id": "w-d", "request_id": "e-d",
          "payload": {}})
print("exit:", r.get("type"))
