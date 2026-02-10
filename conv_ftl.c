// SPDX-License-Identifier: GPL-2.0-only

#include <linux/vmalloc.h> // vmalloc 관련 헤더 (대용량 메모리 할당)
#include <linux/ktime.h>   // 커널 시간 관련 헤더
#include <linux/sched/clock.h> // 스케줄러 시계 관련 헤더
#include <linux/moduleparam.h> // 파라미터 사용을 위한 헤더
#include <linux/random.h> // get_random_u32() 함수 사용을 위해 필수
#include <linux/types.h>

#include "nvmev.h"      // NVMeVirt 공통 헤더
#include "conv_ftl.h"   // Conventional FTL 헤더

// conv_ftl.c 상단 전역 변수 영역
static int gc_mode = 0; // 0:Greedy, 1:CB, 2:Random
static int debug_mode = 0;    // debug mode == 1;
module_param(gc_mode, int, 0644); // insmod 할 때 gc_mode=2 처럼 입력 가능
module_param(debug_mode, int, 0644);
/* ========================================================= */
/* [Meen's Debug] Hot/Cold GC 카운터 및 기준 설정 */
/* 150MB 지점 LPN = 38400 (FIO 스크립트 기준) */
#define HOT_REGION_LPN_LIMIT  38400

static unsigned long total_gc_cnt = 0; // 총 GC 횟수
static unsigned long hot_gc_cnt = 0;   // Hot 블록이 잡힌 횟수
static unsigned long cold_gc_cnt = 0;  // Cold 블록이 잡힌 횟수
/* ========================================================= */
static uint64_t victim_total_age = 0;
static uint64_t victim_chosen_cnt = 0;
// 현재 페이지가 워드라인(Wordline)의 마지막 페이지인지 확인하는 함수
static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp; // SSD 파라미터 가져오기
    // 현재 페이지 번호가 원샷(One-shot) 프로그래밍 단위의 끝인지 계산
    return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

// 가비지 컬렉션(GC)이 필요한지 확인하는 함수 (기본 임계값)
static bool should_gc(struct conv_ftl *conv_ftl)
{
    // 현재 남은 프리 라인(Free Line) 개수가 GC 시작 임계값 이하인지 확인
    return (conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines);
}

// 긴급 GC가 필요한지 확인하는 함수 (높은 임계값)
static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
    // 프리 라인이 매우 부족한 상황인지 확인 (Foreground GC 트리거 용)
    return conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines_high;
}

// 매핑 테이블에서 LPN(논리 페이지 번호)에 해당하는 PPA(물리 주소)를 가져오는 함수
static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
    return conv_ftl->maptbl[lpn]; // 배열에서 해당 LPN의 PPA 반환
}

// 매핑 테이블에 LPN과 PPA 매핑 정보를 기록하는 함수
static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
    NVMEV_ASSERT(lpn < conv_ftl->ssd->sp.tt_pgs); // LPN이 유효 범위 내인지 확인
    conv_ftl->maptbl[lpn] = *ppa; // 매핑 테이블 업데이트
}

// PPA(구조체 주소)를 선형적인 페이지 인덱스(정수)로 변환하는 함수
static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp; // SSD 파라미터
    uint64_t pgidx; // 결과 인덱스 저장 변수

    // 디버깅용: 현재 주소 정보 출력
    NVMEV_DEBUG_VERBOSE("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__,
            ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

    // 채널, LUN, 플레인, 블록, 페이지 정보를 이용해 고유 인덱스 계산
    pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
        ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

    NVMEV_ASSERT(pgidx < spp->tt_pgs); // 계산된 인덱스가 전체 페이지 수 범위 내인지 확인

    return pgidx; // 계산된 인덱스 반환
}

// 역매핑 테이블(Reverse Map)에서 물리 주소에 매핑된 LPN을 가져오는 함수
static inline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(conv_ftl, ppa); // PPA를 인덱스로 변환

    return conv_ftl->rmap[pgidx]; // 해당 물리 위치의 LPN 반환
}

/* set rmap[page_no(ppa)] -> lpn */
// 역매핑 테이블에 물리 주소와 LPN 매핑 정보를 기록하는 함수
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(conv_ftl, ppa); // PPA를 인덱스로 변환

    conv_ftl->rmap[pgidx] = lpn; // 역매핑 테이블 업데이트
}
/*전략별 pqueue사용 및 우선순위 조회 함수*/
// Greedy용 우선순위 조회: VPC(유효 페이지)가 점수
static pqueue_pri_t get_pri_greedy(void *a)
{
    return ((struct line *)a)->vpc;
}

// Greedy용 비교 함수: 부모(next)가 자식(curr)보다 크면 교체 -> 즉, 작은 값이 위로 (Min-Heap)
static int cmp_pri_greedy(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}
//CB, RD는 더미로 떼우기
static pqueue_pri_t get_pri_dummy(void *a) { return 0; }
static int cmp_pri_dummy(pqueue_pri_t next, pqueue_pri_t curr) { return 0; }
static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}
// 쓰기 크레딧(Write Credit)을 하나 소모하는 함수
static inline void consume_write_credit(struct conv_ftl *conv_ftl)
{
    conv_ftl->wfc.write_credits--; // 크레딧 감소 (쓰기 허용량 차감)
}

// 전경(Foreground) GC 함수 선언
static void foreground_gc(struct conv_ftl *conv_ftl);

// 쓰기 크레딧을 확인하고 부족하면 GC를 수행해 채우는 함수


static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl)
{
    struct write_flow_control *wfc = &(conv_ftl->wfc); // 유량 제어 구조체
    if (wfc->write_credits <= 0) { // 크레딧이 바닥나면
        foreground_gc(conv_ftl); // 강제로 GC 수행 (공간 확보)

        wfc->write_credits += wfc->credits_to_refill; // 크레딧 리필
    }
}

// ---------------------------------------------------------
// 전략 1: Greedy (기존 방식) - PQ의 Root(1등) 사용
// ---------------------------------------------------------
static struct line *select_victim_greedy(struct conv_ftl *conv_ftl, bool force)
{

    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct line_mgmt *lm = &conv_ftl->lm;
    struct line *victim_line = pqueue_peek(lm->victim_line_pq); // 1등 확인

    if (!victim_line) return NULL;
    
    // Greedy 특유의 효율성 체크 (VPC가 너무 많으면 GC 안 함)
    if (!force && (victim_line->vpc > (conv_ftl->ssd->sp.pgs_per_line / 8))) {
        return NULL;
    }
    victim_total_age += (ktime_get_ns()-(victim_line->last_modified_time))/1000000;
    victim_chosen_cnt++;
    pqueue_pop(lm->victim_line_pq); // 1등 꺼내기
    victim_line->pos = 0;
    lm->victim_line_cnt--;
    return victim_line;
}

// ---------------------------------------------------------
// 전략 2: Random - 배열 인덱스로 콕 찍기 (O(1))
// ---------------------------------------------------------
static struct line *select_victim_random(struct conv_ftl *conv_ftl, bool force)
{

    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct line_mgmt *lm = &conv_ftl->lm;
    pqueue_t *q = lm->victim_line_pq;   //pqueue를 그대로 가져옴 원본을
    
    if (q->size == 0) return NULL; // 비어있으면 종료

