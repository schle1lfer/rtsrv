#!/bin/bash

./target-local/sbin/srmd \
  --sot ./target-local/etc/srmd/route_sot_v3_hwasic.json \
  --config ./target-local/etc/srmd/srmd.json \
  --foreground

