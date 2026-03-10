#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PID_FILE="$PROJECT_DIR/data/trading_bot.pid"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo -e "${GREEN}Trading Bot — Stop${NC}"

if [ ! -f "$PID_FILE" ]; then
    echo -e "${YELLOW}No PID file found. Bot may not be running.${NC}"
    exit 0
fi

PID=$(cat "$PID_FILE")

if ! kill -0 "$PID" 2>/dev/null; then
    echo -e "${YELLOW}Process $PID not running. Cleaning up PID file.${NC}"
    rm -f "$PID_FILE"
    exit 0
fi

echo "Sending SIGTERM to PID $PID..."
kill "$PID"

# Wait for graceful shutdown (max 10 seconds)
for i in $(seq 1 10); do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo -e "${GREEN}Bot stopped gracefully.${NC}"
        rm -f "$PID_FILE"
        exit 0
    fi
    sleep 1
done

echo -e "${YELLOW}Bot did not stop after 10s. Sending SIGKILL...${NC}"
kill -9 "$PID" 2>/dev/null || true
rm -f "$PID_FILE"
echo -e "${GREEN}Bot killed.${NC}"
