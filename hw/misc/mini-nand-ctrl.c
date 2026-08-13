#include "qemu/osdep.h"
#include "hw/misc/mini-nand-ctrl.h"
#include "hw/misc/mini-nand-mmio.h"
#include "system/address-spaces.h"

static void mini_nand_ctrl_init(Object *obj)
{
    MiniNandCtrlState *s = MINI_NAND_CTRL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    mini_nand_core_init(&s->core, mini_nand_flash_read,
                        mini_nand_mmio_dma_write, &address_space_memory);
    /*
     * MMIO callback에는 core만 전달한다.
     * QOM 없는 test도 production path를 공유한다.
     */
    memory_region_init_io(&s->mmio, obj, &mini_nand_mmio_ops, &s->core,
                          MINI_NAND_MMIO_NAME, MINI_NAND_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
}

static void mini_nand_ctrl_reset_hold(Object *obj, ResetType type)
{
    MiniNandCtrlState *s = MINI_NAND_CTRL(obj);

    mini_nand_core_reset(&s->core);
}

static void mini_nand_ctrl_class_init(ObjectClass *oc, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    rc->phases.hold = mini_nand_ctrl_reset_hold;
}

static const TypeInfo mini_nand_ctrl_types[] = {
    {
        .name = TYPE_MINI_NAND_CTRL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MiniNandCtrlState),
        .instance_init = mini_nand_ctrl_init,
        .class_init = mini_nand_ctrl_class_init,
    },
};

DEFINE_TYPES(mini_nand_ctrl_types)
