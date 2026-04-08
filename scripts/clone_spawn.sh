#!/bin/bash
IPS=("")

if [[ "$1" == "spawn" ]]; then
    echo "spawn  cloning mode"
    IPS=("192.168.1.10" "192.168.1.11" "10.0.0.5")
else
    IPS=("10.27.192.25")
    echo "simple cloning mode"
fi

REMOTE_USER="admin"
REMOTE_PASS="admin"

SRC_DIRS=("local" "scripts")
DEST_PATH="/home/admin/srf"

for ip in "${IPS[@]}"; do
    echo -ne ">>> Coping to $ip...\n"
    
    for dir in "${SRC_DIRS[@]}"; do
        echo -ne ">>> Coping $dir..."

        sshpass -p "$REMOTE_PASS" rsync -avz \
        -e "ssh -o StrictHostKeyChecking=no" --out-format='%n' \
        "$dir/" "$REMOTE_USER@$ip:$DEST_PATH/"

        echo -ne "\n>>> Coping $dir..."

        if [ $? -eq 0 ]; then
            echo " Done: $dir"
        else
            echo " Fail: $dir"
        fi
    done
    
    echo -ne ">>> Coping to $ip..."

    if [ $? -eq 0 ]; then
        echo "Done: $ip"
    else
        echo "Fail: $ip"
    fi
done
