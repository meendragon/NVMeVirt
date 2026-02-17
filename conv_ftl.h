// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H // 헤더 파일 중복 포함 방지 가드

#include <linux/types.h>    // 리눅스 커널 기본 데이터 타입 정의
#include "pqueue/pqueue.h"  // 우선순위 큐 라이브러리 (GC 희생 블록 선정용)
#include "ssd_config.h"     // SSD 설정 관련 헤더
#include "ssd.h"            // SSD 기본 구조체 및 함수 헤더

struct conv_ftl;
typedef struct line *(*victim_select_fn)(struct conv_ftl *, bool);
// FTL 동작을 제어하는 파라미터 구조체
struct convparams {
    uint32_t gc_thres_lines;      // GC를 시작할 프리 라인 개수 임계값 (이보다 적으면 GC 시작)
    uint32_t gc_thres_lines_high; // 긴급 GC(Foreground GC)를 수행할 임계값
    bool enable_gc_delay;         // GC 수행 시 발생하는 지연(Latency) 시뮬레이션 활성화 여부

    double op_area_pcent;         // 오버 프로비저닝(Over-Provisioning) 영역 비율
    int pba_pcent; /* (physical space / logical space) * 100*/ // 물리 공간 대비 논리 공간의 비율 (OP 포함)
};

// 하나의 '라인(Line)' 즉, 슈퍼블록(Superblock)을 관리하는 구조체
struct line {
    int id; /* line id, the same as corresponding block id */ // 라인 ID (물리적 블록 ID와 동일)
    int ipc; /* invalid page count in this line */          // 이 라인에 있는 무효 페이지(Invalid Page) 개수
    int vpc; /* valid page count in this line */            // 이 라인에 있는 유효 페이지(Valid Page) 개수
    struct list_head entry;                                 // 프리/풀 리스트 연결을 위한 리스트 헤드
    /* position in the priority queue for victim lines */
    size_t pos;                                             // 희생 라인 우선순위 큐 내부에서의 위치 인덱스
    uint64_t last_modified_time;                            // update된 즉 Invalid된 수정 시각을 기록해야함
};

/* wp: record next write addr */                
// 다음 쓰기 작업을 수행할 물리적 위치를 가리키는 포인터 구조체
struct write_pointer {
    struct line *curline; // 현재 쓰기를 수행 중인 라인(슈퍼블록) 포인터
    uint32_t ch;          // 현재 쓰기 채널 번호
    uint32_t lun;         // 현재 쓰기 LUN 번호
    uint32_t pg;          // 현재 쓰기 페이지 번호
    uint32_t blk;         // 현재 쓰기 블록 번호
    uint32_t pl;          // 현재 쓰기 플레인 번호
};

// 전체 라인들의 상태(Free, Victim, Full)를 관리하는 구조체
struct line_mgmt {
    struct line *lines; // 모든 라인 정보를 담고 있는 배열
    
    /* free line list, we only need to maintain a list of blk numbers */
    struct list_head free_line_list; // 빈 라인(Free Line)들을 관리하는 리스트
    pqueue_t *victim_line_pq;        // 데이터가 차 있고 GC 대상이 될 라인들을 관리하는 우선순위 큐
    victim_select_fn select_victim; //함수포인터로 init_lines에서 결정된 전략 함수(Greedy/Random/CB)가 들어감
    struct list_head full_line_list; // 완전히 꽉 찬(유효 페이지로만 구성된) 라인 리스트

    uint32_t tt_lines;        // 전체 라인(블록)의 총 개수
    uint32_t free_line_cnt;   // 현재 남아있는 프리 라인 개수
    uint32_t victim_line_cnt; // 현재 희생 후보군에 있는 라인 개수
    uint32_t full_line_cnt;   // 완전히 꽉 찬 라인 개수
};

// 쓰기 흐름 제어(Flow Control)를 위한 구조체 (과도한 쓰기 방지 및 GC 유도)
struct write_flow_control {
    uint32_t write_credits;     // 현재 쓰기 가능한 크레딧 (쓰기 시 감소)
    uint32_t credits_to_refill; // GC 수행 완료 후ㅋ₩ 리필할 크레딧 양
};

// Conventional FTL의 메인 구조체
struct conv_ftl {
    struct ssd *ssd; // 하부 SSD 하드웨어 모델에 대한 포인터

    struct convparams cp;       // FTL 파라미터 설정값
    struct ppa *maptbl; /* page level mapping table */         // 논리 주소(LPN) -> 물리 주소(PPA) 매핑 테이블
    uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */ // 물리 주소 -> 논리 주소 역매핑 테이블 (GC시 사용, OOB 영역 가정)
    struct write_pointer wp;    // 사용자 데이터 쓰기를 위한 포인터
    struct write_pointer slc_wp;
    struct write_pointer tlc_wp;
    struct write_pointer gc_wp; // GC 데이터(유효 페이지 이동) 쓰기를 위한 포인터
    struct write_pointer migration_wp;
    struct line_mgmt lm;        // 라인(블록) 관리자
    struct line_mgmt slc_lm;
    struct line_mgmt tlc_lm;
    struct write_flow_control wfc; // 쓰기 유량 제어기

    uint64_t gc_count;              // 총 GC 수행 횟수
    uint64_t gc_copied_pages;       // GC로 복사된 총 페이지 수

    bool slc_enabled;
    u32 slc_line_limit;
};

// 네임스페이스 초기화 및 FTL 인스턴스 생성 함수 선언
void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
             uint32_t cpu_nr_dispatcher);

// 네임스페이스 제거 및 FTL 리소스 해제 함수 선언
void conv_remove_namespace(struct nvmev_ns *ns);

// NVMe IO 명령(Read/Write/Flush)을 처리하는 메인 함수 선언
bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
               struct nvmev_result *ret);

#endif