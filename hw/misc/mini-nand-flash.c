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
     * 저장 상태 없이 입력 좌표만 사용한다.
     * 그래야 반복 호출 결과가 동일하다.
     */
    for (size_t offset = 0; offset < length; offset++) {
        dst[offset] = (page + offset) & 0xff;
    }

    return MINI_NAND_FLASH_OK;
}
