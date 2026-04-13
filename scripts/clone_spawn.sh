#!/bin/bash
IPS=("")

if [[ "$1" == "spawn" ]]; then
    echo "spawn cloning mode"
    #        Leaf1          Leaf2           Leaf3          Spine1         Spine2
    IPS=("10.27.192.87" "10.27.192.115" "10.27.192.77" "10.27.192.26" "10.27.192.84")
else
    #        Leaf1
    IPS=("10.27.192.87")
    echo "simple cloning mode"
fi

REMOTE_USER="admin"
REMOTE_PASS="admin"

SRC_DIRS=("local")
DEST_PATH="/home/admin/srf"

for ip in "${IPS[@]}"; do
    echo -ne ">>> Coping to $ip...\n"
    
    for dir in "${SRC_DIRS[@]}"; do
        echo -ne ">>> Coping $dir..."

        sshpass -p "$REMOTE_PASS" rsync -avz \
        -e "ssh -o StrictHostKeyChecking=no" --out-format='%n' \
        --exclude "etc/srmd" \
        --exclude "sbin" \
        --exclude "lib" \
        "$dir/" "$REMOTE_USER@$ip:$DEST_PATH/"

        echo -ne "\n>>> Coping $dir..."

        if [ $? -eq 0 ]; then
            echo " Done\r"
        else
            echo " Fail\r"
        fi
    done

    sshpass -p "$REMOTE_PASS" rsync -avz \
    -e "ssh -o StrictHostKeyChecking=no" --out-format='%n' \
    "scripts/target-run-sra.sh" "$REMOTE_USER@$ip:/$DEST_PATH/../run-sra.sh"
    
    echo -ne "\n>>> Coping to $ip..."

    if [ $? -eq 0 ]; then
        echo "Done\n"
    else
        echo "Fail\n"
    fi
done
