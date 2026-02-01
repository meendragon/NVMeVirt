// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"

// 현재 CPU의 시계(Clock)를 가져오는 헬퍼 함수
// 시뮬레이션의 기준 시간이 됩니다.
static inline uint64_t __get_ioclock(struct ssd *ssd)
{
    return cpu_clock(ssd->cpu_nr_dispatcher);
}

// ========================================================
// 1. 쓰기 버퍼(Write Buffer) 관리 함수들
// --------------------------------------------------------
// SSD 내부의 DRAM 버퍼를 시뮬레이 션합니다.
// 동시성 제어를 위해 스핀락(Spinlock)을 사용합니다.
// ========================================================

void buffer_init(struct buffer *buf, size_t size)
{
    spin_lock_init(&buf->lock); // 락 초기화
    buf->size = size;           // 전체 버퍼 크기 설정
    buf->remaining = size;      // 남은 공간 초기화
}

// 버퍼 공간 할당 (공간이 날 때까지 대기 - Busy Wait)
uint32_t buffer_allocate(struct buffer *buf, size_t size)
{
    NVMEV_ASSERT(size <= buf->size); // 요청 크기가 전체 버퍼보다 크면 안 됨

    // 락 획득 시도 (Spinlock)
    while (!spin_trylock(&buf->lock)) {
        cpu_relax(); // CPU 파이프라인 최적화 (Busy Wait 루프)
    }

    // 공간 부족 시 할당 실패 (0 반환)
    if (buf->remaining < size) {
        size = 0;
    }

    // 공간 차감
    buf->remaining -= size;

    spin_unlock(&buf->lock);
    return size;
}

// 버퍼 공간 반환 (해제)
bool buffer_release(struct buffer *buf, size_t size)
{
    while (!spin_trylock(&buf->lock))
        ; // 락 획득 대기
    buf->remaining += size; // 공간 복구
    spin_unlock(&buf->lock);

    return true;
}

// 버퍼 전체 초기화 (Refill)
void buffer_refill(struct buffer *buf)
{
    while (!spin_trylock(&buf->lock))
        ;
    buf->remaining = buf->size; // 전체 공간 복구
    spin_unlock(&buf->lock);
}

