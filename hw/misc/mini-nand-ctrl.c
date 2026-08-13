#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/mini-nand-ctrl.h"
#include "hw/misc/mini-nand-mmio.h"
#include "system/address-spaces.h"

static void mini_nand_ctrl_init(Object *obj)
{
    MiniNandCtrlState *s = MINI_NAND_CTRL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /*
     * MMIO callback에는 core만 전달하고 construction은 realize까지 미룬다.
     * 그러면 machine property가 guest 실행 전에 plain-C config로 고정된다.
     */
    memory_region_init_io(&s->mmio, obj, &mini_nand_mmio_ops, &s->core,
                          MINI_NAND_MMIO_NAME, MINI_NAND_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
}

static void mini_nand_ctrl_realize(DeviceState *dev, Error **errp)
{
    MiniNandCtrlState *s = MINI_NAND_CTRL(dev);

    mini_nand_core_init(&s->core, mini_nand_flash_read,
                        mini_nand_mmio_dma_write, &address_space_memory,
                        (MiniNandFaultConfig){ .fail_nth = s->fail_nth });
}

static void mini_nand_ctrl_reset_hold(Object *obj, ResetType type)
{
    MiniNandCtrlState *s = MINI_NAND_CTRL(obj);

    mini_nand_core_reset(&s->core);
}

static const Property mini_nand_ctrl_properties[] = {
    DEFINE_PROP_UINT32("fail-nth", MiniNandCtrlState, fail_nth, 0),
};

static void mini_nand_ctrl_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mini_nand_ctrl_realize;
    device_class_set_props(dc, mini_nand_ctrl_properties);
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
