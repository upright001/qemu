#include "qemu/osdep.h"
#include "hw/misc/mini-nand-fault-policy.h"

static void test_disabled_never_matches(void)
{
    MiniNandFaultPolicy policy;
    MiniNandFaultConfig config = { .fail_nth = 0 };

    mini_nand_fault_policy_init(&policy, config);
    for (uint32_t sequence = 1; sequence <= 4; sequence++) {
        g_assert_false(mini_nand_fault_policy_evaluate_read(&policy));
        g_assert_cmpuint(policy.eligible_read_sequence, ==, sequence);
        g_assert_false(policy.fired);
    }
}

static void test_first_read_matches_once(void)
{
    MiniNandFaultPolicy policy;
    MiniNandFaultConfig config = { .fail_nth = 1 };

    mini_nand_fault_policy_init(&policy, config);
    g_assert_true(mini_nand_fault_policy_evaluate_read(&policy));
    g_assert_false(mini_nand_fault_policy_evaluate_read(&policy));
    g_assert_cmpuint(policy.eligible_read_sequence, ==, 2);
    g_assert_true(policy.fired);
}

static void test_third_read_matches_and_fourth_is_normal(void)
{
    MiniNandFaultPolicy policy;
    MiniNandFaultConfig config = { .fail_nth = 3 };

    mini_nand_fault_policy_init(&policy, config);
    g_assert_false(mini_nand_fault_policy_evaluate_read(&policy));
    g_assert_false(mini_nand_fault_policy_evaluate_read(&policy));
    g_assert_true(mini_nand_fault_policy_evaluate_read(&policy));
    g_assert_false(mini_nand_fault_policy_evaluate_read(&policy));
    g_assert_cmpuint(policy.eligible_read_sequence, ==, 4);
    g_assert_true(policy.fired);
}

static void test_reset_preserves_config_and_rearms(void)
{
    MiniNandFaultPolicy policy;
    MiniNandFaultConfig config = { .fail_nth = 3 };

    mini_nand_fault_policy_init(&policy, config);
    for (uint32_t sequence = 1; sequence <= 3; sequence++) {
        g_assert_cmpint(mini_nand_fault_policy_evaluate_read(&policy), ==,
                        sequence == 3);
    }

    for (unsigned int reset = 0; reset < 2; reset++) {
        mini_nand_fault_policy_reset(&policy);
        g_assert_cmpuint(policy.config.fail_nth, ==, 3);
        g_assert_cmpuint(policy.eligible_read_sequence, ==, 0);
        g_assert_false(policy.fired);
        g_assert_false(mini_nand_fault_policy_evaluate_read(&policy));
        g_assert_false(mini_nand_fault_policy_evaluate_read(&policy));
        g_assert_true(mini_nand_fault_policy_evaluate_read(&policy));
    }
}

static void test_qmp_configure_clear_and_snapshot(void)
{
    MiniNandFaultPolicy policy;
    MiniNandFaultPolicySnapshot snapshot;

    mini_nand_fault_policy_init(&policy, (MiniNandFaultConfig){ .fail_nth = 3 });
    mini_nand_fault_policy_evaluate_read(&policy);
    mini_nand_fault_policy_configure_once(&policy, 2);
    snapshot = mini_nand_fault_policy_snapshot(&policy);
    g_assert_true(snapshot.enabled);
    g_assert_cmpuint(snapshot.nth, ==, 2);
    g_assert_true(snapshot.once);
    g_assert_cmpuint(snapshot.eligible_sequence, ==, 0);
    g_assert_false(snapshot.fired);
    g_assert_false(mini_nand_fault_policy_evaluate_read(&policy));
    g_assert_true(mini_nand_fault_policy_evaluate_read(&policy));

    mini_nand_fault_policy_clear(&policy);
    snapshot = mini_nand_fault_policy_snapshot(&policy);
    g_assert_false(snapshot.enabled);
    g_assert_cmpuint(snapshot.nth, ==, 0);
    g_assert_true(snapshot.once);
    g_assert_cmpuint(snapshot.eligible_sequence, ==, 0);
    g_assert_false(snapshot.fired);
    mini_nand_fault_policy_clear(&policy);
    snapshot = mini_nand_fault_policy_snapshot(&policy);
    g_assert_false(snapshot.enabled);
    g_assert_cmpuint(snapshot.nth, ==, 0);
    g_assert_true(snapshot.once);
    g_assert_cmpuint(snapshot.eligible_sequence, ==, 0);
    g_assert_false(snapshot.fired);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mini-nand-fault-policy/disabled",
                    test_disabled_never_matches);
    g_test_add_func("/mini-nand-fault-policy/fail-first",
                    test_first_read_matches_once);
    g_test_add_func("/mini-nand-fault-policy/fail-third-once",
                    test_third_read_matches_and_fourth_is_normal);
    g_test_add_func("/mini-nand-fault-policy/reset-rearm",
                    test_reset_preserves_config_and_rearms);
    g_test_add_func("/mini-nand-fault-policy/qmp-configure-clear-snapshot",
                    test_qmp_configure_clear_and_snapshot);
    return g_test_run();
}
