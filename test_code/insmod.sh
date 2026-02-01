#!/bin/bash

# 사용법: ./insmod.sh [GC_MODE]
# 예: ./insmod.sh 1  -> gc_mode=1로 로드
# 예: ./insmod.sh    -> gc_mode=0(기본값)으로 로드

# 첫 번째 인자를 GC_MODE로 사용 (없으면 기본값 0)
GC_MODE=${1:-0}

# 설정 변수 (필요하면 여기서 수정하세요)
MEM_START="4G"
MEM_SIZE="512M"
CPUS="1,2"

# [추가] 디버그 모드 설정 (1=On, 0=Off)
DEBUG_MODE=1
echo "----------------------------------------"

# 1. 기존에 모듈이 떠있는지 확인하고 제거
if lsmod | grep -q "nvmev"; then
    echo "🔄 기존 nvmev 모듈이 감지되어 제거합니다..."
    sudo rmmod nvmev
    # 제거가 덜 끝났을 수 있으니 잠시 대기 (안전장치)
    sleep 1
else
    echo "ℹ️  기존 모듈 없음. 바로 시작합니다."
fi

# 2. 모듈 삽입
MODULE_PATH="/home/meen/nvmevirt/nvmev.ko"
echo "🚀 nvmev.ko 로드 중... (GC_MODE=$GC_MODE)"
CMD="sudo insmod $MODULE_PATH memmap_start=$MEM_START memmap_size=$MEM_SIZE cpus=$CPUS gc_mode=$GC_MODE debug_mode=$DEBUG_MODE"
echo "   Command: $CMD"

$CMD

# 3. 결과 확인
if [ $? -eq 0 ]; then
    echo "✅ 성공! 모듈이 로드되었습니다."
    lsmod | grep nvmev
else
    echo "❌ 실패! 로그를 확인하세요 (dmesg)."
    exit 1
fi
echo "----------------------------------------"