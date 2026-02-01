// SPDX-License-Identifier: GPL-2.0-only

#ifndef _LIB_NVMEV_H
#define _LIB_NVMEV_H

#include <linux/pci.h>
#include <linux/msi.h>
#include <asm/apic.h>

#include "nvme.h" // NVMe 프로토콜 표준 정의 헤더

/* ======================================================== */
/* 컴파일 타임 설정 (Configuration)                        */
/* ======================================================== */

// IO 작업을 처리할 워커 스레드를 SQ(Submission Queue) 단위로 할당할지 여부
#define CONFIG_NVMEV_IO_WORKER_BY_SQ

// x86 인터럽트 처리를 빠르게 할지 여부 (현재 비활성화 - 안정성 위함인 듯)
#undef CONFIG_NVMEV_FAST_X86_IRQ_HANDLING

// 디버깅 및 상세 로그 출력 설정 (기본적으로 비활성화)
#undef CONFIG_NVMEV_VERBOSE
#undef CONFIG_NVMEV_DEBUG
#undef CONFIG_NVMEV_DEBUG_VERBOSE

/*
 * 유휴 시간(Idle Timeout) 설정
 * 설정된 시간(초) 동안 IO가 없으면 CPU 전력 소모를 줄이기 위해
 * 드라이버가 절전 모드(sleep)로 진입합니다.
 * - 기본값: 60초 (테스트에 영향을 주지 않도록 보수적으로 잡음)
 * - 주의: 절전 모드에서 깨어날 때 약간의 지연(Latency Penalty) 발생 가능
 */
#define CONFIG_NVMEVIRT_IDLE_TIMEOUT 60

/* ======================================================== */
/* [추가됨] 가비지 컬렉션(GC) 정책 모드 설정                */
/* FTL(conv_ftl.c)에서 이 값을 참조하여 희생 블록 선정 알고리즘을 결정함 */
/* ======================================================== */
#define GC_MODE_GREEDY       0  /* Greedy 정책: 유효 페이지(VPC)가 가장 적은 블록 선택 */
#define GC_MODE_COST_BENEFIT 1  /* Cost-Benefit 정책: (Age * IPC) / VPC 점수 기반 선택 */
#define GC_MODE_RANDOM       2  /* Random 정책: 전체 블록 중 무작위 선택 */


/* ★ 이 값을 변경하여 FTL의 GC 동작 방식을 결정합니다! ★ */
#define CURRENT_GC_MODE      GC_MODE_GREEDY 

/* ======================================================== */
/* 드라이버 및 디바이스 정보 정의                          */
/* ======================================================== */
#define NVMEV_DRV_NAME "NVMeVirt"      // 드라이버 이름
#define NVMEV_VERSION 0x0110           // 버전 1.1.0
#define NVMEV_DEVICE_ID NVMEV_VERSION
#define NVMEV_VENDOR_ID 0x0c51         // 가상 벤더 ID
#define NVMEV_SUBSYSTEM_ID  0x370d
#define NVMEV_SUBSYSTEM_VENDOR_ID NVMEV_VENDOR_ID

/* 커널 로그 출력 매크로 (printk 래퍼) */
#define NVMEV_INFO(string, args...) printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ERROR(string, args...) printk(KERN_ERR "%s: " string, NVMEV_DRV_NAME, ##args)
// 조건이 거짓이면 커널 패닉을 일으키는 매크로 (디버깅용)
#define NVMEV_ASSERT(x) BUG_ON((!(x)))

/* 디버그 모드일 때만 로그 출력하도록 설정 */
#ifdef CONFIG_NVMEV_DEBUG
#define  NVMEV_DEBUG(string, args...) printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#ifdef CONFIG_NVMEV_DEBUG_VERBOSE
#define  NVMEV_DEBUG_VERBOSE(string, args...) printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#else
#define  NVMEV_DEBUG_VERBOSE(string, args...)
#endif
#else
// 디버그 모드가 아니면 아무 코드도 생성하지 않음 (오버헤드 제거)
#define NVMEV_DEBUG(string, args...)
#define NVMEV_DEBUG_VERBOSE(string, args...)
#endif

/* ======================================================== */
/* 상수 및 단위 변환 매크로                                */
/* ======================================================== */
#define NR_MAX_IO_QUEUE 72          // 최대 지원 IO 큐 개수
#define NR_MAX_PARALLEL_IO 16384    // 동시에 처리 가능한 최대 IO 개수

#define NVMEV_INTX_IRQ 15           // 레거시 인터럽트 번호

// 페이지 오프셋 계산용 마스크
#define PAGE_OFFSET_MASK (PAGE_SIZE - 1)
// 물리 주소(Physical Address)를 페이지 프레임 번호(PFN)로 변환
#define PRP_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))