    // 난수 생성하여 인덱스 바로 접근 (Linear Scan 아님!) 시간복잡도 O(1)으로 예상
    size_t rand_idx = (get_random_u32() % q->size) + 1;
    struct line *victim_line = (struct line *)q->d[rand_idx];

    // 선택된 녀석을 큐에서 강제로 제거 (중간 빼기)
    pqueue_remove(q, victim_line);
    
    victim_line->pos = 0;
    lm->victim_line_cnt--;
    return victim_line;
}


/* 시간 단위 매크로 (가독성 향상) */
#define MS_TO_NS(x)     ((uint64_t)(x) * 1000000ULL)
#define SEC_TO_NS(x)    ((uint64_t)(x) * 1000000000ULL)

/* * [로그 데이터 기반 튜닝]
 * - 0 ~ 100ms : Very Hot (로그의 7ms 데이터 보호)
 * - 100ms ~ 5s : Hot (로그의 1.19s 데이터 보호)
 * - 5s ~ 60s  : Warm (로그의 12s, 22s 데이터 유예)
 * - 60s ~     : Cold (로그의 120s~169s 데이터들은 모두 여기 포함)
 */
#define THRESHOLD_VERY_HOT  MS_TO_NS(100)
#define THRESHOLD_HOT       SEC_TO_NS(5)
#define THRESHOLD_WARM      SEC_TO_NS(60)

/*
 * Age Weight Function (구간별 계단 함수)
 * 반환값: 가중치 (클수록 GC 대상이 될 확률 높음)
 */
static uint64_t get_age_weight(uint64_t age_ns)
{
    // [Level 0] Very Hot (0 ~ 100ms)
    // 방금 기록됨. 참조 국지성(Locality)에 의해 곧 다시 쓰일 확률 매우 높음.
    // 절대 건드리지 않도록 최소 가중치 부여.
    if (age_ns < THRESHOLD_VERY_HOT) {
        return 1; 
    }
    
    // [Level 1] Hot (100ms ~ 5s)
    // 1초 내외의 데이터. 아직 활성 상태로 간주.
    else if (age_ns < THRESHOLD_HOT) {
        return 5;
    }
    
    // [Level 2] Warm (5s ~ 60s)
    // 12초, 22초 등 로그에서 보인 '식어가는' 데이터들.
    // 당장 급하지 않다면 놔두는 게 좋음.
    else if (age_ns < THRESHOLD_WARM) {
        return 20;
    }
    
    // [Level 3] Cold / Frozen (60s ~ )
    // 로그에서 120초~169초 데이터가 대거 발견됨.
    // 이들은 사실상 '정적 데이터(Static Data)'이므로
    // 무효 페이지(IPC)만 조금 있어도 즉시 청소하는 게 이득.
    else {
        return 100; // 가중치 최대 (GC 1순위)
    }
}
// ---------------------------------------------------------
// 전략 3: Cost-Benefit - 전체 스캔 (Linear Scan, O(N))
// ---------------------------------------------------------
// Cost-Benefit 정책을 사용하여 희생 라인(Victim Line)을 선택하는 함수
// (Heap의 정렬을 무시하고 전체를 뒤져서 최적의 대상을 찾음)
static struct line *select_victim_cb(struct conv_ftl *conv_ftl, bool force)
{

    struct ssdparams *spp = &conv_ftl->ssd->sp;
    // 1. 편의를 위해 긴 구조체 경로를 짧은 변수명(lm)으로 할당
    struct line_mgmt *lm = &conv_ftl->lm;
    // 2. 우선순위 큐 구조체 포인터 가져오기
    pqueue_t *q = lm->victim_line_pq;
    // 3. 현재까지 찾은 '최고의 희생양'을 저장할 포인터 초기화
    struct line *best_victim = NULL;
    
    // 4. 현재까지 발견된 '최고 점수'를 저장할 변수 (초기값 0 또는 -1)
    uint64_t max_score = 0; 
    uint64_t victim_age = 0;
    // 5. 나이(Age) 계산을 위해 현재 커널 시간(나노초 단위)을 가져옴
    uint64_t now = ktime_get_ns();
    
    // 6. 반복문 제어를 위한 인덱스 변수
    size_t i;

    // 7. 큐(Victim List)가 비어있다면, 청소할 블록이 없으므로 NULL 반환
    if (q->size == 0) return NULL;

    // ★ 8. 큐 내부 배열(d)을 처음부터 끝까지 순회 (Linear Scan, O(N))
    // - 이유: '시간(Age)'은 모든 블록에 대해 동시에 흐르므로, 
    //   특정 시점에 고정된 우선순위 큐(Heap)로는 최신 점수를 반영할 수 없음.
    // - i = 1부터 시작하는 이유: pqueue 라이브러리는 0번 인덱스를 더미(비움)로 쓰고 1번부터 저장함
    for (i = 1; i <= q->size; i++) {
        // 9. 현재 인덱스(i)에 있는 라인(블록) 포인터를 가져옴 (void* -> struct line*)
        struct line *cand = (struct line *)q->d[i];
        if (!cand) {
            // NULL이면 이 항목은 건너뛰고 계속 진행
            continue;
        }
        // 10. Age(나이) 계산: (현재 시간 - 마지막으로 데이터가 쓰인 시간)
        // - 삼항 연산자: 혹시라도 시간이 역전되는 오류를 방지하기 위해 0 처리
        uint64_t age = (now > cand->last_modified_time) ? 
                       (now - cand->last_modified_time) : 0;
      
        
        uint64_t age_weight = get_age_weight(age);
        // 11. Cost-Benefit 점수 계산
        // - 공식: Benefit(Age * IPC) / Cost(2 * VPC)
        // - (cand->vpc + 1): 분모가 0이 되어 프로그램이 죽는 것을 방지
        uint64_t numerator= age_weight * cand->ipc;
        uint64_t score =  numerator / (cand->vpc + 1);
        // 12. 현재 블록의 점수가 지금까지 찾은 최대 점수보다 높은지 확인
        if (score > max_score) {
            // 13. 최고 점수 갱신
            max_score = score;
            victim_age = age;
            // 14. 현재 블록을 '최고의 희생양' 후보로 등록
            best_victim = cand;
        }
    }

    // 15. 전체를 다 뒤져서 희생양(best_victim)을 찾았다면
    if (best_victim) {
        // 16. 우선순위 큐에서 해당 라인을 '안전하게' 제거
        // (pqueue_pop은 맨 위만 빼지만, remove는 중간에 있는 놈을 빼고 트리를 재정렬함)
        victim_total_age += victim_age / 1000000;
        victim_chosen_cnt++;
        pqueue_remove(q, best_victim); 
        // 17. 해당 라인의 큐 위치 정보 초기화 (큐에서 빠졌음을 표시)
        best_victim->pos = 0;
        
        // 18. 전체 희생 후보 라인 개수 감소
        lm->victim_line_cnt--;
    }

