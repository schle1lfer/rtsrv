#!/bin/bash

INSTALL_DIR="/home/admin/srf"

$INSTALL_DIR/bin/sra \
    --config $INSTALL_DIR/etc/sra/config.json \
    run /tmp/ud_server.sock \
    watch
