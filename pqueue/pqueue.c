// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2014, Volkan Yazıcı <volkan.yazici@gmail.com>
 * All rights reserved.
 */

#include "../nvmev.h"
#include "pqueue.h"

/* * 이진 힙 탐색을 위한 매크로
 * 배열 인덱스를 이용해 부모/자식 노드를 찾습니다.
 * (비트 연산자를 사용하여 속도를 최적화함)
 */
#define left(i) ((i) << 1)       // 왼쪽 자식 = i * 2
#define right(i) (((i) << 1) + 1) // 오른쪽 자식 = i * 2 + 1
#define parent(i) ((i) >> 1)      // 부모 노드 = i / 2

/**
 * @brief 우선순위 큐 초기화
 * * 큐 구조체와 내부 데이터 배열을 할당합니다.
 * 이진 힙 구현 편의상 인덱스 1부터 사용하기 위해 (n + 1) 크기로 할당합니다.
 */
pqueue_t *pqueue_init(size_t n, pqueue_cmp_pri_f cmppri, pqueue_get_pri_f getpri,
              pqueue_set_pri_f setpri, pqueue_get_pos_f getpos, pqueue_set_pos_f setpos)
{
    pqueue_t *q;

    // 커널 로그에 저작권 정보 출력 (한 번만)
    pr_info_once(NVMEV_DRV_NAME ": pqueue: "
             "Copyright (c) 2014, Volkan Yazıcı <volkan.yazici@gmail.com>. "
             "All rights reserved.\n");

    // 큐 구조체 메모리 할당 (커널 메모리)
    if (!(q = kmalloc(sizeof(pqueue_t), GFP_KERNEL)))
        return NULL;

    /* Need to allocate n+1 elements since element 0 isn't used. */
    // 0번 인덱스는 계산 편의를 위해 비워두므로 n+1개 할당
    NVMEV_DEBUG("{alloc} n=%ld, size=%ld\n", n, (n + 1) * sizeof(void *));
    if (!(q->d = kmalloc((n + 1) * sizeof(void *), GFP_KERNEL))) {
        kfree(q); // 실패 시 구조체 해제
        return NULL;
    }

    q->size = 1; // 현재 크기 1 (0번은 더미, 실제 데이터는 없음)
    q->avail = q->step = (n + 1); // 가용 크기 설정
    
    // 콜백 함수들 연결
    q->cmppri = cmppri;
    q->setpri = setpri;
    q->getpri = getpri;
    q->getpos = getpos;
    q->setpos = setpos;

    return q;
}

/**
 * @brief 큐 메모리 해제
 */
void pqueue_free(pqueue_t *q)
{
    kfree(q->d); // 내부 배열 해제
    kfree(q);    // 구조체 해제
}

/**
 * @brief 현재 저장된 아이템 개수 반환
 */
size_t pqueue_size(pqueue_t *q)
{
    /* queue element 0 exists but doesn't count since it isn't used. */
    return (q->size - 1); // 0번 인덱스는 제외하고 계산
}

/**
 * @brief [핵심 로직] 버블 업 (Bubble Up)
 * * 새로운 아이템이 추가되거나 우선순위가 높아졌을 때,
 * 부모 노드와 비교하며 위로 올라가는 함수입니다.
 * * @param i: 시작할 노드의 인덱스
 */
static void bubble_up(pqueue_t *q, size_t i)
{
    size_t parent_node;
    void *moving_node = q->d[i]; // 이동할 노드 임시 저장
    pqueue_pri_t moving_pri = q->getpri(moving_node); // 우선순위 가져오기

    // 부모가 존재하고(>1), 부모보다 내 우선순위가 더 높다면(cmppri 조건 만족) 반복
    for (parent_node = parent(i);
         ((i > 1) && q->cmppri(q->getpri(q->d[parent_node]), moving_pri));
         i = parent_node, parent_node = parent(i)) {
        
        q->d[i] = q->d[parent_node]; // 부모를 아래로 내림
        q->setpos(q->d[i], i);       // 부모의 위치 정보(pos) 갱신
    }

    q->d[i] = moving_node; // 최종 위치에 내 노드 안착
    q->setpos(moving_node, i); // 내 위치 정보 갱신
}

/**
 * @brief 우선순위가 더 높은 자식 노드 찾기
 * * 왼쪽 자식과 오른쪽 자식 중 누구와 자리를 바꿀지 결정합니다.
 */
static size_t maxchild(pqueue_t *q, size_t i)
{
    size_t child_node = left(i); // 일단 왼쪽 자식 선택

    if (child_node >= q->size) // 자식이 없으면 0 반환
        return 0;

    // 오른쪽 자식도 있고, 오른쪽 자식이 왼쪽보다 우선순위가 높다면?
    if ((child_node + 1) < q->size &&
        q->cmppri(q->getpri(q->d[child_node]), q->getpri(q->d[child_node + 1])))
        child_node++; /* use right child instead of left */ // 오른쪽 자식 선택

    return child_node; // 선택된 자식 인덱스 반환
}

/**
 * @brief [핵심 로직] 퍼콜레이트 다운 (Percolate Down)
 * * 루트 노드가 제거되거나 우선순위가 낮아졌을 때,
 * 자식 노드들과 비교하며 아래로 내려가는 함수입니다.
 * * @param i: 시작할 노드의 인덱스
 */
