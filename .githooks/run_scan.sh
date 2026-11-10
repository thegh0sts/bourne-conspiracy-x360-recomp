#!/bin/sh
# Shared by the hooks: run tools/leak_scan.py with whichever Python is installed.
# (On Windows "python3" is often only the Microsoft Store placeholder.)
scan="$(git rev-parse --show-toplevel)/tools/leak_scan.py"
for py in python3 python py; do
  if command -v "$py" >/dev/null 2>&1 && "$py" -c "import sys" >/dev/null 2>&1; then
    exec "$py" "$scan" "$@"
  fi
done
echo "leak_scan: no Python found; install Python or commit with care." >&2
exit 1
