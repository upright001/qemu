#include "qemu/osdep.h"
#include "hw/misc/mini-nand-fault-policy.h"

void mini_nand_fault_policy_init(MiniNandFaultPolicy *policy,
                                 MiniNandFaultConfig config)
{
    policy->config = config;
    mini_nand_fault_policy_reset(policy);
}

void mini_nand_fault_policy_reset(MiniNandFaultPolicy *policy)
{
    /* Reset A는 boot-time 설정은 보존하고 반복 가능한 sequence만 재무장한다. */
    policy->eligible_read_sequence = 0;
    policy->fired = false;
}

bool mini_nand_fault_policy_evaluate_read(MiniNandFaultPolicy *policy)
{
    /* fail_nth는 검증을 통과한 READ의 1-based 순서이므로 비교 전에 증가한다. */
    policy->eligible_read_sequence++;
    if (policy->config.fail_nth != 0 &&
        policy->eligible_read_sequence == policy->config.fail_nth &&
        !policy->fired) {
        policy->fired = MINI_NAND_FAULT_ONCE;
        return true;
    }
    return false;
}

void mini_nand_fault_policy_configure_once(MiniNandFaultPolicy *policy,
                                           uint32_t nth)
{
    /* QMP control은 reset과 달리 설정값을 바꾸되 sequence만 새로 무장한다. */
    policy->config.fail_nth = nth;
    mini_nand_fault_policy_reset(policy);
}

void mini_nand_fault_policy_clear(MiniNandFaultPolicy *policy)
{
    mini_nand_fault_policy_configure_once(policy, 0);
}

MiniNandFaultPolicySnapshot mini_nand_fault_policy_snapshot(
    const MiniNandFaultPolicy *policy)
{
    return (MiniNandFaultPolicySnapshot) {
        .enabled = policy->config.fail_nth != 0,
        .nth = policy->config.fail_nth,
        .once = MINI_NAND_FAULT_ONCE,
        .eligible_sequence = policy->eligible_read_sequence,
        .fired = policy->fired,
    };
}
