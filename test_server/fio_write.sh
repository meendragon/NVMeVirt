#!/bin/sh

sudo fio --directory=/home/meen/NVMeVirt/mnt\
    --direct=1\
    --ioengine=libaio\
    --rw=randwrite\
    --bs=4k\
    --size=700M\
    --io_size=30G\
    --numjobs=1\
    --norandommap=1\
    --randrepeat=0\
    --random_distribution=zipf:1.2\
    --name zipf_hot_cold_test