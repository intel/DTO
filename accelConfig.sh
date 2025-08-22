#!/bin/bash

echo "OPTIONAL Arg-1: DSA device id. Default: 0"
echo "OPTIONAL Arg-2: Enable/Disable DSA device. Default: yes"
echo "OPTIONAL Arg-3: SHARED WQ id. Default: 1"
echo "OPTIONAL Arg-4: ENGINE count. Default: 4"

if [ "$#" -ge 5 ]; then
    echo "ERROR: Incorrect argument count. Expected arg count <= 4"
    exit 1
fi

DEVID=${1:-0}
ENABLE=${2:-yes}
SWQID=${3:-1}
NENGS=${4:-4}

DEV=dsa${DEVID}
SWQ=${DEV}/wq${DEVID}.${SWQID}

echo "=> ${SWQ}:"
accel-config disable-wq ${SWQ}

echo "=> ${DEV}:"
accel-config disable-device ${DEV}

if [ "${ENABLE}" != "yes" ]; then
    echo "Exit after disabling ${DEV}."
    exit 1
fi

for ((i=0; i < ${NENGS}; i++))
do
    echo "=> ${DEV}/engine${DEVID}.${i}"
    echo "configured"
    accel-config config-engine ${DEV}/engine${DEVID}.${i} --group-id=0
done

accel-config config-wq ${SWQ} --group-id=0
accel-config config-wq ${SWQ} --priority=1
accel-config config-wq ${SWQ} --wq-size=128
accel-config config-wq ${SWQ} --max-batch-size=1024
accel-config config-wq ${SWQ} --max-transfer-size=2147483648
accel-config config-wq ${SWQ} --block-on-fault=0
accel-config config-wq ${SWQ} --type=user
accel-config config-wq ${SWQ} --name="dsa-test"
accel-config config-wq ${SWQ} --mode=shared
accel-config config-wq ${SWQ} --threshold=127
accel-config config-wq ${SWQ} --driver-name="user"

echo "=> ${DEV}:"
accel-config enable-device ${DEV}

echo "=> ${SWQ}:"
accel-config enable-wq ${SWQ}

#accel-config config-group ${DEV}/group0.0 --traffic-class-a=1
#accel-config config-group ${DEV}/group0.0 --traffic-class-b=1