    // 19. 최종 선택된 희생 라인 반환 (이후 do_gc 함수가 이 블록을 청소함)
    return best_victim;
}
// 라인(블록 관리 단위) 초기화 함수
static void init_lines(struct conv_ftl *conv_ftl)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp; // SSD 파라미터
    struct line_mgmt *lm = &conv_ftl->lm; // 라인 관리자

    struct line *line;

    pqueue_cmp_pri_f cmp_func;
    pqueue_get_pri_f get_func;

    int i;
    switch (gc_mode) {
    case GC_MODE_RANDOM:
        NVMEV_INFO("GC Strategy: RANDOM\n");
        lm->select_victim = select_victim_random; // 함수 연결
        // Random은 정렬이 필요 없으니 PQ 비교 함수는 아무거나(Greedy용) 넣어도 됨
        cmp_func = cmp_pri_dummy;
        get_func = get_pri_dummy;
        break;

    case GC_MODE_COST_BENEFIT:
        NVMEV_INFO("GC Strategy: COST-BENEFIT (Linear Scan)\n");
        lm->select_victim = select_victim_cb; // 함수 연결
        cmp_func = cmp_pri_dummy; // CB도 스캔할 거면 PQ 정렬은 의미 없음
        get_func = get_pri_dummy;   
        break;

    case GC_MODE_GREEDY:
    default:
        NVMEV_INFO("GC Strategy: GREEDY\n");
        lm->select_victim = select_victim_greedy; // 함수 연결
        cmp_func = cmp_pri_greedy;
        get_func = get_pri_greedy;
        break;
    }

    lm->tt_lines = spp->blks_per_pl; // 전체 라인 수 설정
    NVMEV_ASSERT(lm->tt_lines == spp->tt_lines); // 라인 수 검증
    lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines); // 라인 구조체 배열 메모리 할당

    INIT_LIST_HEAD(&lm->free_line_list); // 프리 라인 리스트 초기화
    INIT_LIST_HEAD(&lm->full_line_list); // 풀(Full) 라인 리스트 초기화

    // 희생 라인 선정을 위한 우선순위 큐 초기화 (Greedy 정책 등 적용)
    lm->victim_line_pq = pqueue_init(spp->tt_lines, 
                                        cmp_func, 
                                        get_func,    
                                        victim_line_set_pri,
                                        victim_line_get_pos,
                                        victim_line_set_pos);

    lm->free_line_cnt = 0; // 프리 라인 카운트 초기화
    for (i = 0; i < lm->tt_lines; i++) { // 모든 라인에 대해 루프
        lm->lines[i] = (struct line){
            .id = i, // 라인 ID 설정
            .ipc = 0, // 무효 페이지 수 0
            .vpc = 0, // 유효 페이지 수 0
            .pos = 0, // 큐 위치 0
            .last_modified_time = 0, 
            .entry = LIST_HEAD_INIT(lm->lines[i].entry), // 리스트 엔트리 초기화
        };

        /* initialize all the lines as free lines */
        // 초기에는 모든 라인을 프리 라인 리스트에 추가
        list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
        lm->free_line_cnt++; // 프리 라인 카운트 증가
    }

    NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines); // 모든 라인이 프리 상태인지 확인
    lm->victim_line_cnt = 0; // 희생 후보 라인 수 0
    lm->full_line_cnt = 0; // 꽉 찬 라인 수 0
}

// 라인 관련 메모리 해제 함수
static void remove_lines(struct conv_ftl *conv_ftl)
{
    pqueue_free(conv_ftl->lm.victim_line_pq); // 우선순위 큐 해제
    vfree(conv_ftl->lm.lines); // 라인 구조체 배열 해제
}

// 쓰기 유량 제어 초기화 함수
static void init_write_flow_control(struct conv_ftl *conv_ftl)
{
    struct write_flow_control *wfc = &(conv_ftl->wfc); // 제어 구조체
    struct ssdparams *spp = &conv_ftl->ssd->sp; // 파라미터

    wfc->write_credits = spp->pgs_per_line; // 초기 크레딧 설정 (한 라인 크기)
    wfc->credits_to_refill = spp->pgs_per_line; // 리필 양 설정
}

// 주소 유효성 검사 함수
static inline void check_addr(int a, int max)
{
    NVMEV_ASSERT(a >= 0 && a < max); // 값이 0 이상 max 미만인지 확인
}

// 프리 라인 리스트에서 다음 빈 라인을 가져오는 함수
static struct line *get_next_free_line(struct conv_ftl *conv_ftl)
{
    struct line_mgmt *lm = &conv_ftl->lm; // 라인 관리자
    // 프리 라인 리스트의 첫 번째 항목 가져오기
    struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

    if (!curline) { // 프리 라인이 없으면
        NVMEV_ERROR("No free line left in VIRT !!!!\n"); // 에러 출력
        return NULL; // NULL 반환
    }

    list_del_init(&curline->entry); // 리스트에서 제거
    lm->free_line_cnt--; // 프리 라인 카운트 감소
    NVMEV_DEBUG("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt); // 디버그 로그
    return curline; // 라인 반환
}

// IO 타입(유저 IO / GC IO)에 따른 쓰기 포인터 가져오는 함수
static struct write_pointer *__get_wp(struct conv_ftl *ftl, uint32_t io_type)
{
    if (io_type == USER_IO) { // 유저 쓰기인 경우
        return &ftl->wp; // 유저용 쓰기 포인터 반환
    } else if (io_type == GC_IO) { // GC 쓰기(Valid Page Copy)인 경우
        return &ftl->gc_wp; // GC용 쓰기 포인터 반환
    }

    NVMEV_ASSERT(0); // 알 수 없는 IO 타입이면 에러
    return NULL;
}

// 쓰기 포인터 초기화 및 첫 블록 할당 함수
static void prepare_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
    struct write_pointer *wp = __get_wp(conv_ftl, io_type); // 해당 타입의 포인터 가져오기
    struct line *curline = get_next_free_line(conv_ftl); // 새 빈 라인 할당

    NVMEV_ASSERT(wp); // 포인터 유효성 검사
    NVMEV_ASSERT(curline); // 라인 유효성 검사

    /* wp->curline is always our next-to-write super-block */
    // 쓰기 포인터 구조체 초기화 (0번 채널, 0번 LUN, 0번 페이지부터 시작)
    *wp = (struct write_pointer){
        .curline = curline,
        .ch = 0,
        .lun = 0,
        .pg = 0,
        .blk = curline->id, // 할당받은 라인의 ID가 블록 번호가 됨
        .pl = 0,
    };
}

