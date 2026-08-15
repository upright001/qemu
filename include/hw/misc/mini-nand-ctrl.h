#ifndef HW_MISC_MINI_NAND_CTRL_H
#define HW_MISC_MINI_NAND_CTRL_H

#include "hw/core/sysbus.h"
#include "hw/misc/mini-nand-core.h"
#include "hw/misc/mini-nand-mmio.h"

#define TYPE_MINI_NAND_CTRL "mini-nand-ctrl"

OBJECT_DECLARE_SIMPLE_TYPE(MiniNandCtrlState, MINI_NAND_CTRL)

struct MiniNandCtrlState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MiniNandCore core;
    MiniNandMmioAdapter adapter;
    qemu_irq irq;
    uint32_t fail_nth;
};

void mini_nand_ctrl_configure_fault_once(MiniNandCtrlState *ctrl,
                                         uint32_t nth);
void mini_nand_ctrl_clear_fault(MiniNandCtrlState *ctrl);
MiniNandCoreSnapshot mini_nand_ctrl_snapshot(const MiniNandCtrlState *ctrl);

#endif
