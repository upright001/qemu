#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/mini-nand-ctrl.h"
#include "hw/misc/mini-nand-mmio.h"
#include "hw/misc/mini-nand-qmp.h"
#include "system/address-spaces.h"

static void mini_nand_ctrl_sync_irq(MiniNandCtrlState *s)
{
    qemu_set_irq(s->irq, mini_nand_core_irq_level(&s->core));
}

/*
 * 들어오는 곳:
 * hw/misc/mini-nand-mmio.c::mini_nand_mmio_write가 호출한다.
 * 성공한 모든 register write 뒤의 callback이다.
 * 현재 역할:
 * core의 IRQ predicate를 qemu_set_irq로 output line에 반영한다.
 * 다음에 볼 코드:
 * hw/riscv/virt.c::virt_create_mini_nand가 이 line을
 * PLIC source 48인 VIRT_MINI_NAND_IRQ에 연결한다.
 * 주의:
 * 이는 host-side line sync다.
 * QEMU C callback은 firmware ISR을 직접 호출하지 않는다.
 * PLIC 전달 뒤 CPU가 mtvec으로 진입한다.
 */
static void mini_nand_ctrl_post_write(void *opaque)
{
    MiniNandCtrlState *s = opaque;

    mini_nand_ctrl_sync_irq(s);
}

void mini_nand_ctrl_configure_fault_once(MiniNandCtrlState *ctrl,
                                         uint32_t nth)
{
    mini_nand_core_configure_fault_once(&ctrl->core, nth);
}

void mini_nand_ctrl_clear_fault(MiniNandCtrlState *ctrl)
{
    mini_nand_core_clear_fault(&ctrl->core);
}

MiniNandCoreSnapshot mini_nand_ctrl_snapshot(const MiniNandCtrlState *ctrl)
{
    return mini_nand_core_snapshot(&ctrl->core);
}

static void mini_nand_ctrl_init(Object *obj)
{
    MiniNandCtrlState *s = MINI_NAND_CTRL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /*
     * MMIO adapter는 core와 QEMU line 사이의 generic bridge만 소유한다.
     * Core construction은 property가 고정되는 realize까지 미룬다.
     */
    memory_region_init_io(&s->mmio, obj, &mini_nand_mmio_ops, &s->adapter,
                          MINI_NAND_MMIO_NAME, MINI_NAND_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
}

static void mini_nand_ctrl_realize(DeviceState *dev, Error **errp)
{
    MiniNandCtrlState *s = MINI_NAND_CTRL(dev);

    mini_nand_core_init(&s->core, mini_nand_flash_read,
                        mini_nand_mmio_dma_write, &address_space_memory,
                        (MiniNandFaultConfig){ .fail_nth = s->fail_nth });
    mini_nand_mmio_adapter_init(&s->adapter, &s->core,
                                mini_nand_ctrl_post_write, s);
    mini_nand_qmp_attach(s);
    mini_nand_ctrl_sync_irq(s);
}

static void mini_nand_ctrl_reset_hold(Object *obj, ResetType type)
{
    MiniNandCtrlState *s = MINI_NAND_CTRL(obj);

    mini_nand_core_reset(&s->core);
    /* System reset은 MMIO write를 지나지 않으므로 여기서 line을 동기화한다. */
    mini_nand_ctrl_sync_irq(s);
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
