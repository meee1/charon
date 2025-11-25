#!/bin/bash
set -e

# Configuration
TARGET_IP="192.168.4.1"
TARGET_USER="root"
TARGET_PASSWORD="analog"
TARGET_PATH="/tmp/charon"
BINARY_NAME="charon"

echo "==> Building charon..."
make clean
make

if [ ! -f "$BINARY_NAME" ]; then
    echo "Error: Binary '$BINARY_NAME' not found after build"
    exit 1
fi

echo "==> Killing any existing charon processes..."
sshpass -p "$TARGET_PASSWORD" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "${TARGET_USER}@${TARGET_IP}" "killall charon || true"

echo "==> Uploading to $TARGET_IP..."
cat "$BINARY_NAME" | sshpass -p "$TARGET_PASSWORD" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "${TARGET_USER}@${TARGET_IP}" "cat > ${TARGET_PATH} && chmod +x ${TARGET_PATH}"

echo "==> Running charon under callgrind on target..."
sshpass -p "$TARGET_PASSWORD" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "${TARGET_USER}@${TARGET_IP}" "cd /tmp && valgrind --tool=callgrind --callgrind-out-file=callgrind.out.charon ./charon"

echo "==> Done! Callgrind output saved to callgrind.out.charon on target"
echo "==> Retrieving callgrind output..."
sshpass -p "$TARGET_PASSWORD" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "${TARGET_USER}@${TARGET_IP}" "cat /tmp/callgrind.out.charon" > callgrind.out.charon
echo "==> Callgrind output saved to ./callgrind.out.charon"
echo "==> Opening in KCachegrind..."
kcachegrind callgrind.out.charon &
