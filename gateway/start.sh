#!/bin/bash
# lanlink-gateway 启动脚本（用户目录版）
cd "$(dirname "$0")"
pkill -f "python3 server.py" 2>/dev/null
sleep 1
setsid nohup python3 server.py > gateway.log 2>&1 < /dev/null &
sleep 2
echo "--- gateway.log ---"
tail -5 gateway.log
echo "--- port ---"
ss -tlnp 2>/dev/null | grep 18888 || echo "PORT NOT LISTENING"