// 용량 단위 변환 매크로 (비트 시프트 연산으로 효율화)
#define KB(k) ((k) << 10)
#define MB(m) ((m) << 20)
#define GB(g) ((g) << 30)

#define BYTE_TO_KB(b) ((b) >> 10)
#define BYTE_TO_MB(b) ((b) >> 20)
#define BYTE_TO_GB(b) ((b) >> 30)

// 시간 단위 변환 매크로 (초 -> 밀리초 -> 마이크로초 -> 나노초)
#define MS_PER_SEC(s) ((s)*1000)
#define US_PER_SEC(s) (MS_PER_SEC(s) * 1000)
#define NS_PER_SEC(s) (US_PER_SEC(s) * 1000)

// LBA(섹터) <-> Byte 변환 매크로
#define LBA_TO_BYTE(lba) ((lba) << LBA_BITS)
#define BYTE_TO_LBA(byte) ((byte) >> LBA_BITS)

#define BITMASK32_ALL (0xFFFFFFFF)
#define BITMASK64_ALL (0xFFFFFFFFFFFFFFFF)
#define ASSERT(X)

#include "ssd_config.h" // SSD 내부 파라미터(채널, 웨이 등) 정의

/* ======================================================== */
/* NVMe 큐(Queue) 관련 구조체                              */
/* ======================================================== */

/**
 * @brief Submission Queue(SQ) 통계 정보
 * IO 처리량, 현재 비행 중(In-flight)인 요청 수 등을 추적
 */
struct nvmev_sq_stat {
    unsigned int nr_dispatched;      // 처리 완료된 총 개수
    unsigned int nr_dispatch;        // 현재 처리(Dispatch) 시도 횟수
    unsigned int nr_in_flight;       // 현재 처리 중인 IO 개수
    unsigned int max_nr_in_flight;   // 최대 동시 처리 기록
    unsigned long long total_io;     // 누적 IO 카운트
};

/**
 * @brief NVMe Submission Queue (명령 제출 큐)
 * 호스트가 디바이스에 명령을 보낼 때 사용하는 큐
 */
struct nvmev_submission_queue {
    int qid;            // 큐 ID
    int cqid;           // 연결된 Completion Queue ID
    int priority;       // 우선순위
    bool phys_contig;   // 물리적으로 연속된 메모리인지 여부

    int queue_size;     // 큐 크기 (Entry 개수)

    struct nvmev_sq_stat stat; // 통계 정보

    // 실제 명령이 저장되는 메모리 영역 (Host RAM 매핑)
    struct nvme_command __iomem **sq;
    void *mapped;       // 커널 가상 주소 매핑 포인터
};

/**
 * @brief NVMe Completion Queue (완료 큐)
 * 디바이스가 작업 결과를 호스트에 알릴 때 사용하는 큐
 */
struct nvmev_completion_queue {
    int qid;            // 큐 ID
    int irq_vector;     // 할당된 인터럽트 벡터 번호 (MSI-X)
    bool irq_enabled;   // 인터럽트 활성화 여부
    bool interrupt_ready; // 인터럽트 발생 가능 상태
    bool phys_contig;   // 물리적 연속성 여부

    spinlock_t entry_lock; // 동시 접근 제어를 위한 스핀락
    struct mutex irq_lock; // 인터럽트 관련 락

    int queue_size;

    int phase;          // Phase Bit (완료 여부 확인용 토글 비트)
    int cq_head;        // 큐의 Head (호스트가 읽은 위치)
    int cq_tail;        // 큐의 Tail (디바이스가 쓴 위치)

    struct nvme_completion __iomem **cq; // 실제 완료 메시지 저장소
    void *mapped;
};

/**
 * @brief Admin Queue (관리 큐)
 * IO가 아닌 관리 명령(큐 생성/삭제, 장치 식별 등) 처리용
 */
struct nvmev_admin_queue {
    int phase;

    int sq_depth; // SQ 깊이
    int cq_depth; // CQ 깊이

    int cq_head;

    struct nvme_command __iomem **nvme_sq;     // Admin SQ
    struct nvme_completion __iomem **nvme_cq;  // Admin CQ
};

// 페이지당 큐 엔트리 개수 계산 매크로
#define NR_SQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_command))
#define NR_CQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_completion))

// 특정 엔트리 ID가 몇 번째 페이지, 몇 번째 오프셋에 있는지 계산
#define SQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_CQE_PER_PAGE)

#define SQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_CQE_PER_PAGE)

/* ======================================================== */
/* 장치 설정 및 작업자(Worker) 구조체                      */
/* ======================================================== */

/**
 * @brief 가상 NVMe 장치의 하드웨어/성능 설정값
 * DRAM 시뮬레이션을 위한 주소 범위 및 타이밍 파라미터 정의
 */
