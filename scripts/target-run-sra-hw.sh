#!/bin/bash

INSTALL_DIR="/home/admin/srf"

$INSTALL_DIR/bin/sra \
    --config $INSTALL_DIR/etc/sra/config.json \
    --logstream stdout --loglevel DEBUG \
    add-del-list /tmp/ud_server.sock
