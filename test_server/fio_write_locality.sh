#!/bin/bash
TARGET_DEV=/home/meen/NVMeVirt/mnt/hot_cold_file

# 안전을 위해 기존 파일 삭제
sudo rm -f $TARGET_DEV

echo "=================================================="
echo "Phase 1: Full Sequential Fill (Clean Start)"
echo "=================================================="
# 1. 일단 10GB를 순차적으로 꽉 채웁니다. (모든 블록 Valid 100%)
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
    echo "Error: Fill phase failed."
    exit 1
fi

echo "=================================================="
echo "Phase 2: Dirtying the Cold Region (Fragmentation)"
echo "=================================================="
# 2. [핵심] 나중에 Cold가 될 영역(2G~10G)을 미리 '더럽힙니다'.
#    - 무작위 쓰기를 통해 물리 블록 내에 Invalid Page(구멍)를 만듭니다.
#    - io_size=8G : 해당 영역 크기만큼 덮어써서 충분히 파편화시킵니다.
sudo fio --name=pre_dirty_cold \
    --filename=$TARGET_DEV \
    --direct=1 \
    --ioengine=libaio \
    --rw=randwrite \
    --bs=4k \
    --offset=2G \
    --size=8G \
    --io_size=8G \
    --numjobs=1 \
    --allow_file_create=0

echo "=================================================="
echo "Phase 3: Hot/Cold Workload (Actual Test)"
echo "=================================================="

# 3. 실제 테스트: Hot은 미친듯이 쓰고, Cold는 거의 안 씁니다.
#    - Cold 영역은 Phase 2에서 이미 '더러워진(Invalid mixed)' 상태입니다.
#    - 시간이 지날수록 Age가 증가합니다.
#    - CB 알고리즘은 Age가 높고 Invalid가 섞인 이 Cold 블록들을 선택하게 됩니다.
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
allow_file_create=0

[hot_job]
rw=randwrite
offset=0
size=2G
rate_iops=15000     ; Hot 영역은 계속 덮어써서 GC 유발
numjobs=1

[cold_job]
rw=randwrite
offset=2G
size=8G
rate_iops=50       ; [중요] Cold는 아주 가끔만 건드림 (Age 증가 유도)
numjobs=1
EOF