struct nvmev_config {
    unsigned long memmap_start; // 예약된 물리 메모리 시작 주소 (바이트)
    unsigned long memmap_size;  // 예약된 메모리 크기

    unsigned long storage_start; // 가상 스토리지 시작 주소
    unsigned long storage_size;  // 가상 스토리지 크기

    unsigned int cpu_nr_dispatcher; // 디스패처가 실행될 CPU 코어
    unsigned int nr_io_workers;     // IO 워커 스레드 개수
    unsigned int cpu_nr_io_workers[32]; // 각 워커가 바인딩될 CPU 코어

    /* IO Unit 및 성능 지연(Latency) 설정 */
    unsigned int nr_io_units;
    unsigned int io_unit_shift; // 2의 승수

    // 읽기/쓰기 성능 시뮬레이션 파라미터 (나노초 단위)
    unsigned int read_delay;    // 읽기 준비 지연
    unsigned int read_time;     // 실제 읽기 소요 시간
    unsigned int read_trailing; // 읽기 후처리 시간
    unsigned int write_delay;   // 쓰기 준비 지연
    unsigned int write_time;    // 실제 쓰기 소요 시간 (tPROG 등)
    unsigned int write_trailing;// 쓰기 후처리 시간
};

/**
 * @brief 개별 IO 작업(Job) 상태 구조체
 * 하나의 NVMe 명령이 처리되는 동안의 생애 주기와 시간 정보를 담음
 */
struct nvmev_io_work {
    int sqid;       // 요청이 들어온 SQ ID
    int cqid;       // 응답을 보낼 CQ ID

    int sq_entry;   // SQ 내 인덱스
    unsigned int command_id; // NVMe Command ID (CID)

    // 시간 측정 및 지연 시뮬레이션용 타임스탬프
    unsigned long long nsecs_start;      // 시작 시간
    unsigned long long nsecs_target;     // 완료 목표 시간
    unsigned long long nsecs_enqueue;    // 큐 진입 시간
    unsigned long long nsecs_copy_start; // 데이터 복사 시작
    unsigned long long nsecs_copy_done;  // 데이터 복사 완료
    unsigned long long nsecs_cq_filled;  // CQ 기록 시간

    bool is_copied;      // 데이터 복사 완료 여부
    bool is_completed;   // 전체 명령 완료 여부

    unsigned int status; // NVMe 상태 코드 (성공/실패)
    unsigned int result0; // 완료 결과 값 0
    unsigned int result1; // 완료 결과 값 1

    bool is_internal;    // 내부 생성 명령인지 여부
    void *write_buffer;  // 쓰기 버퍼 포인터
    size_t buffs_to_release; // 해제할 버퍼 크기

    unsigned int next, prev; // 연결 리스트 링크 (작업 큐 관리용)
};

/**
 * @brief IO 워커 스레드 구조체
 * 실제 IO 요청을 처리하는 커널 스레드 정보
 */
struct nvmev_io_worker {
    struct nvmev_io_work *work_queue; // 이 워커가 처리할 작업 큐

    // 작업 큐 관리를 위한 인덱스들 (Ring Buffer 형태)
    unsigned int free_seq;      /* 빈 슬롯 헤드 */
    unsigned int free_seq_end;  /* 빈 슬롯 테일 */
    unsigned int io_seq;        /* 처리 대기 중인 IO 헤드 */
    unsigned int io_seq_end;    /* 처리 대기 중인 IO 테일 */

    unsigned long long latest_nsecs; // 마지막 작업 시간

    unsigned int id;                // 워커 ID
    struct task_struct *task_struct; // 커널 스레드 구조체 포인터
    char thread_name[32];           // 스레드 이름 (top 명령 등에 표시됨)
};

/* ======================================================== */
/* 메인 디바이스 구조체                                    */
/* ======================================================== */

/**
 * @brief NVMeVirt 가상 장치 전체를 총괄하는 구조체
 * 가상 PCIe 버스, BAR 레지스터, 큐, 워커 등을 모두 포함
 */
struct nvmev_dev {
    // 가상 PCI 버스 및 장치 정보
    struct pci_bus *virt_bus;
    void *virtDev;
    struct pci_header *pcihdr;
    struct pci_pm_cap *pmcap;   // 전원 관리 기능
    struct pci_msix_cap *msixcap; // MSI-X 인터럽트 기능
    struct pcie_cap *pciecap;
    struct pci_ext_cap *extcap;

    struct pci_dev *pdev; // 리눅스 커널의 PCI 장치 구조체

    struct nvmev_config config; // 장치 설정 정보
    struct task_struct *nvmev_dispatcher; // IO 분배자 스레드

