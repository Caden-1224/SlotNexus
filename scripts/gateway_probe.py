#!/usr/bin/env python3
"""edge_gateway 手动探测客户端（开发调试工具，非产品代码）。

用法：
  python3 scripts/gateway_probe.py [端口] [JSON 负载] [超时秒]

默认端口 9100（edge_gateway）；默认负载为一条合法 setup 请求；
默认超时 20 s（硬件后端 setup 含模型加载，可达 ~10 s）。
示例：
  python3 scripts/gateway_probe.py                    # setup -> Manager 分配 work_id
  python3 scripts/gateway_probe.py 9100 'garbage'     # 非法 JSON -> error
"""
import socket
import sys


def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9100
    payload = (
        sys.argv[2]
        if len(sys.argv) > 2
        else '{"version":1,"type":"inference","request_id":"r-1"}'
    )
    timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 20
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall((payload + "\n").encode("utf-8"))
        reply = s.makefile().readline()
        print(reply.strip() if reply else "<对端关闭，无回复>")


if __name__ == "__main__":
    main()
