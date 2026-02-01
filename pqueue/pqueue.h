// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2014, Volkan Yazıcı <volkan.yazici@gmail.com>
 * All rights reserved.
 */

/**
 * @file  pqueue.h
 * @brief 우선순위 큐(Priority Queue) 함수 선언 및 타입 정의
 *
 * 이 헤더 파일은 일반적인(Generic) 우선순위 큐 인터페이스를 정의합니다.
 * 내부적으로 이진 힙(Binary Heap) 알고리즘을 사용하여 O(log n)의 삽입/삭제 성능을 보장합니다.
 *
 * @{
 */

#ifndef PQUEUE_H
#define PQUEUE_H

/** * @brief 우선순위 데이터 타입 (unsigned long long)
 * * FTL에서는 주로 '유효 페이지 수(VPC)'나 '비용-이득 점수(Cost-Benefit Score)'가 
 * 이 우선순위 값으로 사용됩니다.
 */
typedef unsigned long long pqueue_pri_t;

/** * @brief 우선순위 비교, 조회, 설정을 위한 콜백 함수 타입 정의
 * * 이 라이브러리는 큐에 저장되는 데이터 구조체(예: struct line)의 내부를 알 수 없으므로,
 * 사용자가 직접 데이터를 조작할 수 있는 함수 포인터를 제공해야 합니다.
 */

/** * 우선순위를 반환하는 콜백 함수
 * @param a: 큐에 저장된 데이터 포인터 (예: struct line *)
 * @return 해당 데이터의 현재 우선순위 값
 */
typedef pqueue_pri_t (*pqueue_get_pri_f)(void *a);

/** * 우선순위를 설정하는 콜백 함수
 * @param a: 큐에 저장된 데이터 포인터
 * @param pri: 설정할 새로운 우선순위 값
 */
typedef void (*pqueue_set_pri_f)(void *a, pqueue_pri_t pri);

/** * 두 우선순위를 비교하는 콜백 함수 (Min-Heap vs Max-Heap 결정)
 * @param next: 비교할 대상 노드의 우선순위
 * @param curr: 현재 기준 노드의 우선순위
 * @return 
 * - 1 (True): 'next'가 'curr'보다 우선순위가 높음 (교체 필요)
 * - 0 (False): 그렇지 않음
 * - 이 함수의 구현에 따라 오름차순(Min-Heap) 또는 내림차순(Max-Heap)이 결정됩니다.
 */
typedef int (*pqueue_cmp_pri_f)(pqueue_pri_t next, pqueue_pri_t curr);


/** * @brief 큐 내부에서의 위치(인덱스)를 추적하기 위한 콜백 함수 타입 정의
 * * 힙 연산(Bubble up/down) 중에 데이터의 배열 인덱스가 계속 바뀝니다.
 * 데이터 구조체 내부에 자신의 현재 큐 인덱스를 저장해두면,
 * 나중에 특정 데이터를 O(1)에 찾아서 제거하거나 업데이트할 때 매우 유용합니다.
 */

/** 데이터 구조체 내에 저장된 현재 큐 인덱스를 가져오는 함수 */
typedef size_t (*pqueue_get_pos_f)(void *a);

/** 데이터 구조체 내에 현재 큐 인덱스를 기록하는 함수 */
typedef void (*pqueue_set_pos_f)(void *a, size_t pos);


// /** 디버깅용: 엔트리 정보를 출력하는 콜백 함수 (주석 처리됨) */
// typedef void (*pqueue_print_entry_f)(FILE *out, void *a);


/** * @brief 우선순위 큐 메인 구조체 (핸들)
 * * 큐의 상태와 사용자 정의 콜백 함수들을 모두 포함하는 구조체입니다.
 */
typedef struct pqueue_t {
    size_t size;  /**< 현재 큐에 저장된 요소의 개수 */
    size_t avail; /**< 큐에 할당된 전체 슬롯 크기 (Capacity) */
    size_t step;  /**< 큐가 꽉 찼을 때 늘릴 크기 (Realloc 단위) */
    
    /* 사용자 정의 동작을 위한 함수 포인터들 */
    pqueue_cmp_pri_f cmppri; /**< 비교 함수 (우선순위 판단 로직) */
    pqueue_get_pri_f getpri; /**< 우선순위 조회 함수 */
    pqueue_set_pri_f setpri; /**< 우선순위 설정 함수 */
    pqueue_get_pos_f getpos; /**< 위치 조회 함수 */
    pqueue_set_pos_f setpos; /**< 위치 설정 함수 */
    
    void **d; /**< 실제 데이터가 저장되는 이진 힙 배열 (void 포인터 배열) */
} pqueue_t;


