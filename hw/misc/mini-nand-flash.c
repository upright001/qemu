#include "qemu/osdep.h"
#include "hw/misc/mini-nand-flash.h"

MiniNandFlashResult mini_nand_flash_read(MiniNandFlash *flash,
                                         uint32_t page,
                                         uint8_t *dst,
                                         size_t length)
{
    assert(flash != NULL);
    assert(dst != NULL);

    if (page >= MINI_NAND_FLASH_PAGE_COUNT) {
        return MINI_NAND_FLASH_INVALID_PAGE;
    }
    if (length != MINI_NAND_FLASH_PAGE_SIZE) {
        return MINI_NAND_FLASH_INVALID_LENGTH;
    }

    /*
     * 들어오는 곳:
     * hw/misc/mini-nand-core.c::mini_nand_core_write가 호출한다.
     * 유효하고 fault가 아닌 READ만 대상이다.
     * 현재 역할:
     * 저장 상태 없이 page/offset으로 내부 page buffer를 채운다.
     * 다음에 볼 코드:
     * hw/misc/mini-nand-mmio.c::mini_nand_mmio_dma_write가
     * 이 buffer를 guest memory에 복사한다.
     * 주의:
     * third eligible fault는 이 함수 전에 return한다.
     */
    for (size_t offset = 0; offset < length; offset++) {
        dst[offset] = (page + offset) & 0xff;
    }

    return MINI_NAND_FLASH_OK;
}
