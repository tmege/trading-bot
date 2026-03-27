#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
BINARY="$BUILD_DIR/trading_bot"
PID_FILE="$PROJECT_DIR/data/trading_bot.pid"
LOG_DIR="$PROJECT_DIR/logs"

# ── Colors ─────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

# ── Checks ─────────────────────────────────────────────────────────────────
echo -e "${GREEN}Trading Bot — Start (Educational)${NC}"
echo -e "${YELLOW}Paper trading mode (educational) — no real funds at risk.${NC}"

# Check binary exists
if [ ! -f "$BINARY" ]; then
    echo -e "${RED}Error: Binary not found at $BINARY${NC}"
    echo "Run: cmake -B build && cmake --build build"
    exit 1
fi

# Auto-load .env if it exists (for API keys like TB_ANTHROPIC_API_KEY)
# SECURITY: Parse KEY=VALUE only, never source (prevents shell injection)
ENV_FILE="$PROJECT_DIR/.env"
if [ -f "$ENV_FILE" ]; then
    echo "Loading environment from .env"
    while IFS='=' read -r key value; do
        # Skip comments and empty lines
        [[ "$key" =~ ^[[:space:]]*# ]] && continue
        [[ -z "$key" ]] && continue
        # Strip leading/trailing whitespace from key
        key=$(echo "$key" | xargs)
        # Only export valid variable names (alphanumeric + underscore)
        if [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
            export "$key=$value"
        fi
    done < "$ENV_FILE"
fi

# Check if already running
if [ -f "$PID_FILE" ]; then
    old_pid=$(cat "$PID_FILE")
    if kill -0 "$old_pid" 2>/dev/null; then
        echo -e "${YELLOW}Bot already running (PID $old_pid)${NC}"
        echo "Use scripts/stop.sh to stop it first"
        exit 1
    else
        rm -f "$PID_FILE"
    fi
fi

# Create directories
mkdir -p "$PROJECT_DIR/data"
mkdir -p "$LOG_DIR"

# ── Start ──────────────────────────────────────────────────────────────────
echo "Starting trading bot..."
cd "$PROJECT_DIR"

if [ "${1:-}" = "--foreground" ] || [ "${1:-}" = "-f" ]; then
    # Foreground mode (with dashboard)
    exec "$BINARY"
else
    # Background mode
    nohup "$BINARY" > "$LOG_DIR/stdout.log" 2>&1 &
    echo $! > "$PID_FILE"
    echo -e "${GREEN}Bot started in background (PID $(cat "$PID_FILE"))${NC}"
    echo "Logs: $LOG_DIR/"
    echo "Stop: scripts/stop.sh"
fi