static void check_params(struct ssdparams *spp)
{
    /* 예전에는 2의 승수여야 한다는 제약이 있었으나,
     * 현재 일반적인 포인터 증가 방식을 사용하여 제약이 사라짐 */
    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

// ========================================================
// 2. SSD 파라미터 초기화 (Geometry & Latency Setup)
// --------------------------------------------------------
// ssd_config.h의 매크로 값들을 바탕으로
// 전체 용량, 블록 수, 페이지 수 등을 정밀하게 계산합니다.
// ========================================================
void ssd_init_params(struct ssdparams *spp, uint64_t capacity, uint32_t nparts)
{
    uint64_t blk_size, total_size;

    // 섹터 및 페이지 크기 설정
    spp->secsz = LBA_SIZE;              // 보통 512B
    spp->secs_per_pg = 4096 / LBA_SIZE; // 4KB 페이지 기준 섹터 수 (보통 8개)
    spp->pgsz = spp->secsz * spp->secs_per_pg; // 페이지 크기 (4KB)

    // 채널 및 낸드 구조 설정
    spp->nchs = NAND_CHANNELS;
    spp->pls_per_lun = PLNS_PER_LUN;
    spp->luns_per_ch = LUNS_PER_NAND_CH;
    spp->cell_mode = CELL_MODE; // SLC/MLC/TLC 등

    /* 파티셔닝: 여러 개의 FTL 인스턴스가 병렬로 돌아가도록 채널을 나눔 */
    NVMEV_ASSERT((spp->nchs % nparts) == 0);
    spp->nchs /= nparts;    // 파티션 당 채널 수
    capacity /= nparts;     // 파티션 당 용량

    // 블록 크기 및 개수 계산
    if (BLKS_PER_PLN > 0) {
        // 블록 개수가 명시된 경우
        spp->blks_per_pl = BLKS_PER_PLN;
        // 용량을 역산하여 블록 크기 추정
        blk_size = DIV_ROUND_UP(capacity, spp->blks_per_pl * spp->pls_per_lun *
                              spp->luns_per_ch * spp->nchs);
    } else {
        // 블록 크기가 명시된 경우 (보통 ZNS 등)
        NVMEV_ASSERT(BLK_SIZE > 0);
        blk_size = BLK_SIZE;
        // 용량을 기반으로 블록 개수 계산
        spp->blks_per_pl = DIV_ROUND_UP(capacity, blk_size * spp->pls_per_lun *
                                  spp->luns_per_ch * spp->nchs);
    }

    // 원샷(One-shot) 프로그램 및 플래시 페이지 크기 검증
    NVMEV_ASSERT((ONESHOT_PAGE_SIZE % spp->pgsz) == 0 && (FLASH_PAGE_SIZE % spp->pgsz) == 0);
    NVMEV_ASSERT((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);

    // 내부 관리 단위 계산
    spp->pgs_per_oneshotpg = ONESHOT_PAGE_SIZE / (spp->pgsz); // 원샷 당 4KB 페이지 수
    spp->oneshotpgs_per_blk = DIV_ROUND_UP(blk_size, ONESHOT_PAGE_SIZE);

    spp->pgs_per_flashpg = FLASH_PAGE_SIZE / (spp->pgsz);
    spp->flashpgs_per_blk = (ONESHOT_PAGE_SIZE / FLASH_PAGE_SIZE) * spp->oneshotpgs_per_blk;

    spp->pgs_per_blk = spp->pgs_per_oneshotpg * spp->oneshotpgs_per_blk; // 블록 당 총 페이지 수

    spp->write_unit_size = WRITE_UNIT_SIZE;

    // ==========================================
    // 지연 시간(Latency) 및 성능 파라미터 설정
    // ==========================================
    // 낸드 플래시 동작 시간 (Read/Program/Erase)
    spp->pg_4kb_rd_lat[CELL_TYPE_LSB] = NAND_4KB_READ_LATENCY_LSB;
    spp->pg_4kb_rd_lat[CELL_TYPE_MSB] = NAND_4KB_READ_LATENCY_MSB;
    spp->pg_4kb_rd_lat[CELL_TYPE_CSB] = NAND_4KB_READ_LATENCY_CSB;
    
    spp->pg_rd_lat[CELL_TYPE_LSB] = NAND_READ_LATENCY_LSB;
    spp->pg_rd_lat[CELL_TYPE_MSB] = NAND_READ_LATENCY_MSB;
    spp->pg_rd_lat[CELL_TYPE_CSB] = NAND_READ_LATENCY_CSB;
    
    spp->pg_wr_lat = NAND_PROG_LATENCY; // 쓰기 시간 (tPROG)
    spp->blk_er_lat = NAND_ERASE_LATENCY; // 지우기 시간 (tBERS)
    spp->max_ch_xfer_size = MAX_CH_XFER_SIZE;

    // 펌웨어(F/W) 오버헤드 시뮬레이션 값
    spp->fw_4kb_rd_lat = FW_4KB_READ_LATENCY;
    spp->fw_rd_lat = FW_READ_LATENCY;
    spp->fw_ch_xfer_lat = FW_CH_XFER_LATENCY;
    spp->fw_wbuf_lat0 = FW_WBUF_LATENCY0;
    spp->fw_wbuf_lat1 = FW_WBUF_LATENCY1;

    // 대역폭 설정
    spp->ch_bandwidth = NAND_CHANNEL_BANDWIDTH;
    spp->pcie_bandwidth = PCIE_BANDWIDTH;

    spp->write_buffer_size = GLOBAL_WB_SIZE;
    spp->write_early_completion = WRITE_EARLY_COMPLETION; // 버퍼에만 쓰면 완료로 칠지 여부

    /* 주소 변환을 위한 총계 계산 (Total Counts) */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs; // 전체 섹터 수

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs; // 전체 페이지 수

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs; // 전체 블록 수

    spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs; // 전체 LUN 수

    /* 슈퍼블록(Line)은 모든 채널/LUN을 묶은 단위 */
    spp->blks_per_line = spp->tt_luns; 
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; // 라인 개수 = LUN당 블록 수

    check_params(spp);

    // 최종 설정된 정보 로그 출력
    total_size = (unsigned long)spp->tt_luns * spp->blks_per_lun * spp->pgs_per_blk *
             spp->secsz * spp->secs_per_pg;
    blk_size = spp->pgs_per_blk * spp->secsz * spp->secs_per_pg;
    NVMEV_INFO(
        "Total Capacity(GiB,MiB)=%llu,%llu chs=%u luns=%lu lines=%lu blk-size(MiB,KiB)=%u,%u line-size(MiB,KiB)=%lu,%lu",
        BYTE_TO_GB(total_size), BYTE_TO_MB(total_size), spp->nchs, spp->tt_luns,
        spp->tt_lines, BYTE_TO_MB(spp->pgs_per_blk * spp->pgsz),
        BYTE_TO_KB(spp->pgs_per_blk * spp->pgsz), BYTE_TO_MB(spp->pgs_per_line * spp->pgsz),
        BYTE_TO_KB(spp->pgs_per_line * spp->pgsz));
}

// ========================================================
// 3. SSD 계층 구조 초기화 (Hierarchy Init)
// --------------------------------------------------------
// Page -> Block -> Plane -> LUN -> Channel 순서로
// 메모리를 할당하고 구조체를 초기화합니다.
// ========================================================

// 낸드 페이지 초기화
static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    int i;
    pg->nsecs = spp->secs_per_pg;
    // 섹터 상태 배열 할당
    pg->sec = kmalloc(sizeof(nand_sec_status_t) * pg->nsecs, GFP_KERNEL);
    for (i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE; // 초기 상태: Free
}

static void ssd_remove_nand_page(struct nand_page *pg)
{
    kfree(pg->sec);
}

// 낸드 블록 초기화
static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    int i;
    blk->npgs = spp->pgs_per_blk;
    // 페이지 배열 할당
    blk->pg = kmalloc(sizeof(struct nand_page) * blk->npgs, GFP_KERNEL);
    for (i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0; // Invalid Page Count
    blk->vpc = 0; // Valid Page Count
    blk->erase_cnt = 0;
    blk->wp = 0; // Write Pointer (Sequential Write 가정)
}

static void ssd_remove_nand_blk(struct nand_block *blk)
{
    int i;
    for (i = 0; i < blk->npgs; i++)
        ssd_remove_nand_page(&blk->pg[i]);
    kfree(blk->pg);
}

// 낸드 플레인 초기화
static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    int i;
    pl->nblks = spp->blks_per_pl;
    // 블록 배열 할당
    pl->blk = kmalloc(sizeof(struct nand_block) * pl->nblks, GFP_KERNEL);
    for (i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_remove_nand_plane(struct nand_plane *pl)
{
    int i;
    for (i = 0; i < pl->nblks; i++)
        ssd_remove_nand_blk(&pl->blk[i]);
    kfree(pl->blk);
}

// 낸드 LUN(Die) 초기화
static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    int i;
    lun->npls = spp->pls_per_lun;
    // 플레인 배열 할당
    lun->pl = kmalloc(sizeof(struct nand_plane) * lun->npls, GFP_KERNEL);
    for (i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0; // LUN이 사용 가능해지는 시간 (Busy 관리용)
    lun->busy = false;
}

static void ssd_remove_nand_lun(struct nand_lun *lun)
{
    int i;
    for (i = 0; i < lun->npls; i++)
        ssd_remove_nand_plane(&lun->pl[i]);
    kfree(lun->pl);
}

// SSD 채널 초기화
static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    int i;
    ch->nluns = spp->luns_per_ch;
    // LUN 배열 할당
    ch->lun = kmalloc(sizeof(struct nand_lun) * ch->nluns, GFP_KERNEL);
    for (i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }

    // 채널 대역폭 모델 초기화 (전송 지연 시뮬레이션용)
    ch->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
    chmodel_init(ch->perf_model, spp->ch_bandwidth);

    /* 펌웨어 오버헤드 추가 */
    ch->perf_model->xfer_lat += (spp->fw_ch_xfer_lat * UNIT_XFER_SIZE / KB(4));
}

static void ssd_remove_ch(struct ssd_channel *ch)
{
    int i;
    kfree(ch->perf_model);
    for (i = 0; i < ch->nluns; i++)
        ssd_remove_nand_lun(&ch->lun[i]);
    kfree(ch->lun);
}

// PCIe 인터페이스 초기화
static void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp)
{
    pcie->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
    chmodel_init(pcie->perf_model, spp->pcie_bandwidth); // PCIe 대역폭 설정
}

static void ssd_remove_pcie(struct ssd_pcie *pcie)
{
    kfree(pcie->perf_model);
}

// 메인 SSD 구조체 초기화 (진입점)
void ssd_init(struct ssd *ssd, struct ssdparams *spp, uint32_t cpu_nr_dispatcher)
{
    uint32_t i;
    /* 파라미터 복사 */
    ssd->sp = *spp;

    /* 내부 아키텍처(채널 배열) 초기화 */
    ssd->ch = kmalloc(sizeof(struct ssd_channel) * spp->nchs, GFP_KERNEL); 
    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&(ssd->ch[i]), spp);
    }

    /* 시뮬레이션 시계 동기화를 위한 CPU 번호 설정 */
    ssd->cpu_nr_dispatcher = cpu_nr_dispatcher;

    /* PCIe 모델 초기화 */
    ssd->pcie = kmalloc(sizeof(struct ssd_pcie), GFP_KERNEL);
    ssd_init_pcie(ssd->pcie, spp);

    /* 쓰기 버퍼 초기화 */
    ssd->write_buffer = kmalloc(sizeof(struct buffer), GFP_KERNEL);
    buffer_init(ssd->write_buffer, spp->write_buffer_size);

    return;
}

