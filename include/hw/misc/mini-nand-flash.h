#ifndef HW_MISC_MINI_NAND_FLASH_H
#define HW_MISC_MINI_NAND_FLASH_H

#include <stddef.h>
#include <stdint.h>

#define MINI_NAND_FLASH_PAGE_SIZE  4096U
#define MINI_NAND_FLASH_PAGE_COUNT 64U
#define MINI_NAND_FLASH_TOTAL_SIZE \
    (MINI_NAND_FLASH_PAGE_SIZE * MINI_NAND_FLASH_PAGE_COUNT)

typedef struct MiniNandFlash {
    /*
     * backing store가 아닌 complete non-QOM value type token이다.
     * M1 read는 이 값을 변경하지 않는다.
     */
    uint8_t reserved;
} MiniNandFlash;

typedef enum MiniNandFlashResult {
    MINI_NAND_FLASH_OK = 0,
    MINI_NAND_FLASH_INVALID_PAGE,
    MINI_NAND_FLASH_INVALID_LENGTH,
} MiniNandFlashResult;

MiniNandFlashResult mini_nand_flash_read(
    MiniNandFlash *flash,
    uint32_t page,
    uint8_t *dst,
    size_t length
);

#endif
