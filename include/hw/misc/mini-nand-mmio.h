#ifndef HW_MISC_MINI_NAND_MMIO_H
#define HW_MISC_MINI_NAND_MMIO_H

#include "system/memory.h"
#include "exec/hwaddr.h"

#define MINI_NAND_MMIO_NAME        "mini-nand-mmio"
#define MINI_NAND_MMIO_SIZE        0x1000U

extern const MemoryRegionOps mini_nand_mmio_ops;
bool mini_nand_mmio_dma_write(void *opaque,
                              uint64_t address,
                              const uint8_t *source,
                              size_t length);
bool mini_nand_mmio_range_is_free(const MemMapEntry *map,
                                  size_t entries,
                                  size_t self_index);

#endif
