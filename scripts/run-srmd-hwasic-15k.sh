#!/bin/bash

./server-local/sbin/srmd \
  --sot ./server-local/etc/srmd/route_sot_5000_new.json \
  --config ./server-local/etc/srmd/srmd.json \
  --foreground
