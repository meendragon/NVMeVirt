#!/bin/bash

# 경로 변경: /home/meen/nvmevirt/mnt
# 사이즈 변경: 4G -> 1G (중요!)
fio --directory=/home/meen/nvmevirt/mnt \
    --direct=1 \
    --ioengine=io_uring \
    --rw=randread \
    --bs=4k \
    --size=256M \
    --time_based=1 \
    --runtime=20 \
    --numjobs=1 \
    --name=read_test \
    --iodepth=4