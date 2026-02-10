#!/bin/bash
TARGET_DEV=/home/meen/NVMeVirt/mnt/hot_cold_file

# 안전을 위해 기존 파일 삭제
sudo rm -f $TARGET_DEV

echo "=================================================="
echo "Phase 1: Pre-conditioning (Filling 11GB)"
echo "=================================================="

# [명령어 1] 오직 채우기만 수행
# - creat_on_open=1: 파일을 생성함
sudo fio --name=fill \
    --filename=$TARGET_DEV \
    --direct=1 \
    --ioengine=libaio \
    --rw=write \
    --bs=128k \
    --size=10G \
    --numjobs=1 \
    --fsync=1

if [ $? -ne 0 ]; then
    echo "Error: Fill phase failed. Check disk space."
    exit 1
fi

echo "=================================================="
echo "Phase 2: Hot/Cold Workload (CB Logic Test)"
echo "=================================================="

# [명령어 2] 실제 테스트 수행 (기존 파일 재사용)
# - allow_file_create=0: 새 파일 만들지 말고 있는 거 써라 (에러 방지 핵심)
sudo fio - <<EOF
[global]
filename=$TARGET_DEV
direct=1
ioengine=libaio
bs=4k
norandommap=1
randrepeat=0
group_reporting
time_based=1
runtime=300
allow_file_create=0  ; <--- 핵심: 이미 만든 파일 재사용

[hot_job]
rw=randwrite
offset=0
size=2G
rate_iops=15000     ; Hot 유지를 위한 고속 IO
numjobs=1

[cold_job]
rw=randwrite
offset=2G
size=8G
rate_iops=100       ; Cold 유지를 위한 저속 IO
numjobs=1
EOF