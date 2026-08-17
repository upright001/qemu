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
    /*
     * 들어오는 곳:
     * hw/misc/mini-nand-core.c::mini_nand_core_write가 호출한다.
     * 모든 request 검증을 통과한 READ만 대상이다.
     * 현재 역할:
     * eligible READ의 1-based 순서를 올려 fail_nth와 match한다.
     * 다음에 볼 코드:
     * true면 core의 Flash/DMA 전 fault terminal branch로 간다.
     * false면 mini_nand_flash_read로 간다.
     * 주의:
     * invalid request는 sequence를 소비하지 않는다.
     * fired 뒤 네 번째 READ는 once 조건 때문에 false다.
     */
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
    /*
     * 들어오는 곳:
     * hw/misc/mini-nand-qmp.c::qmp_x_mini_nand_set_fault에서
     * PRELAUNCH 설정이 controller/core를 거쳐 도착한다.
     * 현재 역할:
     * fail_nth를 바꾸고 sequence와 fired를 재무장한다.
     * 다음에 볼 코드:
     * VM 실행 뒤 mini_nand_fault_policy_evaluate_read가 사용한다.
     * 주의:
     * QMP set은 COMMAND=RESET과 다르다.
     * device register, counter, IRQ를 초기화하지 않는다.
     */
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
