/* Mini NAND QMP stub의 generated ABI와 실패 우선순위를 검증한다. */

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "qapi/qapi-commands-mini-nand.h"
#include "system/runstate.h"

static RunState test_runstate = RUN_STATE_PRELAUNCH;

bool runstate_check(RunState state)
{
    /* Production stub을 link하지 않아 각 runstate 거부를 독립적으로 고정한다. */
    return test_runstate == state;
}

static void assert_error(Error *error, ErrorClass expected_class,
                         const char *expected_message)
{
    g_assert_nonnull(error);
    g_assert_cmpint(error_get_class(error), ==, expected_class);
    g_assert_cmpstr(error_get_pretty(error), ==, expected_message);
    error_free(error);
}

static void test_validation_precedence(void)
{
    Error *error = NULL;

    test_runstate = RUN_STATE_RUNNING;
    qmp_x_mini_nand_set_fault(0, false, &error);
    assert_error(error, ERROR_CLASS_GENERIC_ERROR, "nth must be greater than zero");

    error = NULL;
    qmp_x_mini_nand_set_fault(1, false, &error);
    assert_error(error, ERROR_CLASS_GENERIC_ERROR, "once must be true");
}

static void test_prelaunch_and_missing_device(void)
{
    Error *error = NULL;

    test_runstate = RUN_STATE_RUNNING;
    qmp_x_mini_nand_set_fault(1, true, &error);
    assert_error(error, ERROR_CLASS_GENERIC_ERROR,
                 "command is available only in prelaunch");

    error = NULL;
    qmp_x_mini_nand_clear_fault(&error);
    assert_error(error, ERROR_CLASS_GENERIC_ERROR,
                 "command is available only in prelaunch");

    error = NULL;
    g_assert_null(qmp_x_query_mini_nand(&error));
    assert_error(error, ERROR_CLASS_GENERIC_ERROR,
                 "command is available only in prelaunch");

    error = NULL;
    test_runstate = RUN_STATE_PAUSED;
    qmp_x_mini_nand_set_fault(1, true, &error);
    assert_error(error, ERROR_CLASS_GENERIC_ERROR,
                 "command is available only in prelaunch");

    error = NULL;
    qmp_x_mini_nand_clear_fault(&error);
    assert_error(error, ERROR_CLASS_GENERIC_ERROR,
                 "command is available only in prelaunch");

    error = NULL;
    g_assert_null(qmp_x_query_mini_nand(&error));
    assert_error(error, ERROR_CLASS_GENERIC_ERROR,
                 "command is available only in prelaunch");

    error = NULL;
    test_runstate = RUN_STATE_PRELAUNCH;
    qmp_x_mini_nand_set_fault(1, true, &error);
    assert_error(error, ERROR_CLASS_DEVICE_NOT_FOUND,
                 "MiniNand controller '/machine/mini-nand-ctrl' is not available");

    error = NULL;
    qmp_x_mini_nand_clear_fault(&error);
    assert_error(error, ERROR_CLASS_DEVICE_NOT_FOUND,
                 "MiniNand controller '/machine/mini-nand-ctrl' is not available");

    error = NULL;
    g_assert_null(qmp_x_query_mini_nand(&error));
    assert_error(error, ERROR_CLASS_DEVICE_NOT_FOUND,
                 "MiniNand controller '/machine/mini-nand-ctrl' is not available");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mini-nand-qmp-stub/validation-precedence",
                    test_validation_precedence);
    g_test_add_func("/mini-nand-qmp-stub/prelaunch-and-missing-device",
                    test_prelaunch_and_missing_device);
    return g_test_run();
}
