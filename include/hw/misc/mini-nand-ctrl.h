#ifndef HW_MISC_MINI_NAND_CTRL_H
#define HW_MISC_MINI_NAND_CTRL_H

#include "hw/core/sysbus.h"
#include "hw/misc/mini-nand-core.h"

#define TYPE_MINI_NAND_CTRL "mini-nand-ctrl"

OBJECT_DECLARE_SIMPLE_TYPE(MiniNandCtrlState, MINI_NAND_CTRL)

struct MiniNandCtrlState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MiniNandCore core;
};

#endif
