#!/bin/bash
# 파일명: fio_full_test.sh

TARGET_DEV=/home/meen/nvmevirt/mnt/hot_cold_file

echo "1. Pre-conditioning (Filling 700MB)..."
echo "2. Hot/Cold Workload Running..."

sudo fio - <<EOF
[global]
filename=$TARGET_DEV
direct=1
ioengine=libaio
bs=4k
norandommap=1
randrepeat=0
group_reporting
time_based=0   ; 준비 단계는 시간 제한 없이 끝까지 채워야 함

# ---------------------------------------------------------
# [Step 1] 빈 공간 꽉 채우기 (Pre-conditioning)
# 순차 쓰기(write)로 0부터 700MB까지 예쁘게 채워넣음
# ---------------------------------------------------------
[prepare_fill]
rw=write       ; 순차 쓰기
size=700M      ; 전체 용량 채우기
numjobs=1
stonewall      ; 🚧 [중요] 이 작업이 끝날 때까지 밑에 놈들은 대기!

# ---------------------------------------------------------
# [Step 2] Hot/Cold 고문 시작 (Aging)
# 위 작업이 끝나면 자동으로 시작됨
# ---------------------------------------------------------
[hot_job]
rw=randwrite
time_based=1
runtime=300    ; 5분 동안 지속
offset=0
size=150M
# rate_iops제거 -> 풀악셀
numjobs=1

[cold_job]
rw=randwrite
time_based=1
runtime=300
offset=150M
size=550M
rate_iops=100  ; Cold는 살살
numjobs=1
EOF