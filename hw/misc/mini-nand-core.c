#include "qemu/osdep.h"
#include "hw/misc/mini-nand-core.h"

void mini_nand_core_init(MiniNandCore *core,
                         MiniNandReadFn flash_read,
                         MiniNandDmaWriteFn dma_write,
                         void *dma_opaque,
                         MiniNandFaultConfig fault_config)
{
    memset(core, 0, sizeof(*core));
    core->flash_read = flash_read;
    core->dma_write = dma_write;
    core->dma_opaque = dma_opaque;
    mini_nand_fault_policy_init(&core->fault_policy, fault_config);
    core->fault_count = 0;
    mini_nand_core_reset(core);
}

void mini_nand_core_reset(MiniNandCore *core)
{
    /*
     * 시스템 reset과 COMMAND=RESET은 이 helper만 사용한다.
     * Flash seam과 handle은 construction 자원이다.
     * 따라서 reset의 초기화 대상에서 제외한다.
     */
    core->page = 0;
    core->dma_addr_lo = 0;
    core->dma_addr_hi = 0;
    core->length = 0;
    core->status = MINI_NAND_STATUS_IDLE;
    core->error_code = MINI_NAND_ERR_NONE;
    core->read_count = 0;
    core->fault_count = 0;
    core->irq_status = 0;
    core->irq_enable = 0;
    mini_nand_fault_policy_reset(&core->fault_policy);
    core->result_valid = false;
    memset(core->page_buffer, 0, sizeof(core->page_buffer));
}

bool mini_nand_core_irq_level(const MiniNandCore *core)
{
    /*
     * Core는 line이나 interrupt controller를 알지 않는다.
     * 외부 adapter가 이 순수 predicate를 사용해 전달 계층을 동기화한다.
     */
    return (core->irq_status & core->irq_enable) != 0;
}

void mini_nand_core_configure_fault_once(MiniNandCore *core, uint32_t nth)
{
    mini_nand_fault_policy_configure_once(&core->fault_policy, nth);
}

void mini_nand_core_clear_fault(MiniNandCore *core)
{
    mini_nand_fault_policy_clear(&core->fault_policy);
}

MiniNandCoreSnapshot mini_nand_core_snapshot(const MiniNandCore *core)
{
    return (MiniNandCoreSnapshot) {
        .fault = mini_nand_fault_policy_snapshot(&core->fault_policy),
        .status = core->status,
        .error_code = core->error_code,
        .read_count = core->read_count,
        .fault_count = core->fault_count,
        .irq_status = core->irq_status,
        .irq_enable = core->irq_enable,
    };
}

uint32_t mini_nand_core_read(const MiniNandCore *core,
                             uint32_t offset,
                             MiniNandAccessResult *result)
{
    switch (offset) {
    case MINI_NAND_REG_VERSION:
        *result = MINI_NAND_ACCESS_OK;
        return MINI_NAND_VERSION;
    case MINI_NAND_REG_COMMAND:
        *result = MINI_NAND_ACCESS_OK;
        return 0;
    case MINI_NAND_REG_PAGE:
        *result = MINI_NAND_ACCESS_OK;
        return core->page;
    case MINI_NAND_REG_DMA_ADDR_LO:
        *result = MINI_NAND_ACCESS_OK;
        return core->dma_addr_lo;
    case MINI_NAND_REG_DMA_ADDR_HI:
        *result = MINI_NAND_ACCESS_OK;
        return core->dma_addr_hi;
    case MINI_NAND_REG_LENGTH:
        *result = MINI_NAND_ACCESS_OK;
        return core->length;
    case MINI_NAND_REG_STATUS:
        *result = MINI_NAND_ACCESS_OK;
        return core->status;
    case MINI_NAND_REG_ERROR_CODE:
        *result = MINI_NAND_ACCESS_OK;
        return core->error_code;
    case MINI_NAND_REG_READ_COUNT:
        *result = MINI_NAND_ACCESS_OK;
        return core->read_count;
    case MINI_NAND_REG_FAULT_COUNT:
        *result = MINI_NAND_ACCESS_OK;
        return core->fault_count;
    case MINI_NAND_REG_IRQ_STATUS:
        *result = MINI_NAND_ACCESS_OK;
        return core->irq_status & MINI_NAND_IRQ_VALID_MASK;
    case MINI_NAND_REG_IRQ_ENABLE:
        *result = MINI_NAND_ACCESS_OK;
        return core->irq_enable & MINI_NAND_IRQ_VALID_MASK;
    default:
        *result = MINI_NAND_ACCESS_UNDEFINED;
        return 0;
    }
}

