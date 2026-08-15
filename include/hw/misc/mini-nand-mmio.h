#ifndef HW_MISC_MINI_NAND_MMIO_H
#define HW_MISC_MINI_NAND_MMIO_H

#include "system/memory.h"
#include "exec/hwaddr.h"
#include "hw/misc/mini-nand-core.h"

#define MINI_NAND_MMIO_NAME        "mini-nand-mmio"
#define MINI_NAND_MMIO_SIZE        0x1000U

typedef void (*MiniNandMmioPostWriteFn)(void *opaque);
typedef void (*MiniNandMmioFaultObserver)(void *opaque, uint32_t command,
                                          uint32_t page,
                                          uint32_t eligible_sequence,
                                          uint32_t error_code);

typedef struct MiniNandMmioAdapter {
    MiniNandCore *core;
    MiniNandMmioPostWriteFn post_write;
    void *post_write_opaque;
    MiniNandMmioFaultObserver fault_observer;
    void *fault_observer_opaque;
} MiniNandMmioAdapter;

extern const MemoryRegionOps mini_nand_mmio_ops;
void mini_nand_mmio_adapter_init(MiniNandMmioAdapter *adapter,
                                 MiniNandCore *core,
                                 MiniNandMmioPostWriteFn post_write,
                                 void *post_write_opaque);
void mini_nand_mmio_set_fault_observer(MiniNandMmioAdapter *adapter,
                                       MiniNandMmioFaultObserver observer,
                                       void *opaque);
bool mini_nand_mmio_dma_write(void *opaque,
                              uint64_t address,
                              const uint8_t *source,
                              size_t length);
bool mini_nand_mmio_range_is_free(const MemMapEntry *map,
                                  size_t entries,
                                  size_t self_index);

#endif
