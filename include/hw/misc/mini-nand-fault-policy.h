#ifndef HW_MISC_MINI_NAND_FAULT_POLICY_H
#define HW_MISC_MINI_NAND_FAULT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define MINI_NAND_FAULT_ONCE true

typedef struct MiniNandFaultConfig {
    uint32_t fail_nth;
} MiniNandFaultConfig;

typedef struct MiniNandFaultPolicy {
    MiniNandFaultConfig config;
    uint32_t eligible_read_sequence;
    bool fired;
} MiniNandFaultPolicy;

typedef struct MiniNandFaultPolicySnapshot {
    bool enabled;
    uint32_t nth;
    bool once;
    uint32_t eligible_sequence;
    bool fired;
} MiniNandFaultPolicySnapshot;

void mini_nand_fault_policy_init(MiniNandFaultPolicy *policy,
                                 MiniNandFaultConfig config);
void mini_nand_fault_policy_reset(MiniNandFaultPolicy *policy);
bool mini_nand_fault_policy_evaluate_read(MiniNandFaultPolicy *policy);
void mini_nand_fault_policy_configure_once(MiniNandFaultPolicy *policy,
                                           uint32_t nth);
void mini_nand_fault_policy_clear(MiniNandFaultPolicy *policy);
MiniNandFaultPolicySnapshot mini_nand_fault_policy_snapshot(
    const MiniNandFaultPolicy *policy);

#endif
