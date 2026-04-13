#!/bin/bash

INSTALL_DIR="/home/admin/srf"

$INSTALL_DIR/srf/bin/sra \
    --config $INSTALL_DIR/srf/etc/sra/config.json \
    watch