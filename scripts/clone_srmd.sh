#!/bin/bash

IPS=("10.124.224.65")

REMOTE_USER="admin"
REMOTE_PASS="admin"

SRC_DIRS=("target-local")
DEST_PATH="/home/admin/"

device_index=0

for ip in "${IPS[@]}"; do
    echo -ne ">>> Coping to $ip...\n"

    # instalations
    for dir in "${SRC_DIRS[@]}"; do
        echo -ne ">>> Coping $dir...\n"

        sshpass -p "$REMOTE_PASS" rsync -avz \
        -e "ssh -o StrictHostKeyChecking=no" --out-format='%n' \
        --exclude "bin" \
        "$dir/" "$REMOTE_USER@$ip:$DEST_PATH/$SRC_DIRS"

        echo -ne "\n>>> Coping $dir..."

        if [ $? -eq 0 ]; then
            echo -ne " Done\r"
        else
            echo -ne " Fail\r"
        fi
    done

    # run-target-srmd.sh
    sshpass -p "$REMOTE_PASS" rsync -avz \
    -e "ssh -o StrictHostKeyChecking=no" --out-format='%n' \
    "scripts/run-target-srmd.sh" "$REMOTE_USER@$ip:/$DEST_PATH/run-target-srmd.sh"

    ((device_index++))

    echo -ne "\n>>> Coping to $ip..."

    if [ $? -eq 0 ]; then
        echo -ne "Done\n"
    else
        echo -ne "Fail\n"
    fi
done
