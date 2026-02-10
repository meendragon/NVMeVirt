#!/bin/bash
# 파일명: fio_cb_test.sh

TARGET_DEV=/home/meen/nvmevirt/mnt/hot_cold_file

echo "1. Pre-conditioning (Filling 700MB)..."
echo "2. Hot/Cold Workload (Balanced for CB logic)..."

sudo fio - <<EOF
[global]
filename=$TARGET_DEV
direct=1
ioengine=libaio
bs=4k
norandommap=1
randrepeat=0
group_reporting
time_based=0

# ---------------------------------------------------------
# [Step 1] 빈 공간 꽉 채우기 (변경 없음)
# ---------------------------------------------------------
[prepare_fill]
rw=write
size=700M
numjobs=1
stonewall

# ---------------------------------------------------------
# [Step 2] Hot/Cold (수정됨)
# Hot에 제한을 걸어서 유효 페이지가 '즉시 0'이 되는 걸 방지함
# ---------------------------------------------------------
[hot_job]
rw=randwrite
time_based=1
runtime=300
offset=0
size=150M
rate_iops=2500  ; <--- [핵심 수정] 무제한에서 2500으로 제한!
numjobs=1

[cold_job]
rw=randwrite
time_based=1
runtime=300
offset=150M
size=550M
rate_iops=50    ; <--- [핵심 수정] Cold는 더 차갑게 (100 -> 50)
numjobs=1
EOF