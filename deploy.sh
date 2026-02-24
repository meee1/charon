#!/bin/bash
set -e

DEVICES="192.168.2.1 192.168.4.1"
REMOTE_PATH="/tmp/charon"
USER="root"
PASS="analog"

echo "Building charon..."
make

echo "Build successful."

ssh-keygen -f '/home/michael/.ssh/known_hosts' -R '192.168.2.1'
ssh-keygen -f '/home/michael/.ssh/known_hosts' -R '192.168.4.1'

for DEV in $DEVICES; do
    echo "Killing charon on $DEV..."
    sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no ${USER}@${DEV} "killall charon" || true
    echo "Uploading to $DEV..."
    sshpass -p "$PASS" scp -O -o StrictHostKeyChecking=no charon ${USER}@${DEV}:${REMOTE_PATH}
    sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no ${USER}@${DEV} "chmod +x ${REMOTE_PATH}"
    echo "$DEV done."
done

echo "All devices updated."