// 쓰기 포인터를 다음 위치로 이동시키는 함수 (핵심 로직: 페이지->채널->LUN 순회)
static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp; // SSD 파라미터
    struct line_mgmt *lm = &conv_ftl->lm; // 라인 관리자
    struct write_pointer *wpp = __get_wp(conv_ftl, io_type); // 현재 쓰기 포인터

    // 디버그용: 현재 포인터 위치 출력
    NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
            wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

    check_addr(wpp->pg, spp->pgs_per_blk); // 페이지 주소 검사
    wpp->pg++; // 페이지 번호 증가
    if ((wpp->pg % spp->pgs_per_oneshotpg) != 0) // 원샷 프로그램 단위 안이면
        goto out; // 단순히 페이지만 증가시키고 종료

    wpp->pg -= spp->pgs_per_oneshotpg; // 페이지 번호 조정
    check_addr(wpp->ch, spp->nchs); // 채널 주소 검사
    wpp->ch++; // 채널 번호 증가 (채널 인터리빙)
    if (wpp->ch != spp->nchs) // 마지막 채널이 아니면
        goto out; // 종료

    wpp->ch = 0; // 채널 0으로 리셋
    check_addr(wpp->lun, spp->luns_per_ch); // LUN 주소 검사
    wpp->lun++; // LUN 번호 증가 (LUN 인터리빙)
    /* in this case, we should go to next lun */
    if (wpp->lun != spp->luns_per_ch) // 마지막 LUN이 아니면
        goto out; // 종료

    wpp->lun = 0; // LUN 0으로 리셋
    /* go to next wordline in the block */
    wpp->pg += spp->pgs_per_oneshotpg; // 다음 워드라인으로 페이지 이동
    if (wpp->pg != spp->pgs_per_blk) // 블록의 끝이 아니면
        goto out; // 종료

    // 블록이 가득 찬 경우:
    wpp->pg = 0; // 페이지 0으로 리셋
    /* move current line to {victim,full} line list */
    if (wpp->curline->vpc == spp->pgs_per_line) { // 모든 페이지가 유효하면 (Full)
        /* all pgs are still valid, move to full line list */
        NVMEV_ASSERT(wpp->curline->ipc == 0); // 무효 페이지는 0이어야 함
        list_add_tail(&wpp->curline->entry, &lm->full_line_list); // 풀 라인 리스트로 이동
        lm->full_line_cnt++; // 풀 라인 카운트 증가
        NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
    } else { // 무효 페이지가 섞여있으면 (Victim 후보)
        NVMEV_DEBUG_VERBOSE("wpp: line is moved to victim list\n");
        NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
        /* there must be some invalid pages in this line */
        NVMEV_ASSERT(wpp->curline->ipc > 0); // 무효 페이지가 반드시 존재해야 함
        pqueue_insert(lm->victim_line_pq, wpp->curline); // 희생 라인 우선순위 큐에 삽입
        lm->victim_line_cnt++; // 희생 라인 카운트 증가
    }
    /* current line is used up, pick another empty line */
    check_addr(wpp->blk, spp->blks_per_pl); // 블록 주소 검사
    wpp->curline = get_next_free_line(conv_ftl); // 새 프리 라인(오픈 블록) 가져오기
    NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

    wpp->blk = wpp->curline->id; // 새 라인의 ID를 현재 블록으로 설정
    check_addr(wpp->blk, spp->blks_per_pl); // 블록 주소 검사

    /* make sure we are starting from page 0 in the super block */
    // 새 블록은 처음부터 시작해야 함을 검증
    NVMEV_ASSERT(wpp->pg == 0);
    NVMEV_ASSERT(wpp->lun == 0);
    NVMEV_ASSERT(wpp->ch == 0);
    /* TODO: assume # of pl_per_lun is 1, fix later */
    NVMEV_ASSERT(wpp->pl == 0); // 플레인 0 가정
out:
    // 이동 완료 후 현재 위치 출력
    NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
            wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

// 현재 쓰기 포인터 위치를 기반으로 새 페이지(PPA)를 생성하여 반환하는 함수
static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
{
    struct ppa ppa;
    struct write_pointer *wp = __get_wp(conv_ftl, io_type); // 쓰기 포인터 가져오기

    ppa.ppa = 0; // PPA 초기화
    ppa.g.ch = wp->ch; // 현재 채널
    ppa.g.lun = wp->lun; // 현재 LUN
    ppa.g.pg = wp->pg; // 현재 페이지
    ppa.g.blk = wp->blk; // 현재 블록
    ppa.g.pl = wp->pl; // 현재 플레인

    NVMEV_ASSERT(ppa.g.pl == 0); // 플레인은 0이어야 함 (단일 플레인 가정)

    return ppa; // 생성된 PPA 반환
}

// 매핑 테이블 초기화 함수
static void init_maptbl(struct conv_ftl *conv_ftl)
{
    int i;
    struct ssdparams *spp = &conv_ftl->ssd->sp;

    conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs); // 전체 페이지 수만큼 할당
    for (i = 0; i < spp->tt_pgs; i++) {
        conv_ftl->maptbl[i].ppa = UNMAPPED_PPA; // 초기값은 '매핑 안됨'으로 설정
    }
}

// 매핑 테이블 해제 함수
static void remove_maptbl(struct conv_ftl *conv_ftl)
{
    vfree(conv_ftl->maptbl); // 메모리 해제
}

// 역매핑 테이블 초기화 함수
static void init_rmap(struct conv_ftl *conv_ftl)
{
    int i;
    struct ssdparams *spp = &conv_ftl->ssd->sp;

    conv_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs); // 전체 페이지 수만큼 할당
    for (i = 0; i < spp->tt_pgs; i++) {
        conv_ftl->rmap[i] = INVALID_LPN; // 초기값은 '유효하지 않은 LPN'으로 설정
    }
}

// 역매핑 테이블 해제 함수
static void remove_rmap(struct conv_ftl *conv_ftl)
{
    vfree(conv_ftl->rmap); // 메모리 해제
}

// FTL 인스턴스 초기화 함수
static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd)
{
    /*copy convparams*/
    conv_ftl->cp = *cpp; // 파라미터 복사

    conv_ftl->ssd = ssd; // SSD 포인터 연결

    conv_ftl->gc_count = 0;
    conv_ftl->gc_copied_pages = 0;
    /* initialize maptbl */
    init_maptbl(conv_ftl); // 매핑 테이블 할당 및 초기화

    /* initialize rmap */
    init_rmap(conv_ftl); // 역매핑 테이블 할당 및 초기화

    /* initialize all the lines */
    init_lines(conv_ftl); // 라인 관리 구조체 초기화

    /* initialize write pointer, this is how we allocate new pages for writes */
    prepare_write_pointer(conv_ftl, USER_IO); // 유저 쓰기 포인터 준비
    prepare_write_pointer(conv_ftl, GC_IO);   // GC 쓰기 포인터 준비

    init_write_flow_control(conv_ftl); // 쓰기 유량 제어 초기화

    // 초기화 완료 로그 출력
    NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", conv_ftl->ssd->sp.nchs,
           conv_ftl->ssd->sp.tt_pgs);

    return;
}

// FTL 인스턴스 제거 함수
static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
    remove_lines(conv_ftl); // 라인 해제
    remove_rmap(conv_ftl);  // 역매핑 테이블 해제
    remove_maptbl(conv_ftl); // 매핑 테이블 해제
}

// FTL 파라미터 기본값 설정 함수
static void conv_init_params(struct convparams *cpp)
{
    cpp->op_area_pcent = OP_AREA_PERCENT; // 오버 프로비저닝 비율
    cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/ // GC 시작 임계값 (최소 2개)
    cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/ // 긴급 GC 임계값
    cpp->enable_gc_delay = 1; // GC 지연 시뮬레이션 활성화
    cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100); // 물리 공간 비율 계산
}

