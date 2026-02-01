#!/bin/bash

MOUNT_POINT="/home/meen/nvmevirt/mnt"

# Phase 1: Cold 데이터 (Sequential Write)
echo "Phase 1: Writing cold data..."
fio --directory=${MOUNT_POINT} \
    --direct=1 \
    --ioengine=libaio \
    --rw=write \
    --bs=128k \
    --io_size=150M \
    --numjobs=1 \
    --group_reporting \
    --name=cold_data

echo ""
echo "Waiting 10 seconds for data to age..."
sleep 10

echo ""
echo "Phase 2: Overwrite (GC trigger)..."
fio --directory=${MOUNT_POINT} \
    --direct=1 \
    --ioengine=libaio \
    --rw=randwrite \
    --bs=4k \
    --filename=cold_data.0.0 \
    --numjobs=1 \
    --time_based \
    --runtime=60 \
    --group_reporting \
    --name=overwrite

sync
echo ""
echo "Done. Check: sudo dmesg | grep 'FLUSH - Final GC Stats' -A 5"