void ssd_remove(struct ssd *ssd)
{
    uint32_t i;

    kfree(ssd->write_buffer);
    if (ssd->pcie) {
        kfree(ssd->pcie->perf_model);
        kfree(ssd->pcie);
    }

    for (i = 0; i < ssd->sp.nchs; i++) {
        ssd_remove_ch(&(ssd->ch[i]));
    }

    kfree(ssd->ch);
}

// ========================================================
// 4. 성능 시뮬레이션 함수들 (Timing Advancement)
// --------------------------------------------------------
// 물리적으로 대기(Sleep)하지 않고, 논리적인 시간 값만 증가시켜
// 지연 시간을 시뮬레이션합니다.
// ========================================================

// PCIe 전송 시간 계산
uint64_t ssd_advance_pcie(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
    struct channel_model *perf_model = ssd->pcie->perf_model;
    // 대역폭 모델을 이용해 전송 완료 시각 계산
    return chmodel_request(perf_model, request_time, length);
}

/* 쓰기 버퍼 성능 모델
  Y = A + (B * X)
  Y : latency (ns)
  X : transfer size (4KB unit)
  A : fw_wbuf_lat0 (기본 오버헤드)
  B : fw_wbuf_lat1 + pcie dma transfer (단위당 지연)
*/
uint64_t ssd_advance_write_buffer(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
    uint64_t nsecs_latest = request_time;
    struct ssdparams *spp = &ssd->sp;

    // 펌웨어 오버헤드 추가
    nsecs_latest += spp->fw_wbuf_lat0;
    nsecs_latest += spp->fw_wbuf_lat1 * DIV_ROUND_UP(length, KB(4));

    // PCIe 전송 시간 추가
    nsecs_latest = ssd_advance_pcie(ssd, nsecs_latest, length);

    return nsecs_latest;
}