// 네임스페이스(NVMe Namespace) 초기화 함수
void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
             uint32_t cpu_nr_dispatcher)
{
    struct ssdparams spp;
    struct convparams cpp;
    struct conv_ftl *conv_ftls;
    struct ssd *ssd;
    uint32_t i;
    const uint32_t nr_parts = SSD_PARTITIONS; // 파티션 수

    ssd_init_params(&spp, size, nr_parts); // SSD 파라미터 초기화
    conv_init_params(&cpp); // FTL 파라미터 초기화

    conv_ftls = kmalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL); // FTL 인스턴스 배열 할당

    for (i = 0; i < nr_parts; i++) { // 각 파티션마다 루프
        ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL); // SSD 구조체 할당
        ssd_init(ssd, &spp, cpu_nr_dispatcher); // SSD 초기화
        conv_init_ftl(&conv_ftls[i], &cpp, ssd); // FTL 초기화
    }

    /* PCIe, Write buffer are shared by all instances*/
    // PCIe 인터페이스와 쓰기 버퍼는 모든 인스턴스가 공유함
    for (i = 1; i < nr_parts; i++) {
        kfree(conv_ftls[i].ssd->pcie->perf_model); // 중복 할당된 것 해제
        kfree(conv_ftls[i].ssd->pcie);
        kfree(conv_ftls[i].ssd->write_buffer);

        conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie; // 0번 인스턴스의 것을 공유
        conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
    }

    ns->id = id; // 네임스페이스 ID 설정
    ns->csi = NVME_CSI_NVM; // 커맨드 셋 식별자
    ns->nr_parts = nr_parts; // 파티션 수
    ns->ftls = (void *)conv_ftls; // FTL 포인터 연결
    ns->size = (uint64_t)((size * 100) / cpp.pba_pcent); // 논리적 크기 계산
    ns->mapped = mapped_addr; // 매핑된 주소
    /*register io command handler*/
    ns->proc_io_cmd = conv_proc_nvme_io_cmd; // IO 처리 핸들러 등록

    // 정보 출력 로그
    NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
           size, ns->size, cpp.pba_pcent);

    return;
}

// 네임스페이스 제거 함수
void conv_remove_namespace(struct nvmev_ns *ns)
{
    struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls; // FTL 인스턴스 가져오기
    const uint32_t nr_parts = SSD_PARTITIONS;
    uint32_t i;

    /* PCIe, Write buffer are shared by all instances*/
    for (i = 1; i < nr_parts; i++) {
        /*
         * These were freed from conv_init_namespace() already.
         * Mark these NULL so that ssd_remove() skips it.
         */
        // 공유 자원은 중복 해제 방지를 위해 NULL 처리
        conv_ftls[i].ssd->pcie = NULL;
        conv_ftls[i].ssd->write_buffer = NULL;
    }

    for (i = 0; i < nr_parts; i++) {
        conv_remove_ftl(&conv_ftls[i]); // FTL 내부 해제
        ssd_remove(conv_ftls[i].ssd); // SSD 내부 해제
        kfree(conv_ftls[i].ssd); // SSD 구조체 해제
    }

    kfree(conv_ftls); // FTL 배열 해제
    ns->ftls = NULL; // 포인터 초기화
}

// PPA 유효성 검사 함수
static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    //int sec = ppa->g.sec;

    // 각 주소 성분이 범위 내에 있는지 확인
    if (ch < 0 || ch >= spp->nchs)
        return false;
    if (lun < 0 || lun >= spp->luns_per_ch)
        return false;
    if (pl < 0 || pl >= spp->pls_per_lun)
        return false;
    if (blk < 0 || blk >= spp->blks_per_pl)
        return false;
    if (pg < 0 || pg >= spp->pgs_per_blk)
        return false;

    return true; // 모두 통과하면 유효함
}

// LPN 유효성 검사 함수
static inline bool valid_lpn(struct conv_ftl *conv_ftl, uint64_t lpn)
{
    // LPN이 전체 페이지 수보다 작은지 확인
    return (lpn < conv_ftl->ssd->sp.tt_pgs);
}

// PPA가 매핑되어 있는지 확인하는 함수
static inline bool mapped_ppa(struct ppa *ppa)
{
    // PPA가 UNMAPPED 상태가 아니면 매핑된 것임
    return !(ppa->ppa == UNMAPPED_PPA);
}

// PPA에 해당하는 라인(블록) 포인터를 가져오는 함수
static inline struct line *get_line(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    return &(conv_ftl->lm.lines[ppa->g.blk]); // 블록 인덱스를 이용해 라인 반환
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
// 페이지를 무효화(Invalid) 처리하는 함수 (덮어쓰기 발생 시 구버전 페이지 처리)
static void mark_page_invalid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp; // 파라미터
    struct line_mgmt *lm = &conv_ftl->lm; // 라인 관리자
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(conv_ftl->ssd, ppa); // 페이지 가져오기
    NVMEV_ASSERT(pg->status == PG_VALID); // 유효 상태였는지 확인
    pg->status = PG_INVALID; // 무효 상태로 변경
    
    /* update corresponding block status */
    blk = get_blk(conv_ftl->ssd, ppa); // 블록 가져오기
    NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++; // 블록 내 무효 페이지 수(IPC) 증가
    NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--; // 블록 내 유효 페이지 수(VPC) 감소

    /* update corresponding line status */
    line = get_line(conv_ftl, ppa); // 라인 가져오기
    NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) { // 기존에 꽉 찬 라인(Full Line)이었다면
        NVMEV_ASSERT(line->ipc == 0);
        was_full_line = true; // 플래그 설정
    }
    line->ipc++; // 라인 무효 페이지 증가
    NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) { // 이미 우선순위 큐(Victim List)에 있다면
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line); // 우선순위(VPC) 갱신
    } else {
        line->vpc--; // 큐에 없으면 VPC만 감소
    }

    if (was_full_line) { // 원래 Full Line 리스트에 있었다면
        /* move line: "full" -> "victim" */
        list_del_init(&line->entry); // 리스트에서 제거
        lm->full_line_cnt--; // Full 라인 수 감소
        pqueue_insert(lm->victim_line_pq, line); // Victim 우선순위 큐로 이동
        lm->victim_line_cnt++; // Victim 라인 수 증가
    }
    line->last_modified_time = ktime_get_ns(); 
}

// 페이지를 유효화(Valid) 처리하는 함수 (새 데이터 쓰기 시)
static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(conv_ftl->ssd, ppa); // 페이지 가져오기
    NVMEV_ASSERT(pg->status == PG_FREE); // 프리 상태였는지 확인
    pg->status = PG_VALID; // 유효 상태로 변경

    /* update corresponding block status */
    blk = get_blk(conv_ftl->ssd, ppa); // 블록 가져오기
    NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
    blk->vpc++; // 블록 유효 페이지 수 증가

    /* update corresponding line status */
    line = get_line(conv_ftl, ppa); // 라인 가져오기
    NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
    line->vpc++; // 라인 유효 페이지 수 증가
}

// 블록을 프리(Free) 상태로 초기화하는 함수 (Erase 수행 시)
static void mark_block_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct nand_block *blk = get_blk(conv_ftl->ssd, ppa); // 블록 가져오기
    struct nand_page *pg = NULL;
    int i;

    for (i = 0; i < spp->pgs_per_blk; i++) { // 블록 내 모든 페이지 루프
        /* reset page status */
        pg = &blk->pg[i];
        NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE; // 페이지 상태를 Free로 리셋
    }

    /* reset block status */
    NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0; // 무효 페이지 수 리셋
    blk->vpc = 0; // 유효 페이지 수 리셋
    blk->erase_cnt++; // 지우기 횟수(Erase Count) 증가
}