static void percolate_down(pqueue_t *q, size_t i)
{
    size_t child_node;
    void *moving_node = q->d[i]; // 이동할 노드
    pqueue_pri_t moving_pri = q->getpri(moving_node);

    // 자식이 있고, 자식이 나보다 우선순위가 높다면 반복
    while ((child_node = maxchild(q, i)) &&
           q->cmppri(moving_pri, q->getpri(q->d[child_node]))) {
        
        q->d[i] = q->d[child_node]; // 자식을 위로 올림
        q->setpos(q->d[i], i);      // 자식의 위치 정보 갱신
        i = child_node;             // 나는 자식 위치로 이동
    }

    q->d[i] = moving_node; // 최종 위치 안착
    q->setpos(moving_node, i);
}

/**
 * @brief 큐에 아이템 삽입
 */
int pqueue_insert(pqueue_t *q, void *d)
{
    size_t i;

    if (!q) return 1;

    /* allocate more memory if necessary */
    // 큐가 꽉 찼을 때 메모리 재할당 로직 (현재는 단순히 에러 출력하고 멈춤)
    // 임베디드나 커널 환경에서는 재할당을 피하고 처음에 넉넉히 잡는 경우가 많음
    if (q->size >= q->avail) {
        NVMEV_ERROR("Need more space in pqueue\n");
        // realloc 로직 주석 처리됨
    }

    /* insert item */
    i = q->size++; // 큐 크기 증가시키고 끝 위치 확보
    q->d[i] = d;   // 끝에 데이터 저장
    bubble_up(q, i); // 위로 올리면서 제자리 찾기

    return 0;
}

/**
 * @brief 우선순위 변경 (FTL 핵심 기능)
 * * 블록의 유효 페이지 수(VPC)가 변하면 호출됩니다.
 * 이전 우선순위와 비교해서 위로 보낼지 아래로 보낼지 결정합니다.
 */
void pqueue_change_priority(pqueue_t *q, pqueue_pri_t new_pri, void *d)
{
    size_t posn;
    pqueue_pri_t old_pri = q->getpri(d); // 기존 우선순위 확인

    q->setpri(d, new_pri); // 새 우선순위 설정
    posn = q->getpos(d);   // 현재 위치 확인 (O(1))

    // 비교 함수(cmppri)의 리턴값에 따라 방향 결정
    // 예: Min-Heap에서 값이 작아졌으면 위로(bubble_up), 커졌으면 아래로(percolate_down)
    if (q->cmppri(old_pri, new_pri))
        bubble_up(q, posn);
    else
        percolate_down(q, posn);
}

/**
 * @brief 임의의 아이템 제거
 * * 큐 중간에 있는 아이템을 제거할 때 사용합니다.
 */
int pqueue_remove(pqueue_t *q, void *d)
{
    size_t posn = q->getpos(d); // 제거할 놈 위치 찾기
    
    // 맨 끝에 있는 놈을 제거할 놈 위치로 덮어씌움 (빈 공간 채우기)
    q->d[posn] = q->d[--q->size]; 
    
    // 덮어씌워진 놈(원래 맨 끝에 있던 놈)을 제자리 찾아주기
    if (q->cmppri(q->getpri(d), q->getpri(q->d[posn])))
        bubble_up(q, posn);
    else
        percolate_down(q, posn);

    return 0;
}

/**
 * @brief 최우선순위 아이템 추출 (Pop)
 * * 보통 GC 대상(Victim)을 꺼낼 때 사용합니다.
 */
void *pqueue_pop(pqueue_t *q)
{
    void *head;

    if (!q || q->size == 1) // 큐가 비었으면 NULL
        return NULL;

    head = q->d[1]; // 1번(루트) 아이템 꺼냄
    q->d[1] = q->d[--q->size]; // 맨 끝 아이템을 루트로 올림
    percolate_down(q, 1); // 루트로 올라온 놈을 제자리로 내림

    return head;
}

/**
 * @brief 최우선순위 아이템 확인 (Peek)
 * * 제거하지 않고 누가 1등인지만 확인합니다.
 */
void *pqueue_peek(pqueue_t *q)
{
    void *d;

    // 1. 큐가 비었나 체크
    if (!q || q->size == 1)
        return NULL;

    // 2. 1번 방(루트)에 있는 주소값 복사
    d = q->d[1]; 
    
    // 3. 반환 (큐 상태 변화 없음)
    return d;
}

// ... (디버그용 프린트 함수 생략) ...

/**
 * @brief 큐 유효성 검사 (재귀 함수)
 * * 모든 노드가 힙 속성(부모 우선순위 >= 자식 우선순위)을 만족하는지 검사합니다.
 */
static int subtree_is_valid(pqueue_t *q, int pos)
{
    if (left(pos) < q->size) {
        /* has a left child */
        // 부모와 왼쪽 자식 비교해서 힙 속성 깨졌으면 0 리턴
        if (q->cmppri(q->getpri(q->d[pos]), q->getpri(q->d[left(pos)])))
            return 0;
        // 왼쪽 서브트리도 재귀적으로 검사
        if (!subtree_is_valid(q, left(pos)))
            return 0;
    }
    if (right(pos) < q->size) {
        /* has a right child */
        // 부모와 오른쪽 자식 비교
        if (q->cmppri(q->getpri(q->d[pos]), q->getpri(q->d[right(pos)])))
            return 0;
        // 오른쪽 서브트리 검사
        if (!subtree_is_valid(q, right(pos)))
            return 0;
    }
    return 1; // 문제 없으면 1
}

int pqueue_is_valid(pqueue_t *q)
{
    return subtree_is_valid(q, 1); // 루트(1번)부터 검사 시작
}