// [핵심] 낸드 플래시 동작 시뮬레이션
// 명령(Read/Write/Erase)에 따라 실제 낸드 동작 시간과 채널 전송 시간을 계산
uint64_t ssd_advance_nand(struct ssd *ssd, struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    // 명령 시작 시간: 명시된 시간이 없으면 현재 시간 사용
    uint64_t cmd_stime = (ncmd->stime == 0) ? __get_ioclock(ssd) : ncmd->stime;
    uint64_t nand_stime, nand_etime;
    uint64_t chnl_stime, chnl_etime;
    uint64_t remaining, xfer_size, completed_time;
    struct ssdparams *spp;
    struct nand_lun *lun;
    struct ssd_channel *ch;
    struct ppa *ppa = ncmd->ppa;
    uint32_t cell;

    // 디버그 로그
    NVMEV_DEBUG(
        "SSD: %p, Enter stime: %lld, ch %d lun %d blk %d page %d command %d ppa 0x%llx\n",
        ssd, ncmd->stime, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg, c, ppa->ppa);

    if (ppa->ppa == UNMAPPED_PPA) {
        NVMEV_ERROR("Error ppa 0x%llx\n", ppa->ppa);
        return cmd_stime;
    }

    spp = &ssd->sp;
    lun = get_lun(ssd, ppa); // 해당 LUN 포인터
    ch = get_ch(ssd, ppa);   // 해당 채널 포인터
    cell = get_cell(ssd, ppa); // 셀 타입 (SLC/MLC 등)
    remaining = ncmd->xfer_size;

    switch (c) {
    case NAND_READ:
        // [읽기 동작 순서]
        // 1. NAND Array에서 Page Register로 데이터 읽기 (tR)
        // 2. 채널을 통해 컨트롤러로 데이터 전송 (tDMA)

        // LUN이 이전에 바빴다면, 끝난 시간부터 시작 (Serialization)
        nand_stime = max(lun->next_lun_avail_time, cmd_stime);

        // 낸드 읽기 시간 추가 (tR)
        if (ncmd->xfer_size == 4096) {
            nand_etime = nand_stime + spp->pg_4kb_rd_lat[cell];
        } else {
            nand_etime = nand_stime + spp->pg_rd_lat[cell];
        }

        // 채널 전송 시작 (낸드 읽기가 끝나야 가능)
        chnl_stime = nand_etime;

        // 데이터가 클 경우 쪼개서 전송 시뮬레이션
        while (remaining) {
            xfer_size = min(remaining, (uint64_t)spp->max_ch_xfer_size);
            // 채널 점유 시간 계산 (다른 LUN이 채널 쓰고 있으면 대기)
            chnl_etime = chmodel_request(ch->perf_model, chnl_stime, xfer_size);

            if (ncmd->interleave_pci_dma) { 
                // PCIe 전송과 낸드 채널 전송을 겹쳐서(Overlap) 처리 (Pipeline)
                completed_time = ssd_advance_pcie(ssd, chnl_etime, xfer_size);
            } else {
                completed_time = chnl_etime;
            }

            remaining -= xfer_size;
            chnl_stime = chnl_etime;
        }

        // LUN 사용 가능 시간 갱신
        lun->next_lun_avail_time = chnl_etime;
        break;

    case NAND_WRITE:
        // [쓰기 동작 순서]
        // 1. 채널을 통해 데이터 전송 (tDMA)
        // 2. Page Register에서 NAND Array로 프로그램 (tPROG)

        // 채널 전송부터 시작 (LUN Busy 여부 확인)
        chnl_stime = max(lun->next_lun_avail_time, cmd_stime);

        // 채널 전송 시간 계산
        chnl_etime = chmodel_request(ch->perf_model, chnl_stime, ncmd->xfer_size);

        // 낸드 프로그램 시작 (전송이 끝나야 가능)
        nand_stime = chnl_etime;
        nand_etime = nand_stime + spp->pg_wr_lat; // tPROG 추가

        // LUN 사용 가능 시간 갱신
        lun->next_lun_avail_time = nand_etime;
        completed_time = nand_etime;
        break;

    case NAND_ERASE:
        /* Erase: 데이터 전송 없음, 낸드 내부 동작만 수행 */
        nand_stime = max(lun->next_lun_avail_time, cmd_stime);
        nand_etime = nand_stime + spp->blk_er_lat; // tBERS 추가
        lun->next_lun_avail_time = nand_etime;
        completed_time = nand_etime;
        break;

    case NAND_NOP:
        /* No Operation: 단순히 동기화를 위해 현재 LUN의 완료 시간을 반환 */
        nand_stime = max(lun->next_lun_avail_time, cmd_stime);
        lun->next_lun_avail_time = nand_stime;
        completed_time = nand_stime;
        break;

    default:
        NVMEV_ERROR("Unsupported NAND command: 0x%x\n", c);
        return 0;
    }

    return completed_time;
}

// 모든 채널과 LUN을 통틀어 가장 늦게 끝나는 시간(유휴 상태가 되는 시간)을 반환
uint64_t ssd_next_idle_time(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    uint32_t i, j;
    uint64_t latest = __get_ioclock(ssd);

    for (i = 0; i < spp->nchs; i++) {
        struct ssd_channel *ch = &ssd->ch[i];

        for (j = 0; j < spp->luns_per_ch; j++) {
            struct nand_lun *lun = &ch->lun[j];
            // 모든 LUN 중 가장 늦게 일이 끝나는 시간 찾기
            latest = max(latest, lun->next_lun_avail_time);
        }
    }

    return latest;
}

void adjust_ftl_latency(int target, int lat)
{
/* TODO: 런타임에 지연 시간을 변경하고 싶을 때 사용하는 함수 (현재 사용 안 함) */
#if 0
    // ... (코드 생략) ...
#endif
}