/**
 * @brief 우선순위 큐 초기화 함수
 *
 * 큐 사용을 위해 메모리를 할당하고 콜백 함수들을 등록합니다.
 *
 * @param n: 초기 할당할 큐의 크기 (예상되는 아이템 개수)
 * @param cmppri: 우선순위 비교 함수 포인터 (Min/Max 힙 결정)
 * @param getpri: 우선순위 조회 함수 포인터
 * @param setpri: 우선순위 설정 함수 포인터
 * @param getpos: 위치 조회 함수 포인터
 * @param setpos: 위치 설정 함수 포인터
 *
 * @return 성공 시 큐 핸들(pqueue_t *) 반환, 메모리 부족 시 NULL 반환
 */
pqueue_t *pqueue_init(size_t n, pqueue_cmp_pri_f cmppri, pqueue_get_pri_f getpri,
              pqueue_set_pri_f setpri, pqueue_get_pos_f getpos, pqueue_set_pos_f setpos);

/**
 * @brief 큐 메모리 해제 함수
 * * 큐 구조체와 내부 배열(d)을 해제합니다. 
 * 단, 큐 안에 들어있는 데이터(void *d들이 가리키는 객체) 자체는 해제하지 않으므로 주의해야 합니다.
 * * @param q: 해제할 큐의 핸들
 */
void pqueue_free(pqueue_t *q);

/**
 * @brief 큐의 현재 크기 반환
 * @param q: 큐 핸들
 * @return 현재 저장된 요소의 개수
 */
size_t pqueue_size(pqueue_t *q);

/**
 * @brief 큐에 새로운 아이템 삽입
 * * 아이템을 힙의 맨 끝에 추가한 뒤, 부모 노드와 비교하며 위로 올라가는 
 * 'Bubble Up' 과정을 통해 힙 속성을 유지합니다.
 * * @param q: 큐 핸들
 * @param d: 삽입할 아이템의 포인터 (예: struct line *)
 * @return 성공 시 0 반환
 */
int pqueue_insert(pqueue_t *q, void *d);

/**
 * @brief 큐에 있는 특정 아이템의 우선순위 변경
 * * 특정 아이템의 우선순위 값이 바뀌었을 때 호출합니다. 
 * 변경된 값에 따라 아이템을 위로 올리거나(Bubble Up) 아래로 내리는(Trickle Down) 
 * 재정렬 작업을 수행합니다. FTL에서 블록의 유효 페이지 수가 변할 때 주로 사용됩니다.
 * * @param q: 큐 핸들
 * @param new_pri: 적용할 새로운 우선순위 값
 * @param d: 대상 아이템의 포인터
 */
void pqueue_change_priority(pqueue_t *q, pqueue_pri_t new_pri, void *d);

/**
 * @brief 우선순위가 가장 높은 아이템 꺼내기 (Pop)
 * * 루트 노드(가장 높은 우선순위)를 제거하고 반환합니다.
 * 그 후 맨 마지막 노드를 루트로 옮기고 아래로 내리는(Trickle Down) 재정렬을 수행합니다.
 * * @param q: 큐 핸들
 * @return 가장 높은 우선순위의 아이템, 큐가 비어있으면 NULL
 */
void *pqueue_pop(pqueue_t *q);

/**
 * @brief 특정 아이템을 큐에서 제거
 * * 루트가 아닌 임의의 위치에 있는 아이템을 제거합니다.
 * getpos 콜백을 통해 O(1)로 위치를 찾은 뒤 제거하고 재정렬합니다.
 * * @param q: 큐 핸들
 * @param d: 제거할 아이템의 포인터
 * @return 성공 시 0
 */
int pqueue_remove(pqueue_t *q, void *d);

/**
 * @brief 가장 높은 우선순위 아이템 확인 (제거하지 않음)
 * * 루트 노드를 슬쩍 봅니다(Peek). 큐의 상태는 변하지 않습니다.
 * * @param q: 큐 핸들
 * @return 가장 높은 우선순위의 아이템
 */
void *pqueue_peek(pqueue_t *q);

// /** 디버그용 프린트 함수 (생략됨) */
// void pqueue_print(pqueue_t *q, pqueue_print_entry_f print);

// /** 디버그용 덤프 함수 (생략됨) */
// void pqueue_dump(pqueue_t *q, pqueue_print_entry_f print);

/**
 * @brief 큐 유효성 검사 (디버그용)
 * * 힙 속성(부모가 자식보다 우선순위가 높은지)이 올바르게 유지되고 있는지 검사합니다.
 * * @internal 디버그 전용 함수
 * @param q: 큐 핸들
 * @return 유효하면 1, 아니면 0
 */
int pqueue_is_valid(pqueue_t *q);

#endif /* PQUEUE_H */
/** @} */