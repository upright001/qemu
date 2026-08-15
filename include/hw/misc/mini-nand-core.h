#ifndef HW_MISC_MINI_NAND_CORE_H
#define HW_MISC_MINI_NAND_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hw/misc/mini-nand-fault-policy.h"
#include "hw/misc/mini-nand-flash.h"

#define MINI_NAND_VERSION          0x00010000U
#define MINI_NAND_CMD_READ         0x00000001U
#define MINI_NAND_CMD_RESET        0x000000ffU
#define MINI_NAND_IRQ_COMPLETE     (1U << 0)
#define MINI_NAND_IRQ_ERROR        (1U << 1)
#define MINI_NAND_IRQ_VALID_MASK \
    (MINI_NAND_IRQ_COMPLETE | MINI_NAND_IRQ_ERROR)

typedef enum MiniNandRegister {
    MINI_NAND_REG_VERSION = 0x00,
    MINI_NAND_REG_COMMAND = 0x04,
    MINI_NAND_REG_PAGE = 0x08,
    MINI_NAND_REG_DMA_ADDR_LO = 0x0c,
    MINI_NAND_REG_DMA_ADDR_HI = 0x10,
    MINI_NAND_REG_LENGTH = 0x14,
    MINI_NAND_REG_STATUS = 0x18,
    MINI_NAND_REG_ERROR_CODE = 0x1c,
    MINI_NAND_REG_READ_COUNT = 0x20,
    MINI_NAND_REG_FAULT_COUNT = 0x24,
    MINI_NAND_REG_IRQ_STATUS = 0x28,
    MINI_NAND_REG_IRQ_ENABLE = 0x2c,
} MiniNandRegister;

typedef enum MiniNandStatus {
    MINI_NAND_STATUS_IDLE = 0,
    MINI_NAND_STATUS_BUSY = 1,
    MINI_NAND_STATUS_DONE = 2,
    MINI_NAND_STATUS_ERROR = 3,
} MiniNandStatus;

typedef enum MiniNandError {
    MINI_NAND_ERR_NONE = 0,
    MINI_NAND_ERR_INVALID_CMD = 1,
    MINI_NAND_ERR_INVALID_PAGE = 2,
    MINI_NAND_ERR_INVALID_LENGTH = 3,
    MINI_NAND_ERR_DMA = 4,
    MINI_NAND_ERR_UNCORRECTABLE = 5,
} MiniNandError;

typedef enum MiniNandAccessResult {
    MINI_NAND_ACCESS_OK = 0,
    MINI_NAND_ACCESS_READ_ONLY,
    MINI_NAND_ACCESS_RESERVED,
    MINI_NAND_ACCESS_UNDEFINED,
} MiniNandAccessResult;

typedef MiniNandFlashResult (*MiniNandReadFn)(
    MiniNandFlash *flash,
    uint32_t page,
    uint8_t *dst,
    size_t length
);

typedef bool (*MiniNandDmaWriteFn)(void *opaque,
                                   uint64_t address,
                                   const uint8_t *source,
                                   size_t length);

typedef struct MiniNandCore {
    MiniNandFlash flash;
    MiniNandReadFn flash_read;
    MiniNandDmaWriteFn dma_write;
    void *dma_opaque;
    uint32_t page;
    uint32_t dma_addr_lo;
    uint32_t dma_addr_hi;
    uint32_t length;
    uint32_t status;
    uint32_t error_code;
    uint32_t read_count;
    uint32_t fault_count;
    uint32_t irq_status;
    uint32_t irq_enable;
    MiniNandFaultPolicy fault_policy;
    bool result_valid;
    uint8_t page_buffer[MINI_NAND_FLASH_PAGE_SIZE];
} MiniNandCore;

typedef struct MiniNandCoreSnapshot {
    MiniNandFaultPolicySnapshot fault;
    uint32_t status;
    uint32_t error_code;
    uint32_t read_count;
    uint32_t fault_count;
    uint32_t irq_status;
    uint32_t irq_enable;
} MiniNandCoreSnapshot;

void mini_nand_core_init(MiniNandCore *core,
                         MiniNandReadFn flash_read,
                         MiniNandDmaWriteFn dma_write,
                         void *dma_opaque,
                         MiniNandFaultConfig fault_config);
void mini_nand_core_reset(MiniNandCore *core);
bool mini_nand_core_irq_level(const MiniNandCore *core);
uint32_t mini_nand_core_read(const MiniNandCore *core,
                             uint32_t offset,
                             MiniNandAccessResult *result);
MiniNandAccessResult mini_nand_core_write(MiniNandCore *core,
                                          uint32_t offset,
                                          uint32_t value);
void mini_nand_core_configure_fault_once(MiniNandCore *core, uint32_t nth);
void mini_nand_core_clear_fault(MiniNandCore *core);
MiniNandCoreSnapshot mini_nand_core_snapshot(const MiniNandCore *core);

#endif
