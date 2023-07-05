#!/bin/bash
# ==========================================================================
# Copyright (C) 2023 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==========================================================================

accel-config disable-device dsa0
accel-config disable-device dsa2
accel-config disable-device dsa4
accel-config disable-device dsa6

accel-config load-config -c ./dto-4-dsa.conf

accel-config enable-device dsa0
accel-config enable-device dsa2
accel-config enable-device dsa4
accel-config enable-device dsa6

accel-config enable-wq dsa0/wq0.0
accel-config enable-wq dsa2/wq2.0
accel-config enable-wq dsa4/wq4.0
accel-config enable-wq dsa6/wq6.0

export DTO_USESTDC_CALLS=0
export DTO_COLLECT_STATS=1
export DTO_WAIT_METHOD=yield
export DTO_MIN_BYTES=8192
export DTO_CPU_SIZE_FRACTION=0.33
export DTO_AUTO_ADJUST_KNOBS=1

# Run dto-test without DTO library
#/usr/bin/time ./dto-test-wodto

# Run dto-test with DTO library using LD_PRELOAD method
#export LD_PRELOAD=./libdto.so.1.0
#/usr/bin/time ./dto-test-wodto

# Run dto-test with DTO library using "re-compile with DTO" method
# (i.e., without LD_PRELOAD)
/usr/bin/time ./dto-test

# Run dto-test with DTO and get DSA perfmon counters
#perf stat -e dsa0/event=0x1,event_category=0x0/,dsa2/event=0x1,event_category=0x0/,dsa4/event=0x1,event_category=0x0/,dsa6/event=0x1,event_category=0x0/,dsa0/event=0x1,event_category=0x1/,dsa2/event=0x1,event_category=0x1/,dsa4/event=0x1,event_category=0x1/,dsa6/event=0x1,event_category=0x1/,dsa0/event=0x2,event_category=0x1/,dsa2/event=0x2,event_category=0x1/,dsa4/event=0x2,event_category=0x1/,dsa6/event=0x2,event_category=0x1/ /usr/bin/time ./dto-test