// GC 과정에서 페이지를 읽는 함수
static void gc_read_page(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct convparams *cpp = &conv_ftl->cp;
    /* advance conv_ftl status, we don't care about how long it takes */
    if (cpp->enable_gc_delay) { // GC 지연 시간 시뮬레이션이 켜져있으면
        struct nand_cmd gcr = {
            .type = GC_IO, // GC IO 타입
            .cmd = NAND_READ, // 읽기 명령
            .stime = 0,
            .xfer_size = spp->pgsz, // 페이지 크기만큼 전송
            .interleave_pci_dma = false,
            .ppa = ppa, // 읽을 주소
        };
        ssd_advance_nand(conv_ftl->ssd, &gcr); // NAND 시뮬레이터에 명령 전달
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
// GC 과정에서 유효 페이지를 새 위치로 쓰는(복사하는) 함수
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct convparams *cpp = &conv_ftl->cp;
    struct ppa new_ppa;
    uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa); // 구 주소의 LPN 확인

    NVMEV_ASSERT(valid_lpn(conv_ftl, lpn)); // LPN 유효성 확인
    new_ppa = get_new_page(conv_ftl, GC_IO); // GC용 새 페이지(Open Block) 할당
    /* update maptbl */
    set_maptbl_ent(conv_ftl, lpn, &new_ppa); // 매핑 테이블을 새 주소로 갱신
    /* update rmap */
    set_rmap_ent(conv_ftl, lpn, &new_ppa); // 역매핑 테이블 갱신

    mark_page_valid(conv_ftl, &new_ppa); // 새 페이지를 유효 상태로 마킹
    /* GC로 복사된 페이지 수 증가 */
    conv_ftl->gc_copied_pages++;

    /* need to advance the write pointer here */
    advance_write_pointer(conv_ftl, GC_IO); // GC 쓰기 포인터 전진

    if (cpp->enable_gc_delay) { // 지연 시뮬레이션
        struct nand_cmd gcw = {
            .type = GC_IO,
            .cmd = NAND_NOP, // 기본은 NOP
            .stime = 0,
            .interleave_pci_dma = false,
            .ppa = &new_ppa,
        };
        if (last_pg_in_wordline(conv_ftl, &new_ppa)) { // 워드라인 끝이면 실제 쓰기 명령 수행
            gcw.cmd = NAND_WRITE;
            gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
        }

        ssd_advance_nand(conv_ftl->ssd, &gcw); // 명령 전달
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(conv_ftl, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;

    new_lun = get_lun(conv_ftl, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

    return 0;
}
/* here ppa identifies the block we want to clean */
// 하나의 블록을 청소(GC)하는 함수 (유효 페이지 읽기/쓰기)
static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    int pg;

    for (pg = 0; pg < spp->pgs_per_blk; pg++) { // 블록 내 페이지 순회
        ppa->g.pg = pg;
        pg_iter = get_pg(conv_ftl->ssd, ppa); // 페이지 가져오기
        /* there shouldn't be any free page in victim blocks */
        NVMEV_ASSERT(pg_iter->status != PG_FREE); // Victim 블록엔 Free 페이지가 없어야 함
        if (pg_iter->status == PG_VALID) { // 유효 페이지라면
            gc_read_page(conv_ftl, ppa); // 읽고
            /* delay the maptbl update until "write" happens */
            gc_write_page(conv_ftl, ppa); // 다른 곳에 씀 (Copy)
            cnt++; // 복사한 페이지 수 카운트
        }
    }

    NVMEV_ASSERT(get_blk(conv_ftl->ssd, ppa)->vpc == cnt); // 복사한 수와 VPC가 일치하는지 검증
}

/* here ppa identifies the block we want to clean */
// 하나의 플래시 페이지 단위로 청소하는 함수
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct convparams *cpp = &conv_ftl->cp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0, i = 0;
    uint64_t completed_time = 0;
    struct ppa ppa_copy = *ppa;

    for (i = 0; i < spp->pgs_per_flashpg; i++) { // 플래시 페이지 내 서브 페이지들 순회
        pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);
        /* there shouldn't be any free page in victim blocks */
        NVMEV_ASSERT(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) // 유효하면 카운트
            cnt++;

        ppa_copy.g.pg++;
    }

    ppa_copy = *ppa;

    if (cnt <= 0) // 유효 페이지 없으면 리턴
        return;

    if (cpp->enable_gc_delay) { // 읽기 지연 시뮬레이션
        struct nand_cmd gcr = {
            .type = GC_IO,
            .cmd = NAND_READ,
            .stime = 0,
            .xfer_size = spp->pgsz * cnt,
            .interleave_pci_dma = false,
            .ppa = &ppa_copy,
        };
        completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
    }

    for (i = 0; i < spp->pgs_per_flashpg; i++) { // 다시 순회하며 쓰기 수행
        pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);

        /* there shouldn't be any free page in victim blocks */
        if (pg_iter->status == PG_VALID) {
            /* delay the maptbl update until "write" happens */
            gc_write_page(conv_ftl, &ppa_copy); // 유효 페이지 복사 해당연산이 코스트에 해당한다고 볼 수 있기 때문에

        }

        ppa_copy.g.pg++;
    }
}

