#ifndef HW_MISC_MINI_NAND_QMP_H
#define HW_MISC_MINI_NAND_QMP_H

typedef struct MiniNandCtrlState MiniNandCtrlState;

/* QMP event bridge는 realize 뒤에만 transient observer로 결합한다. */
void mini_nand_qmp_attach(MiniNandCtrlState *ctrl);

#endif