/*
 * 들어오는 곳:
 * hw/misc/mini-nand-mmio.c::mini_nand_mmio_write가 호출한다.
 * pure-core unit도 같은 API를 직접 호출한다.
 * 현재 역할:
 * register policy와 동기 READ state machine을 소유한다.
 * eligible READ 중 Flash와 DMA까지 성공한 경로만
 * Flash fill -> guest DMA -> DONE/COMPLETE 순서로 끝난다.
 * 다음에 볼 코드:
 * hw/misc/mini-nand-flash.c::mini_nand_flash_read와
 * hw/misc/mini-nand-mmio.c::mini_nand_mmio_dma_write다.
 * 주의:
 * 세 번째 once fault는 Flash/DMA 전에 return한다.
 * DMA 실패는 ERROR/ERROR IRQ로 끝난다.
 * COMMAND=RESET은 Flash/DMA 없이 core state를 초기화한다.
 * 따라서 guest memory는 건드리지 않는다.
 * IRQ_STATUS는 core 내부 latch다.
 * 이후 adapter/controller/board/guest 계층의 전달과 관찰은
 * hw/misc/mini-nand-mmio.c::mini_nand_mmio_write와
 * hw/misc/mini-nand-ctrl.c::mini_nand_ctrl_post_write에서 이어진다.
 */
