#include "qemu/osdep.h"
#include "hw/misc/mini-nand-core.h"

void mini_nand_core_init(MiniNandCore *core, MiniNandReadFn flash_read)
{
    memset(core, 0, sizeof(*core));
    core->flash_read = flash_read;
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
    core->result_valid = false;
    memset(core->page_buffer, 0, sizeof(core->page_buffer));
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
        return 0;
    case MINI_NAND_REG_IRQ_STATUS:
    case MINI_NAND_REG_IRQ_ENABLE:
        *result = MINI_NAND_ACCESS_RESERVED;
        return 0;
    default:
        *result = MINI_NAND_ACCESS_UNDEFINED;
        return 0;
    }
}

MiniNandAccessResult mini_nand_core_write(MiniNandCore *core,
                                          uint32_t offset,
                                          uint32_t value)
{
    MiniNandFlashResult flash_result;

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
         * Flash 호출과 read_count 증가는
         * 세 검증을 통과한 뒤에만 허용한다.
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

        /*
         * 동기 Flash seam은 BUSY 상태를 먼저 거쳐야 한다.
         */
        core->error_code = MINI_NAND_ERR_NONE;
        core->status = MINI_NAND_STATUS_BUSY;
        core->read_count++;
        flash_result = core->flash_read(&core->flash, core->page,
                                        core->page_buffer,
                                        sizeof(core->page_buffer));
        assert(flash_result == MINI_NAND_FLASH_OK);
        core->status = MINI_NAND_STATUS_DONE;
        core->result_valid = true;
        return MINI_NAND_ACCESS_OK;
    case MINI_NAND_REG_VERSION:
    case MINI_NAND_REG_STATUS:
    case MINI_NAND_REG_ERROR_CODE:
    case MINI_NAND_REG_READ_COUNT:
    case MINI_NAND_REG_FAULT_COUNT:
        return MINI_NAND_ACCESS_READ_ONLY;
    case MINI_NAND_REG_IRQ_STATUS:
    case MINI_NAND_REG_IRQ_ENABLE:
        return MINI_NAND_ACCESS_RESERVED;
    default:
        return MINI_NAND_ACCESS_UNDEFINED;
    }
}
