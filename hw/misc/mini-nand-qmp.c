#include "qemu/osdep.h"
#include "hw/misc/mini-nand-ctrl.h"
#include "hw/misc/mini-nand-qmp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mini-nand.h"
#include "qapi/qapi-events-mini-nand.h"
#include "qapi/qapi-types-mini-nand.h"
#include "qom/object.h"
#include "system/runstate.h"

static MiniNandCtrlState *mini_nand_qmp_get(Error **errp)
{
    Object *object;

    object = object_resolve_path_type("/machine/mini-nand-ctrl",
                                      TYPE_MINI_NAND_CTRL, NULL);
    if (!object || !DEVICE(object)->realized) {
        error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                  "MiniNand controller '/machine/mini-nand-ctrl' is not available");
        return NULL;
    }
    return MINI_NAND_CTRL(object);
}

static bool mini_nand_qmp_prelaunch(Error **errp)
{
    if (!runstate_check(RUN_STATE_PRELAUNCH)) {
        error_setg(errp, "command is available only in prelaunch");
        return false;
    }
    return true;
}

static MiniNandQmpStatus mini_nand_qmp_status(uint32_t status)
{
    return status == MINI_NAND_STATUS_DONE ? MINI_NAND_QMP_STATUS_DONE :
           status == MINI_NAND_STATUS_ERROR ? MINI_NAND_QMP_STATUS_ERROR :
           status == MINI_NAND_STATUS_BUSY ? MINI_NAND_QMP_STATUS_BUSY :
           MINI_NAND_QMP_STATUS_IDLE;
}

static MiniNandQmpError mini_nand_qmp_error(uint32_t error)
{
    return error == MINI_NAND_ERR_INVALID_CMD ? MINI_NAND_QMP_ERROR_INVALID_COMMAND :
           error == MINI_NAND_ERR_INVALID_PAGE ? MINI_NAND_QMP_ERROR_INVALID_PAGE :
           error == MINI_NAND_ERR_INVALID_LENGTH ? MINI_NAND_QMP_ERROR_INVALID_LENGTH :
           error == MINI_NAND_ERR_DMA ? MINI_NAND_QMP_ERROR_DMA :
           error == MINI_NAND_ERR_UNCORRECTABLE ? MINI_NAND_QMP_ERROR_UNCORRECTABLE :
           MINI_NAND_QMP_ERROR_NONE;
}

void qmp_x_mini_nand_set_fault(uint32_t nth, bool once, Error **errp)
{
    MiniNandCtrlState *ctrl;

    if (nth == 0) {
        error_setg(errp, "nth must be greater than zero");
        return;
    }
    if (!once) {
        error_setg(errp, "once must be true");
        return;
    }
    if (!mini_nand_qmp_prelaunch(errp)) {
        return;
    }
    ctrl = mini_nand_qmp_get(errp);
    if (ctrl) {
        mini_nand_ctrl_configure_fault_once(ctrl, nth);
    }
}

void qmp_x_mini_nand_clear_fault(Error **errp)
{
    MiniNandCtrlState *ctrl;

    if (!mini_nand_qmp_prelaunch(errp)) {
        return;
    }
    ctrl = mini_nand_qmp_get(errp);
    if (ctrl) {
        mini_nand_ctrl_clear_fault(ctrl);
    }
}

MiniNandQmpInfo *qmp_x_query_mini_nand(Error **errp)
{
    MiniNandCtrlState *ctrl;
    MiniNandCoreSnapshot snapshot;
    MiniNandQmpInfo *info;

    if (!mini_nand_qmp_prelaunch(errp)) {
        return NULL;
    }
    ctrl = mini_nand_qmp_get(errp);
    if (!ctrl) {
        return NULL;
    }
    snapshot = mini_nand_ctrl_snapshot(ctrl);
    info = g_new0(MiniNandQmpInfo, 1);
    info->fault_enabled = snapshot.fault.enabled;
    info->nth = snapshot.fault.nth;
    info->once = snapshot.fault.once;
    info->eligible_sequence = snapshot.fault.eligible_sequence;
    info->fired = snapshot.fault.fired;
    info->status = mini_nand_qmp_status(snapshot.status);
    info->error = mini_nand_qmp_error(snapshot.error_code);
    info->read_count = snapshot.read_count;
    info->fault_count = snapshot.fault_count;
    info->irq_status = snapshot.irq_status;
    info->irq_enable = snapshot.irq_enable;
    return info;
}

static void mini_nand_qmp_fault_observer(void *opaque, uint32_t command,
                                         uint32_t page, uint32_t sequence,
                                         uint32_t error)
{
    /* Observer는 settled IRQ 뒤에만 호출되므로 event가 terminal state를 앞서지 않는다. */
    qapi_event_send_mini_nand_fault_injected(MINI_NAND_QMP_OPERATION_READ,
                                             page, sequence,
                                             mini_nand_qmp_error(error));
}

void mini_nand_qmp_attach(MiniNandCtrlState *ctrl)
{
    mini_nand_mmio_set_fault_observer(&ctrl->adapter,
                                      mini_nand_qmp_fault_observer, ctrl);
}