// GC가 끝난 라인을 프리 라인 리스트로 되돌리는 함수
static void mark_line_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    struct line_mgmt *lm = &conv_ftl->lm;
    struct line *line = get_line(conv_ftl, ppa); // 라인 가져오기
    line->ipc = 0; // 무효 카운트 초기화
    line->vpc = 0; // 유효 카운트 초기화
    /* move this line to free line list */
    list_add_tail(&line->entry, &lm->free_line_list); // 프리 라인 리스트 끝에 추가
    lm->free_line_cnt++; // 프리 라인 수 증가
}
static void count_gc_victim_type(struct conv_ftl *conv_ftl, struct line *victim)
{
    u64 check_lpn = INVALID_LPN;
    struct ppa ppa;
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    int i;

    // [1] Full Garbage (유효 페이지 0개) 처리
    // 유효 페이지가 하나도 없다는 건, 쓰자마자 지워진 "초(Ultra) Hot" 데이터일 확률이 높습니다.
    // 카피할 게 없어서 GC가 제일 좋아하는 상황입니다. 이것도 Hot으로 쳐줍니다.
    if (victim->vpc == 0) {
        hot_gc_cnt++;
        total_gc_cnt++;
        return;
    }

    // [2] PPA 구조체 세팅 (탐색용)
    // Line ID가 곧 Block ID입니다.
    ppa.g.ch = 0;   // 첫 번째 채널
    ppa.g.lun = 0;  // 첫 번째 LUN
    ppa.g.pl = 0;   // 첫 번째 플레인
    ppa.g.blk = victim->id; // ★ 핵심: 희생양의 블록 ID
    
    // [3] 블록 내 페이지를 뒤져서 LPN 찾기
    // (보통 Hot/Cold는 스트라이핑되어 저장되므로, 0번 채널/0번 LUN만 봐도 충분합니다)
    for (i = 0; i < spp->pgs_per_blk; i++) {
        ppa.g.pg = i;

        // ★ 기존에 있는 함수 활용 (가장 안전함)
        check_lpn = get_rmap_ent(conv_ftl, &ppa);

        // 유효한 LPN을 찾았다면 스톱!
        if (check_lpn != INVALID_LPN) {
             break; 
        }
    }

    // [4] 예외 처리: 유효 페이지가 있다고 했는데(vpc > 0) 못 찾은 경우
    if (check_lpn == INVALID_LPN) {
        // 이 경우는 다른 채널/LUN에 유효 페이지가 숨어있는 경우입니다.
        // 하지만 통계적으로 무시해도 될 수준이거나, Hot/Cold 데이터는 
        // 보통 모든 채널에 걸쳐 쓰이므로 여기서 못 찾을 확률은 매우 낮습니다.
        return; 
    }

    total_gc_cnt++;

    // [5] 판별 로직
    if (check_lpn < HOT_REGION_LPN_LIMIT) {
        hot_gc_cnt++; // 🔥 Hot 영역
    } else {
        cold_gc_cnt++; // 🧊 Cold 영역
    }
}
// 실제 GC를 수행하는 메인 함수
static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct ppa ppa;
    int flashpg;


    victim_line = conv_ftl->lm.select_victim(conv_ftl, force);
    if (!victim_line) {
        return -1; // 선택 실패 시 리턴
    }
    if(debug_mode){
        count_gc_victim_type(conv_ftl, victim_line);
    }
    conv_ftl->gc_count++;
    ppa.g.blk = victim_line->id;
    ppa.g.blk = victim_line->id; // 선택된 라인 ID를 블록 주소로 설정
    // GC 정보 디버그 출력
    NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
            victim_line->ipc, victim_line->vpc, conv_ftl->lm.victim_line_cnt,
            conv_ftl->lm.full_line_cnt, conv_ftl->lm.free_line_cnt);

    conv_ftl->wfc.credits_to_refill = victim_line->ipc; // 회수된 공간만큼 크레딧 리필량 설정

    /* copy back valid data */
    // 모든 플래시 페이지를 순회하며 유효 데이터 이동
    for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
        int ch, lun;

        ppa.g.pg = flashpg * spp->pgs_per_flashpg;
        for (ch = 0; ch < spp->nchs; ch++) { // 모든 채널 순회
            for (lun = 0; lun < spp->luns_per_ch; lun++) { // 모든 LUN 순회
                struct nand_lun *lunp;

                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(conv_ftl->ssd, &ppa);
                clean_one_flashpg(conv_ftl, &ppa); // 해당 페이지 청소(복사)

                if (flashpg == (spp->flashpgs_per_blk - 1)) { // 마지막 페이지라면 (블록 비우기 완료)
                    struct convparams *cpp = &conv_ftl->cp;

                    mark_block_free(conv_ftl, &ppa); // 블록 상태를 Free로 변경 (메타데이터)

                    if (cpp->enable_gc_delay) { // Erase 지연 시뮬레이션
                        struct nand_cmd gce = {
                            .type = GC_IO,
                            .cmd = NAND_ERASE, // 지우기 명령
                            .stime = 0,
                            .interleave_pci_dma = false,
                            .ppa = &ppa,
                        };
                        ssd_advance_nand(conv_ftl->ssd, &gce);
                    }

                    lunp->gc_endtime = lunp->next_lun_avail_time; // 시간 갱신
                }
            }
        }
    }

    /* update line status */
    mark_line_free(conv_ftl, &ppa); // 라인을 프리 리스트로 복귀

    return 0;
}

// 전경(Foreground) GC 수행 함수 (쓰기 도중 공간 부족 시 호출)
static void foreground_gc(struct conv_ftl *conv_ftl)
{
    if (should_gc_high(conv_ftl)) { // 긴급 임계값 체크
        NVMEV_DEBUG_VERBOSE("should_gc_high passed");
        /* perform GC here until !should_gc(conv_ftl) */
        do_gc(conv_ftl, true); // 강제로 GC 수행
    }
}

// 두 PPA가 동일한 플래시 페이지(물리적 위치)인지 확인하는 함수
static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
    uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

    // 블록 번호와 페이지 번호가 같은지 비교
    return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