MiniNandAccessResult mini_nand_core_write(MiniNandCore *core,
                                          uint32_t offset,
                                          uint32_t value)
{
    MiniNandFlashResult flash_result;
    uint64_t dma_address;

    switch (offset) {
    case MINI_NAND_REG_PAGE:
        core->page = value;
        return MINI_NAND_ACCESS_OK;
    case MINI_NAND_REG_DMA_ADDR_LO:
        core->dma_addr_lo = value;
        return MINI_NAND_ACCESS_OK;
    case MINI_NAND_REG_DMA_ADDR_HI:
        core->dma_addr_hi = value;
        return MINI_NAND_ACCESS_OK;
    case MINI_NAND_REG_LENGTH:
        core->length = value;
        return MINI_NAND_ACCESS_OK;
    case MINI_NAND_REG_COMMAND:
        if (value == MINI_NAND_CMD_RESET) {
            mini_nand_core_reset(core);
            return MINI_NAND_ACCESS_OK;
        }

        /*
         * 검증 실패는 이전 page buffer를 보존한다.
         * 그러나 stale 결과를 재사용할 수는 없다.
         * Flash 호출과 sequence/count 증가는
         * 네 검증을 통과한 뒤에만 허용한다.
         */
        if (value != MINI_NAND_CMD_READ) {
            core->status = MINI_NAND_STATUS_ERROR;
            core->error_code = MINI_NAND_ERR_INVALID_CMD;
            core->result_valid = false;
            return MINI_NAND_ACCESS_OK;
        }
        if (core->page >= MINI_NAND_FLASH_PAGE_COUNT) {
            core->status = MINI_NAND_STATUS_ERROR;
            core->error_code = MINI_NAND_ERR_INVALID_PAGE;
            core->result_valid = false;
            return MINI_NAND_ACCESS_OK;
        }
        if (core->length != MINI_NAND_FLASH_PAGE_SIZE) {
            core->status = MINI_NAND_STATUS_ERROR;
            core->error_code = MINI_NAND_ERR_INVALID_LENGTH;
            core->result_valid = false;
            return MINI_NAND_ACCESS_OK;
        }

        dma_address = ((uint64_t)core->dma_addr_hi << 32) |
                      core->dma_addr_lo;
        if (dma_address > UINT64_MAX - core->length) {
            core->status = MINI_NAND_STATUS_ERROR;
            core->error_code = MINI_NAND_ERR_DMA;
            core->result_valid = false;
            return MINI_NAND_ACCESS_OK;
        }

        /*
         * 새 동기 작업은 이전 결과를 먼저 무효화한다.
         * 두 callback은 같은 BUSY/NONE/count 상태를 관찰하고,
         * Flash가 채운 내부 page buffer만 DMA 경계로 전달한다.
         */
        core->error_code = MINI_NAND_ERR_NONE;
        core->status = MINI_NAND_STATUS_BUSY;
        core->result_valid = false;
        core->read_count++;
        /*
         * 들어오는 곳:
         * command/page/length/DMA 검증을 모두 통과한 READ다.
         * 현재 역할:
         * eligible sequence를 소비하고 세 번째 match면
         * ERROR/UNCORRECTABLE과 ERROR IRQ를 latch한다.
         * 다음에 볼 코드:
         * non-match는 아래 flash_read와 dma_write로 진행한다.
         * match는 mini_nand_mmio_write로 즉시 돌아간다.
         * 주의:
         * match 판단은 Flash/DMA callback보다 앞이다.
         * once policy가 fired라 네 번째 eligible READ는 정상이다.
         */
        if (mini_nand_fault_policy_evaluate_read(&core->fault_policy)) {
            /*
             * Fault match는 기존 내부 결과와 guest memory를 보존하려고
             * Flash와 DMA callback에 도달하기 전에 동기 종료한다.
             */
            core->status = MINI_NAND_STATUS_ERROR;
            core->error_code = MINI_NAND_ERR_UNCORRECTABLE;
            core->fault_count++;
            /* Terminal 관찰값이 모두 정해진 뒤 ERROR event를 게시한다. */
            core->irq_status |= MINI_NAND_IRQ_ERROR;
            return MINI_NAND_ACCESS_OK;
        }
        flash_result = core->flash_read(&core->flash, core->page,
                                        core->page_buffer,
                                        sizeof(core->page_buffer));
        assert(flash_result == MINI_NAND_FLASH_OK);
        if (!core->dma_write(core->dma_opaque, dma_address,
                             core->page_buffer,
                             sizeof(core->page_buffer))) {
            core->status = MINI_NAND_STATUS_ERROR;
            core->error_code = MINI_NAND_ERR_DMA;
            core->result_valid = false;
            /* DMA 실패의 state와 buffer 결과가 settle된 뒤 event를 게시한다. */
            core->irq_status |= MINI_NAND_IRQ_ERROR;
            return MINI_NAND_ACCESS_OK;
        }
        core->status = MINI_NAND_STATUS_DONE;
        core->result_valid = true;
        /* DMA 성공 결과가 guest-visible해진 뒤 COMPLETE event를 게시한다. */
        core->irq_status |= MINI_NAND_IRQ_COMPLETE;
        return MINI_NAND_ACCESS_OK;
    case MINI_NAND_REG_VERSION:
    case MINI_NAND_REG_STATUS:
    case MINI_NAND_REG_ERROR_CODE:
    case MINI_NAND_REG_READ_COUNT:
    case MINI_NAND_REG_FAULT_COUNT:
        return MINI_NAND_ACCESS_READ_ONLY;
    case MINI_NAND_REG_IRQ_STATUS:
        core->irq_status &= ~(value & MINI_NAND_IRQ_VALID_MASK);
        return MINI_NAND_ACCESS_OK;
    case MINI_NAND_REG_IRQ_ENABLE:
        core->irq_enable = value & MINI_NAND_IRQ_VALID_MASK;
        return MINI_NAND_ACCESS_OK;
    default:
        return MINI_NAND_ACCESS_UNDEFINED;
    }
}
