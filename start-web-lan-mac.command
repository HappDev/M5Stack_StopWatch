#!/bin/zsh
cd -- "$(dirname -- "$0")" || exit 1
python3 tools/server.py --host 0.0.0.0
