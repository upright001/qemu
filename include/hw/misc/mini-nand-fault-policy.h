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

void mini_nand_fault_policy_init(MiniNandFaultPolicy *policy,
                                 MiniNandFaultConfig config);
void mini_nand_fault_policy_reset(MiniNandFaultPolicy *policy);
bool mini_nand_fault_policy_evaluate_read(MiniNandFaultPolicy *policy);

#endif
