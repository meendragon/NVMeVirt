#!/bin/bash

# 경로가 마운트 포인트(/mnt)라면 파일 시스템 오버헤드 때문에 size를 좀 더 줄여야 합니다.
fio --directory=/home/meen/nvmevirt/mnt \
    --direct=1 \
    --ioengine=libaio \
    --rw=randwrite \
    --bs=4k \
    --size=380M \
    --numjobs=1 \
    --time_based \
    --runtime=60 \
    --group_reporting \
    --name=gc_trigger_test