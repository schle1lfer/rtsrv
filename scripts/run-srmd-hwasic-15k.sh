#!/bin/bash

./server-local/sbin/srmd \
  --sot ./server-local/etc/srmd/route_sot_v3_hwasic-15k.json \
  --config ./server-local/etc/srmd/srmd.json \
  --foreground
