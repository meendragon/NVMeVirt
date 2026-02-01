#!/bin/bash

# ==========================================
# NVMeVirt Full Benchmark Automation Script
# Modes: 0 (Default), 1 (Background), 2 (User Defined)
# ==========================================

# 스크립트들이 있는 경로 (현재 위치의 test_code 폴더 가정)
SCRIPT_DIR="./test_code"
# 결과를 저장할 디렉토리 생성 (날짜시간 포함)
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RESULT_DIR="./results_$TIMESTAMP"

# [추가] 디버그 모드 설정 (1=On, 0=Off)
DEBUG_MODE=1

mkdir -p "$RESULT_DIR"

echo "=================================================="
echo "🚀 벤치마크 자동화 시작"
echo "📂 결과 저장 경로: $RESULT_DIR"
echo "🛠  Debug Mode: $DEBUG_MODE"
echo "=================================================="

# 테스트할 모드 리스트 (0, 1, 2)
for MODE in 0 1 2
do
    echo ""
    echo "##################################################"
    echo "▶️  Testing GC Mode: $MODE 시작"
    echo "##################################################"

    # 1. 모듈 로드 (insmod.sh 활용)
    # [수정] MODE 뒤에 DEBUG_MODE를 두 번째 인자로 전달
    echo "[Step 1] Module Load (GC_MODE=$MODE, DEBUG_MODE=$DEBUG_MODE)..."
    $SCRIPT_DIR/insmod.sh $MODE $DEBUG_MODE
    
    if [ $? -ne 0 ]; then
        echo "❌ 모듈 로드 실패. 스크립트를 중단합니다."
        exit 1
    fi
    sleep 2 # 안정화를 위한 대기

    # 2. 마운트
    echo "[Step 2] Mounting..."
    $SCRIPT_DIR/mount.sh
    sleep 1

    # 3. Read Test
    echo "[Step 3] Running FIO Read Test..."
    $SCRIPT_DIR/fio_read.sh | tee "$RESULT_DIR/mode_${MODE}_read.log"
    sleep 2
    
    # Read 테스트가 만든 파일 삭제 (공간 확보)
    echo "🧹 Cleaning up read_test file..."
    rm -f /home/meen/nvmevirt/mnt/read_test*
    sleep 1
    
    # 4. Write Test
    echo "[Step 4] Running FIO Write Test..."
    $SCRIPT_DIR/fio_write.sh | tee "$RESULT_DIR/mode_${MODE}_write.log"
    sleep 2

    # 5. 언마운트
    echo "[Step 5] Unmounting..."
    $SCRIPT_DIR/unmount.sh
    sleep 2

    # 6. 모듈 제거
    echo "[Step 6] Removing Module..."
    sudo rmmod nvmev
    sleep 1
    
    echo "✅ GC Mode $MODE 테스트 완료"
done

echo ""
echo "=================================================="
echo "🎉 모든 테스트가 완료되었습니다!"
echo "📂 결과 파일들은 $RESULT_DIR 에 저장되었습니다."
echo "=================================================="