    void *storage_mapped; // 실제 스토리지 메모리 매핑

    struct nvmev_io_worker *io_workers; // 워커 스레드 배열
    unsigned int io_worker_turn;        // 라운드 로빈 분배용 인덱스

    void __iomem *msix_table; // MSI-X 테이블 (인터럽트 벡터)

    bool intx_disabled;

    // BAR(Base Address Register) 관련: 호스트가 장치 레지스터에 접근하는 통로
    struct __nvme_bar *old_bar;
    struct nvme_ctrl_regs __iomem *bar; // NVMe 컨트롤러 레지스터

    // 도어벨(Doorbell) 레지스터: 호스트가 큐에 뭔가 넣었다고 알리는 벨
    u32 *old_dbs;
    u32 __iomem *dbs;

    // 네임스페이스(NS) 및 큐 관리
    struct nvmev_ns *ns;
    unsigned int nr_ns;
    unsigned int nr_sq;
    unsigned int nr_cq;

    // 큐 포인터 배열
    struct nvmev_admin_queue *admin_q;
    struct nvmev_submission_queue *sqes[NR_MAX_IO_QUEUE + 1];
    struct nvmev_completion_queue *cqes[NR_MAX_IO_QUEUE + 1];

    unsigned int mdts; // 최대 데이터 전송 크기 (Max Data Transfer Size)

    // Procfs (디버깅/정보 확인용 파일 시스템) 엔트리들
    struct proc_dir_entry *proc_root;
    struct proc_dir_entry *proc_read_times;
    struct proc_dir_entry *proc_write_times;
    struct proc_dir_entry *proc_io_units;
    struct proc_dir_entry *proc_stat;
    struct proc_dir_entry *proc_debug;

    unsigned long long *io_unit_stat;
};

/* ======================================================== */
/* FTL 인터페이스 및 함수 원형                             */
/* ======================================================== */

// 내부 IO 요청 전달용 구조체
struct nvmev_request {
    struct nvme_command *cmd; // NVMe 명령
    uint32_t sq_id;           // SQ ID
    uint64_t nsecs_start;     // 시작 시간
};

// IO 처리 결과 반환용 구조체
struct nvmev_result {
    uint32_t status;          // 성공/실패 상태
    uint64_t nsecs_target;    // 시뮬레이션 된 완료 시간
};

/**
 * @brief NVMe 네임스페이스 (논리적 저장 공간) 구조체
 * 실제 FTL(Flash Translation Layer) 로직이 여기에 연결됨
 */
struct nvmev_ns {
    uint32_t id;      // NSID (보통 1)
    uint32_t csi;     // Command Set Identifier
    uint64_t size;    // 네임스페이스 크기 (바이트)
    void *mapped;     // 매핑된 주소

    /* 파티션 및 FTL 인스턴스 정보 */
    uint32_t nr_parts; // 병렬 처리를 위한 파티션 수
    void *ftls;        // FTL 인스턴스 배열 (conv_ftl 등)

    /* [중요] IO 처리 함수 포인터 (여기에 conv_proc_nvme_io_cmd 등이 연결됨) */
    bool (*proc_io_cmd)(struct nvmev_ns *ns, struct nvmev_request *req,
                struct nvmev_result *ret);

    /* 특정 명령어 셋(CSS) 식별 및 처리 함수 */
    bool (*identify_io_cmd)(struct nvmev_ns *ns, struct nvme_command cmd);
    unsigned int (*perform_io_cmd)(struct nvmev_ns *ns, struct nvme_command *cmd,
                       uint32_t *status);
};

/* 함수 원형 선언들 (extern) */

// 가상 장치 초기화 및 종료
extern struct nvmev_dev *nvmev_vdev;
struct nvmev_dev *VDEV_INIT(void);
void VDEV_FINALIZE(struct nvmev_dev *nvmev_vdev);

// PCI 및 인터럽트 관련
bool nvmev_proc_bars(void);
bool NVMEV_PCI_INIT(struct nvmev_dev *dev);
void nvmev_signal_irq(int msi_index);

// Admin Queue 처리
void nvmev_proc_admin_sq(int new_db, int old_db);
void nvmev_proc_admin_cq(int new_db, int old_db);

// IO Queue 처리 및 워커 관리
struct buffer;
void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
                struct buffer *write_buffer, size_t buffs_to_release);
void NVMEV_IO_WORKER_INIT(struct nvmev_dev *nvmev_vdev);
void NVMEV_IO_WORKER_FINAL(struct nvmev_dev *nvmev_vdev);
int nvmev_proc_io_sq(int qid, int new_db, int old_db);
void nvmev_proc_io_cq(int qid, int new_db, int old_db);

#endif /* _LIB_NVMEV_H */