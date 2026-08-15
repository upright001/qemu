#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mini-nand.h"
#include "qapi/qapi-types-mini-nand.h"
#include "system/runstate.h"

static bool mini_nand_stub_prelaunch(Error **errp)
{
    if (!runstate_check(RUN_STATE_PRELAUNCH)) {
        error_setg(errp, "command is available only in prelaunch");
        return false;
    }
    return true;
}

static void mini_nand_stub_missing(Error **errp)
{
    error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
              "MiniNand controller '/machine/mini-nand-ctrl' is not available");
}

void qmp_x_mini_nand_set_fault(uint32_t nth, bool once, Error **errp)
{
    if (nth == 0) {
        error_setg(errp, "nth must be greater than zero");
    } else if (!once) {
        error_setg(errp, "once must be true");
    } else if (mini_nand_stub_prelaunch(errp)) {
        mini_nand_stub_missing(errp);
    }
}

void qmp_x_mini_nand_clear_fault(Error **errp)
{
    if (mini_nand_stub_prelaunch(errp)) {
        mini_nand_stub_missing(errp);
    }
}

MiniNandQmpInfo *qmp_x_query_mini_nand(Error **errp)
{
    if (mini_nand_stub_prelaunch(errp)) {
        mini_nand_stub_missing(errp);
    }
    return NULL;
}