// NVMe 읽기 명령 처리 함수
static bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
    struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
    struct conv_ftl *conv_ftl = &conv_ftls[0];
    /* spp are shared by all instances*/
    struct ssdparams *spp = &conv_ftl->ssd->sp;

    struct nvme_command *cmd = req->cmd;
    uint64_t lba = cmd->rw.slba; // 시작 LBA
    uint64_t nr_lba = (cmd->rw.length + 1); // 읽을 섹터 수
    uint64_t start_lpn = lba / spp->secs_per_pg; // 시작 LPN
    uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg; // 끝 LPN
    uint64_t lpn;
    uint64_t nsecs_start = req->nsecs_start; // 시작 시간
    uint64_t nsecs_completed, nsecs_latest = nsecs_start;
    uint32_t xfer_size, i;
    uint32_t nr_parts = ns->nr_parts; // 파티션 수

    struct ppa prev_ppa;
    struct nand_cmd srd = {
        .type = USER_IO,
        .cmd = NAND_READ,
        .stime = nsecs_start,
        .interleave_pci_dma = true,
    };

    NVMEV_ASSERT(conv_ftls);
    NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
    if ((end_lpn / nr_parts) >= spp->tt_pgs) { // 범위 초과 검사
        NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
                start_lpn, spp->tt_pgs);
        return false;
    }

    if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) { // 4KB 이하면 짧은 지연시간 적용
        srd.stime += spp->fw_4kb_rd_lat;
    } else {
        srd.stime += spp->fw_rd_lat;
    }

    for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) { // 파티션 별 처리
        conv_ftl = &conv_ftls[start_lpn % nr_parts];
        xfer_size = 0;
        prev_ppa = get_maptbl_ent(conv_ftl, start_lpn / nr_parts); // 첫 PPA 조회

        /* normal IO read path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) { // LPN 순회
            uint64_t local_lpn;
            struct ppa cur_ppa;

            local_lpn = lpn / nr_parts;
            cur_ppa = get_maptbl_ent(conv_ftl, local_lpn); // 매핑 테이블 조회
            if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) { // 매핑 안됨 or 무효
                NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
                NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
                        cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
                        cur_ppa.g.pl, cur_ppa.g.pg);
                continue;
            }

            // aggregate read io in same flash page
            // 같은 플래시 페이지 내의 읽기 요청이면 묶어서 처리 (최적화)
            if (mapped_ppa(&prev_ppa) &&
                is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
                xfer_size += spp->pgsz; // 전송 크기만 증가
                continue;
            }

            if (xfer_size > 0) { // 이전까지 묶인 요청 처리
                srd.xfer_size = xfer_size;
                srd.ppa = &prev_ppa;
                nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd); // NAND 읽기 시뮬레이션
                nsecs_latest = max(nsecs_completed, nsecs_latest); // 시간 갱신
            }

            xfer_size = spp->pgsz;
            prev_ppa = cur_ppa;
        }

        // issue remaining io
        // 남은 요청 처리
        if (xfer_size > 0) {
            srd.xfer_size = xfer_size;
            srd.ppa = &prev_ppa;
            nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
            nsecs_latest = max(nsecs_completed, nsecs_latest);
        }
    }

    ret->nsecs_target = nsecs_latest; // 완료 시간 설정
    ret->status = NVME_SC_SUCCESS; // 성공 상태 설정
    return true;
}

// NVMe 쓰기 명령 처리 함수
static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
    struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
    struct conv_ftl *conv_ftl = &conv_ftls[0];

    /* wbuf and spp are shared by all instances */
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct buffer *wbuf = conv_ftl->ssd->write_buffer; // 쓰기 버퍼

    struct nvme_command *cmd = req->cmd;
    uint64_t lba = cmd->rw.slba; // 시작 LBA
    uint64_t nr_lba = (cmd->rw.length + 1); // 길이
    uint64_t start_lpn = lba / spp->secs_per_pg; // 시작 LPN
    uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg; // 끝 LPN

    uint64_t lpn;
    uint32_t nr_parts = ns->nr_parts;

    uint64_t nsecs_latest;
    uint64_t nsecs_xfer_completed;
    uint32_t allocated_buf_size;

    struct nand_cmd swr = {
        .type = USER_IO,
        .cmd = NAND_WRITE,
        .interleave_pci_dma = false,
        .xfer_size = spp->pgsz * spp->pgs_per_oneshotpg, // 원샷 크기 설정
    };

    NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
    if ((end_lpn / nr_parts) >= spp->tt_pgs) { // 범위 초과 검사
        NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
                __func__, start_lpn, spp->tt_pgs);
        return false;
    }

    allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba)); // 쓰기 버퍼 할당
    if (allocated_buf_size < LBA_TO_BYTE(nr_lba)) // 버퍼 부족 시
        return false; // 실패

    // 버퍼 쓰기 시뮬레이션 및 시간 계산
    nsecs_latest =
        ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
    nsecs_xfer_completed = nsecs_latest;

    swr.stime = nsecs_latest;

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) { // LPN 순회
        uint64_t local_lpn;
        uint64_t nsecs_completed = 0;
        struct ppa ppa;

        conv_ftl = &conv_ftls[lpn % nr_parts];
        local_lpn = lpn / nr_parts;
        ppa = get_maptbl_ent(
            conv_ftl, local_lpn); // Check whether the given LPN has been written before
                                  // 기존 매핑 확인
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(conv_ftl, &ppa); // 기존 페이지 무효화
            set_rmap_ent(conv_ftl, INVALID_LPN, &ppa); // 역매핑 삭제
            NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(conv_ftl, &ppa));
        }

        /* new write */
        ppa = get_new_page(conv_ftl, USER_IO); // 새 페이지(PPA) 할당 (쓰기 포인터에서)
        /* update maptbl */
        set_maptbl_ent(conv_ftl, local_lpn, &ppa); // 매핑 테이블 갱신
        NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(conv_ftl, &ppa));
        /* update rmap */
        set_rmap_ent(conv_ftl, local_lpn, &ppa); // 역매핑 테이블 갱신
        
        mark_page_valid(conv_ftl, &ppa); // 새 페이지 유효화
   
        /* need to advance the write pointer here */
        advance_write_pointer(conv_ftl, USER_IO); // 쓰기 포인터 전진

        /* Aggregate write io in flash page */
        // 원샷 프로그램 단위가 찼을 때 실제 NAND 쓰기 명령 발생
        if (last_pg_in_wordline(conv_ftl, &ppa)) {
            swr.ppa = &ppa;

            nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr); // NAND 쓰기 시뮬레이션
            nsecs_latest = max(nsecs_completed, nsecs_latest);

            schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
                            spp->pgs_per_oneshotpg * spp->pgsz); // 내부 연산 스케줄링
        }

        consume_write_credit(conv_ftl); // 크레딧 소모
        check_and_refill_write_credit(conv_ftl); // 크레딧 확인 및 필요 시 GC (Foreground GC)
    }

    if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
        /* Wait all flash operations */
        ret->nsecs_target = nsecs_latest; // 모든 플래시 작업 완료 대기 (FUA 설정 시)
    } else {
        /* Early completion */
        ret->nsecs_target = nsecs_xfer_completed; // 버퍼 전송 완료 시점 반환 (빠른 응답)
    }
    ret->status = NVME_SC_SUCCESS; // 성공

    return true;
}

// NVMe Flush 명령 처리 함수
static void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
    uint64_t start, latest;
    uint32_t i;
    struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
    
    start = local_clock(); // 현재 시간
    latest = start;
    for (i = 0; i < ns->nr_parts; i++) { // 모든 인스턴스 확인
        latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd)); // SSD가 유휴 상태가 되는 시간 계산
    }
    
    NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);
    if (debug_mode) {
		uint64_t total_gc = 0;
		uint64_t total_copied = 0;
		
		for (i = 0; i < ns->nr_parts; i++) {
			total_gc += conv_ftls[i].gc_count;
			total_copied += conv_ftls[i].gc_copied_pages;
		}
		
		printk(KERN_INFO "NVMeVirt: [FLUSH - Final GC Stats]\n");
		printk(KERN_INFO "NVMeVirt:  Total GC Count: %llu\n", total_gc);
		printk(KERN_INFO "NVMeVirt:  Total Copied Pages: %llu\n", total_copied);
		printk(KERN_INFO "NVMeVirt:  Avg Pages per GC: %llu\n", 
		       total_gc > 0 ? total_copied / total_gc : 0);
        if (total_gc_cnt > 0) {
            printk(KERN_INFO "NVMeVirt: [Hot/Cold Analysis]\n");
            printk(KERN_INFO "NVMeVirt:  Total Sampled GC: %lu\n", total_gc_cnt);
            printk(KERN_INFO "NVMeVirt:  🔥 Hot Victims : %lu\n", hot_gc_cnt);
            printk(KERN_INFO "NVMeVirt:  🧊 Cold Victims: %lu\n", cold_gc_cnt);
            printk(KERN_INFO "NVMeVirt:  🧊 Cold Ratio  : %lu%%\n", (cold_gc_cnt * 100) / total_gc_cnt);
            printk(KERN_INFO "NVMeVirt:  Average Age  : %llu%%\n", victim_total_age / victim_chosen_cnt);
        } else {
            printk(KERN_INFO "NVMeVirt: [Hot/Cold Analysis] No GC triggered yet.\n");
        }
	}
    ret->status = NVME_SC_SUCCESS;
    ret->nsecs_target = latest; // 완료 목표 시간 설정
    return;
}

// IO 명령 처리 메인 진입점 (디스패처)
bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
    struct nvme_command *cmd = req->cmd; // NVMe 명령

    NVMEV_ASSERT(ns->csi == NVME_CSI_NVM); // NVM 커맨드셋 확인

    switch (cmd->common.opcode) { // 오퍼코드 확인
    case nvme_cmd_write:
        if (!conv_write(ns, req, ret)) // 쓰기 함수 호출
            return false;
        break;
    case nvme_cmd_read:
        if (!conv_read(ns, req, ret)) // 읽기 함수 호출
            return false;
        break;
    case nvme_cmd_flush:
        conv_flush(ns, req, ret); // 플러시 함수 호출
        break;
    default: // 구현되지 않은 명령
        NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
                nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
        break;
    }

    return true; // 성공 리턴
}
