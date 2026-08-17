#include "qemu/osdep.h"
#include "hw/misc/mini-nand-core.h"

typedef struct CoreSnapshot {
    MiniNandFlash flash;
    MiniNandReadFn flash_read;
    MiniNandDmaWriteFn dma_write;
    void *dma_opaque;
    uint32_t page;
    uint32_t dma_addr_lo;
    uint32_t dma_addr_hi;
    uint32_t length;
    uint32_t status;
    uint32_t error_code;
    uint32_t read_count;
    uint32_t fault_count;
    uint32_t irq_status;
    uint32_t irq_enable;
    MiniNandFaultPolicy fault_policy;
    bool result_valid;
    uint8_t page_buffer[MINI_NAND_FLASH_PAGE_SIZE];
} CoreSnapshot;

static MiniNandCore *spy_core;
static unsigned int spy_flash_calls;
static unsigned int spy_dma_calls;
static uint32_t spy_page;
static size_t spy_length;
static uint32_t spy_read_count_before;
static uint32_t spy_sequence_before;
static uint32_t spy_fault_count_before;
static uint32_t spy_irq_status_before;
static char spy_events[3];
static bool spy_dma_result;
static uint64_t spy_expected_dma_address;

static void assert_page_pattern(uint32_t page, const uint8_t *data);

static MiniNandFlashResult spy_flash_read(MiniNandFlash *flash,
                                          uint32_t page,
                                          uint8_t *dst,
                                          size_t length)
{
    g_assert_nonnull(spy_core);
    g_assert_cmpuint(spy_core->status, ==, MINI_NAND_STATUS_BUSY);
    g_assert_cmpuint(spy_core->error_code, ==, MINI_NAND_ERR_NONE);
    g_assert_false(spy_core->result_valid);
    g_assert_cmpuint(spy_core->read_count, ==,
                     spy_read_count_before + 1);
    g_assert_cmpuint(spy_core->fault_policy.eligible_read_sequence, ==,
                     spy_sequence_before + 1);
    g_assert_cmpuint(spy_core->fault_count, ==, spy_fault_count_before);
    g_assert_cmpuint(spy_core->irq_status, ==, spy_irq_status_before);
    spy_events[spy_flash_calls + spy_dma_calls] = 'F';
    spy_flash_calls++;
    spy_page = page;
    spy_length = length;
    return mini_nand_flash_read(flash, page, dst, length);
}

static bool spy_dma_write(void *opaque, uint64_t address,
                          const uint8_t *source, size_t length)
{
    g_assert_true(opaque == spy_core);
    g_assert_cmpuint(spy_core->status, ==, MINI_NAND_STATUS_BUSY);
    g_assert_cmpuint(spy_core->error_code, ==, MINI_NAND_ERR_NONE);
    g_assert_false(spy_core->result_valid);
    g_assert_cmpuint(spy_core->read_count, ==,
                     spy_read_count_before + 1);
    g_assert_cmpuint(spy_core->fault_policy.eligible_read_sequence, ==,
                     spy_sequence_before + 1);
    g_assert_cmpuint(spy_core->fault_count, ==, spy_fault_count_before);
    g_assert_cmpuint(spy_core->irq_status, ==, spy_irq_status_before);
    g_assert_cmphex(address, ==, spy_expected_dma_address);
    g_assert_cmpuint(length, ==, MINI_NAND_FLASH_PAGE_SIZE);
    assert_page_pattern(spy_core->page, source);
    spy_events[spy_flash_calls + spy_dma_calls] = 'D';
    spy_dma_calls++;
    return spy_dma_result;
}

static void spy_reset(MiniNandCore *core)
{
    spy_core = core;
    spy_flash_calls = 0;
    spy_dma_calls = 0;
    spy_page = UINT32_MAX;
    spy_length = 0;
    spy_read_count_before = core->read_count;
    spy_sequence_before = core->fault_policy.eligible_read_sequence;
    spy_fault_count_before = core->fault_count;
    spy_irq_status_before = core->irq_status;
    memset(spy_events, 0, sizeof(spy_events));
    spy_dma_result = true;
    spy_expected_dma_address = UINT64_C(0x0000000081000000);
}

static CoreSnapshot snapshot(const MiniNandCore *core)
{
    CoreSnapshot state = {
        .flash = core->flash,
        .flash_read = core->flash_read,
        .dma_write = core->dma_write,
        .dma_opaque = core->dma_opaque,
        .page = core->page,
        .dma_addr_lo = core->dma_addr_lo,
        .dma_addr_hi = core->dma_addr_hi,
        .length = core->length,
        .status = core->status,
        .error_code = core->error_code,
        .read_count = core->read_count,
        .fault_count = core->fault_count,
        .irq_status = core->irq_status,
        .irq_enable = core->irq_enable,
        .fault_policy = core->fault_policy,
        .result_valid = core->result_valid,
    };

    memcpy(state.page_buffer, core->page_buffer, sizeof(state.page_buffer));
    return state;
}

static void assert_snapshot_equal(const CoreSnapshot *actual,
                                  const CoreSnapshot *expected)
{
    g_assert_cmpmem(&actual->flash, sizeof(actual->flash),
                    &expected->flash, sizeof(expected->flash));
    g_assert_true(actual->flash_read == expected->flash_read);
    g_assert_true(actual->dma_write == expected->dma_write);
    g_assert_true(actual->dma_opaque == expected->dma_opaque);
    g_assert_cmpuint(actual->page, ==, expected->page);
    g_assert_cmpuint(actual->dma_addr_lo, ==, expected->dma_addr_lo);
    g_assert_cmpuint(actual->dma_addr_hi, ==, expected->dma_addr_hi);
    g_assert_cmpuint(actual->length, ==, expected->length);
    g_assert_cmpuint(actual->status, ==, expected->status);
    g_assert_cmpuint(actual->error_code, ==, expected->error_code);
    g_assert_cmpuint(actual->read_count, ==, expected->read_count);
    g_assert_cmpuint(actual->fault_count, ==, expected->fault_count);
    g_assert_cmpuint(actual->irq_status, ==, expected->irq_status);
    g_assert_cmpuint(actual->irq_enable, ==, expected->irq_enable);
    g_assert_cmpuint(actual->fault_policy.config.fail_nth, ==,
                     expected->fault_policy.config.fail_nth);
    g_assert_cmpuint(actual->fault_policy.eligible_read_sequence, ==,
                     expected->fault_policy.eligible_read_sequence);
    g_assert_cmpint(actual->fault_policy.fired, ==,
                    expected->fault_policy.fired);
    g_assert_cmpint(actual->result_valid, ==, expected->result_valid);
    g_assert_cmpmem(actual->page_buffer, sizeof(actual->page_buffer),
                    expected->page_buffer, sizeof(expected->page_buffer));
}

static void assert_reset_snapshot(const MiniNandCore *core)
{
    static const uint8_t zeros[MINI_NAND_FLASH_PAGE_SIZE];

    g_assert_cmpuint(core->page, ==, 0);
    g_assert_cmpuint(core->dma_addr_lo, ==, 0);
    g_assert_cmpuint(core->dma_addr_hi, ==, 0);
    g_assert_cmpuint(core->length, ==, 0);
    g_assert_cmpuint(core->status, ==, MINI_NAND_STATUS_IDLE);
    g_assert_cmpuint(core->error_code, ==, MINI_NAND_ERR_NONE);
    g_assert_cmpuint(core->read_count, ==, 0);
    g_assert_cmpuint(core->fault_count, ==, 0);
    g_assert_cmpuint(core->irq_status, ==, 0);
    g_assert_cmpuint(core->irq_enable, ==, 0);
    g_assert_false(mini_nand_core_irq_level(core));
    g_assert_false(core->result_valid);
    g_assert_cmpmem(core->page_buffer, sizeof(core->page_buffer),
                    zeros, sizeof(zeros));
}

static uint32_t core_read(const MiniNandCore *core, uint32_t offset)
{
    MiniNandAccessResult result;
    uint32_t value = mini_nand_core_read(core, offset, &result);

    g_assert_cmpint(result, ==, MINI_NAND_ACCESS_OK);
    return value;
}

static void configure_valid_read(MiniNandCore *core, uint32_t page)
{
    g_assert_cmpint(mini_nand_core_write(core, MINI_NAND_REG_PAGE, page),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpint(mini_nand_core_write(core, MINI_NAND_REG_DMA_ADDR_LO,
                                         0x81000000),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpint(mini_nand_core_write(core, MINI_NAND_REG_DMA_ADDR_HI,
                                         0),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpint(mini_nand_core_write(core, MINI_NAND_REG_LENGTH,
                                         MINI_NAND_FLASH_PAGE_SIZE),
                    ==, MINI_NAND_ACCESS_OK);
}

static void configure_read(MiniNandCore *core, uint32_t page,
                           uint32_t length, uint64_t address)
{
    mini_nand_core_write(core, MINI_NAND_REG_PAGE, page);
    mini_nand_core_write(core, MINI_NAND_REG_DMA_ADDR_LO, address);
    mini_nand_core_write(core, MINI_NAND_REG_DMA_ADDR_HI, address >> 32);
    mini_nand_core_write(core, MINI_NAND_REG_LENGTH, length);
}

static void submit_read(MiniNandCore *core)
{
    g_assert_cmpint(mini_nand_core_write(core, MINI_NAND_REG_COMMAND,
                                         MINI_NAND_CMD_READ),
                    ==, MINI_NAND_ACCESS_OK);
}

static void assert_page_pattern(uint32_t page, const uint8_t *data)
{
    for (size_t offset = 0; offset < MINI_NAND_FLASH_PAGE_SIZE; offset++) {
        g_assert_cmpuint(data[offset], ==, (page + offset) & 0xff);
    }
}

static void test_dma_success_order_address(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_COMMAND, 2),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpuint(core.error_code, ==, MINI_NAND_ERR_INVALID_CMD);
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);

    g_assert_cmpuint(spy_flash_calls, ==, 1);
    g_assert_cmpuint(spy_dma_calls, ==, 1);
    g_assert_cmpstr(spy_events, ==, "FD");
    g_assert_cmpuint(core.status, ==, MINI_NAND_STATUS_DONE);
    g_assert_cmpuint(core.error_code, ==, MINI_NAND_ERR_NONE);
    g_assert_cmpuint(core.irq_status, ==, MINI_NAND_IRQ_COMPLETE);
    g_assert_true(core.result_valid);
    assert_page_pattern(42, core.page_buffer);
}

static void assert_rejected_without_operation(MiniNandCore *core,
                                              MiniNandError error,
                                              uint32_t count_before,
                                              const uint8_t *buffer_before)
{
    g_assert_cmpuint(core->status, ==, MINI_NAND_STATUS_ERROR);
    g_assert_cmpuint(core->error_code, ==, error);
    g_assert_false(core->result_valid);
    g_assert_cmpuint(core->read_count, ==, count_before);
    g_assert_cmpuint(spy_flash_calls, ==, 0);
    g_assert_cmpuint(spy_dma_calls, ==, 0);
    g_assert_cmpmem(core->page_buffer, sizeof(core->page_buffer),
                    buffer_before, MINI_NAND_FLASH_PAGE_SIZE);
}

static void test_dma_overflow_precedence(void)
{
    MiniNandCore core;
    uint8_t buffer_before[MINI_NAND_FLASH_PAGE_SIZE];
    uint32_t count_before;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    memset(core.page_buffer, 0xa5, sizeof(core.page_buffer));
    memcpy(buffer_before, core.page_buffer, sizeof(buffer_before));

    configure_read(&core, MINI_NAND_FLASH_PAGE_COUNT,
                   MINI_NAND_FLASH_PAGE_SIZE - 1,
                   UINT64_C(0xfffffffffffff000));
    count_before = core.read_count;
    spy_reset(&core);
    mini_nand_core_write(&core, MINI_NAND_REG_COMMAND, 2);
    assert_rejected_without_operation(&core, MINI_NAND_ERR_INVALID_CMD,
                                      count_before, buffer_before);

    configure_read(&core, MINI_NAND_FLASH_PAGE_COUNT,
                   MINI_NAND_FLASH_PAGE_SIZE - 1,
                   UINT64_C(0xfffffffffffff000));
    count_before = core.read_count;
    spy_reset(&core);
    submit_read(&core);
    assert_rejected_without_operation(&core, MINI_NAND_ERR_INVALID_PAGE,
                                      count_before, buffer_before);

    configure_read(&core, 42, MINI_NAND_FLASH_PAGE_SIZE - 1,
                   UINT64_C(0xfffffffffffff000));
    count_before = core.read_count;
    spy_reset(&core);
    submit_read(&core);
    assert_rejected_without_operation(&core, MINI_NAND_ERR_INVALID_LENGTH,
                                      count_before, buffer_before);

    configure_read(&core, 42, MINI_NAND_FLASH_PAGE_SIZE,
                   UINT64_C(0xfffffffffffff000));
    count_before = core.read_count;
    spy_reset(&core);
    submit_read(&core);
    assert_rejected_without_operation(&core, MINI_NAND_ERR_DMA,
                                      count_before, buffer_before);
}

static void test_dma_overflow_safe_boundary(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_read(&core, 42, MINI_NAND_FLASH_PAGE_SIZE,
                   UINT64_C(0xffffffffffffefff));
    spy_reset(&core);
    spy_expected_dma_address = UINT64_C(0xffffffffffffefff);
    submit_read(&core);

    g_assert_cmpuint(spy_flash_calls, ==, 1);
    g_assert_cmpuint(spy_dma_calls, ==, 1);
    g_assert_cmpstr(spy_events, ==, "FD");
    g_assert_cmpuint(core.status, ==, MINI_NAND_STATUS_DONE);
    g_assert_true(core.result_valid);
}

static void test_dma_transaction_failure(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    spy_dma_result = false;
    submit_read(&core);

    g_assert_cmpuint(spy_flash_calls, ==, 1);
    g_assert_cmpuint(spy_dma_calls, ==, 1);
    g_assert_cmpstr(spy_events, ==, "FD");
    g_assert_cmpuint(core.read_count, ==, 1);
    g_assert_cmpuint(core.status, ==, MINI_NAND_STATUS_ERROR);
    g_assert_cmpuint(core.error_code, ==, MINI_NAND_ERR_DMA);
    g_assert_cmpuint(core.irq_status, ==, MINI_NAND_IRQ_ERROR);
    g_assert_false(core.result_valid);
    assert_page_pattern(42, core.page_buffer);
}

static void test_dma_reset_preserves_seam(void)
{
    MiniNandCore core;
    MiniNandDmaWriteFn dma_write;
    void *dma_opaque;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    dma_write = core.dma_write;
    dma_opaque = core.dma_opaque;
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    mini_nand_core_reset(&core);

    g_assert_true(core.dma_write == dma_write);
    g_assert_true(core.dma_opaque == dma_opaque);
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    g_assert_cmpuint(spy_dma_calls, ==, 1);
}

static void test_dma_recovery_after_error(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    spy_dma_result = false;
    submit_read(&core);
    g_assert_cmpuint(core.error_code, ==, MINI_NAND_ERR_DMA);

    configure_valid_read(&core, 43);
    spy_reset(&core);
    submit_read(&core);
    g_assert_cmpuint(spy_flash_calls, ==, 1);
    g_assert_cmpuint(spy_dma_calls, ==, 1);
    g_assert_cmpstr(spy_events, ==, "FD");
    g_assert_cmpuint(core.status, ==, MINI_NAND_STATUS_DONE);
    g_assert_cmpuint(core.error_code, ==, MINI_NAND_ERR_NONE);
    g_assert_cmpuint(core.read_count, ==, 2);
    g_assert_true(core.result_valid);
}

static void test_construction_and_system_reset(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    assert_reset_snapshot(&core);
    mini_nand_core_write(&core, MINI_NAND_REG_PAGE, 42);
    mini_nand_core_write(&core, MINI_NAND_REG_LENGTH,
                         MINI_NAND_FLASH_PAGE_SIZE);
    mini_nand_core_reset(&core);
    assert_reset_snapshot(&core);
}

static void test_register_dispositions(void)
{
    static const uint32_t read_only[] = {
        MINI_NAND_REG_VERSION,
        MINI_NAND_REG_STATUS,
        MINI_NAND_REG_ERROR_CODE,
        MINI_NAND_REG_READ_COUNT,
        MINI_NAND_REG_FAULT_COUNT,
    };
    MiniNandCore core;
    CoreSnapshot before;
    CoreSnapshot after;
    MiniNandAccessResult result;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_VERSION), ==,
                     MINI_NAND_VERSION);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_COMMAND), ==, 0);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_STATUS), ==,
                     MINI_NAND_STATUS_IDLE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_ERROR_CODE), ==,
                     MINI_NAND_ERR_NONE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_READ_COUNT), ==, 0);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_FAULT_COUNT), ==, 0);
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_PAGE, 42),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_DMA_ADDR_LO,
                                         0x81000000),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_DMA_ADDR_HI,
                                         0x12345678),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_LENGTH,
                                         MINI_NAND_FLASH_PAGE_SIZE),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_PAGE), ==, 42);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_DMA_ADDR_LO), ==,
                     0x81000000);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_DMA_ADDR_HI), ==,
                     0x12345678);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_LENGTH), ==,
                     MINI_NAND_FLASH_PAGE_SIZE);

    before = snapshot(&core);
    for (size_t index = 0; index < G_N_ELEMENTS(read_only); index++) {
        g_assert_cmpint(mini_nand_core_write(&core, read_only[index], 1),
                        ==, MINI_NAND_ACCESS_READ_ONLY);
        after = snapshot(&core);
        assert_snapshot_equal(&after, &before);
    }
    g_assert_cmpint(mini_nand_core_write(&core, 0x30, 1),
                    ==, MINI_NAND_ACCESS_UNDEFINED);
    after = snapshot(&core);
    assert_snapshot_equal(&after, &before);
    g_assert_cmpuint(mini_nand_core_read(&core, 0x30, &result), ==, 0);
    g_assert_cmpint(result, ==, MINI_NAND_ACCESS_UNDEFINED);
}

static void test_irq_reset_mask_and_late_enable(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    assert_reset_snapshot(&core);
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);

    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_IRQ_STATUS), ==,
                     MINI_NAND_IRQ_COMPLETE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_IRQ_ENABLE), ==, 0);
    g_assert_false(mini_nand_core_irq_level(&core));

    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_IRQ_ENABLE,
                                         UINT32_MAX),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_IRQ_ENABLE), ==,
                     MINI_NAND_IRQ_VALID_MASK);
    g_assert_true(mini_nand_core_irq_level(&core));

    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_IRQ_ENABLE, 0),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_IRQ_STATUS), ==,
                     MINI_NAND_IRQ_COMPLETE);
    g_assert_false(mini_nand_core_irq_level(&core));

    mini_nand_core_reset(&core);
    assert_reset_snapshot(&core);
}

static void test_irq_sticky_selective_w1c_isolation(void)
{
    MiniNandCore core;
    CoreSnapshot expected;
    CoreSnapshot actual;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);

    spy_reset(&core);
    spy_dma_result = false;
    submit_read(&core);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_IRQ_STATUS), ==,
                     MINI_NAND_IRQ_VALID_MASK);
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_IRQ_ENABLE,
                                         MINI_NAND_IRQ_VALID_MASK),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_true(mini_nand_core_irq_level(&core));

    expected = snapshot(&core);
    expected.irq_status = MINI_NAND_IRQ_ERROR;
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_IRQ_STATUS,
                                         MINI_NAND_IRQ_COMPLETE | (1U << 31)),
                    ==, MINI_NAND_ACCESS_OK);
    actual = snapshot(&core);
    assert_snapshot_equal(&actual, &expected);
    g_assert_true(mini_nand_core_irq_level(&core));

    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_IRQ_STATUS,
                                         MINI_NAND_IRQ_ERROR),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_IRQ_STATUS), ==, 0);
    g_assert_false(mini_nand_core_irq_level(&core));
}

static void test_valid_page_42_read_observes_busy_and_completes(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);

    g_assert_cmpuint(spy_flash_calls, ==, 1);
    g_assert_cmpuint(spy_page, ==, 42);
    g_assert_cmpuint(spy_length, ==, MINI_NAND_FLASH_PAGE_SIZE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_STATUS), ==,
                     MINI_NAND_STATUS_DONE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_ERROR_CODE), ==,
                     MINI_NAND_ERR_NONE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_READ_COUNT), ==, 1);
    g_assert_true(core.result_valid);
    assert_page_pattern(42, core.page_buffer);
}

static void assert_invalid_request(MiniNandCore *core, MiniNandError error)
{
    CoreSnapshot before = snapshot(core);
    MiniNandFlash flash_before = core->flash;

    spy_reset(core);
    submit_read(core);
    g_assert_cmpuint(spy_flash_calls, ==, 0);
    g_assert_cmpuint(spy_dma_calls, ==, 0);
    g_assert_cmpuint(core_read(core, MINI_NAND_REG_STATUS), ==,
                     MINI_NAND_STATUS_ERROR);
    g_assert_cmpuint(core_read(core, MINI_NAND_REG_ERROR_CODE), ==, error);
    g_assert_false(core->result_valid);
    g_assert_cmpuint(core->read_count, ==, before.read_count);
    g_assert_cmpmem(core->page_buffer, sizeof(core->page_buffer),
                    before.page_buffer, sizeof(before.page_buffer));
    g_assert_cmpmem(&core->flash, sizeof(core->flash), &flash_before,
                    sizeof(flash_before));
}

static void test_invalid_command_preserves_valid_result_buffer(void)
{
    MiniNandCore core;
    CoreSnapshot before;
    MiniNandFlash flash_before;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    before = snapshot(&core);
    flash_before = core.flash;
    spy_reset(&core);
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_COMMAND, 2),
                    ==, MINI_NAND_ACCESS_OK);
    g_assert_cmpuint(spy_flash_calls, ==, 0);
    g_assert_cmpuint(spy_dma_calls, ==, 0);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_STATUS), ==,
                     MINI_NAND_STATUS_ERROR);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_ERROR_CODE), ==,
                     MINI_NAND_ERR_INVALID_CMD);
    g_assert_false(core.result_valid);
    g_assert_cmpuint(core.read_count, ==, before.read_count);
    g_assert_cmpmem(core.page_buffer, sizeof(core.page_buffer),
                    before.page_buffer, sizeof(before.page_buffer));
    g_assert_cmpmem(&core.flash, sizeof(core.flash), &flash_before,
                    sizeof(flash_before));
}

static void test_invalid_page_preserves_buffer_and_count(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    mini_nand_core_write(&core, MINI_NAND_REG_PAGE,
                         MINI_NAND_FLASH_PAGE_COUNT);
    assert_invalid_request(&core, MINI_NAND_ERR_INVALID_PAGE);
}

static void test_invalid_length_preserves_buffer_and_count(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    mini_nand_core_write(&core, MINI_NAND_REG_LENGTH,
                         MINI_NAND_FLASH_PAGE_SIZE - 1);
    assert_invalid_request(&core, MINI_NAND_ERR_INVALID_LENGTH);
}

static void test_both_invalid_prefers_page_error(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, MINI_NAND_FLASH_PAGE_COUNT);
    mini_nand_core_write(&core, MINI_NAND_REG_LENGTH,
                         MINI_NAND_FLASH_PAGE_SIZE - 1);
    assert_invalid_request(&core, MINI_NAND_ERR_INVALID_PAGE);
}

static void test_done_and_error_recover_with_valid_read(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    configure_valid_read(&core, 43);
    spy_reset(&core);
    submit_read(&core);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_STATUS), ==,
                     MINI_NAND_STATUS_DONE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_READ_COUNT), ==, 2);

    mini_nand_core_write(&core, MINI_NAND_REG_PAGE,
                         MINI_NAND_FLASH_PAGE_COUNT);
    assert_invalid_request(&core, MINI_NAND_ERR_INVALID_PAGE);
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    g_assert_cmpuint(spy_flash_calls, ==, 1);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_STATUS), ==,
                     MINI_NAND_STATUS_DONE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_ERROR_CODE), ==,
                     MINI_NAND_ERR_NONE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_READ_COUNT), ==, 3);
}

static void test_command_reset_from_done_and_error(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_COMMAND,
                                         MINI_NAND_CMD_RESET),
                    ==, MINI_NAND_ACCESS_OK);
    assert_reset_snapshot(&core);

    configure_valid_read(&core, MINI_NAND_FLASH_PAGE_COUNT);
    assert_invalid_request(&core, MINI_NAND_ERR_INVALID_PAGE);
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_COMMAND,
                                         MINI_NAND_CMD_RESET),
                    ==, MINI_NAND_ACCESS_OK);
    assert_reset_snapshot(&core);
}

static void test_system_and_command_reset_snapshots_match(void)
{
    MiniNandCore system_core;
    MiniNandCore command_core;
    CoreSnapshot system_snapshot;
    CoreSnapshot command_snapshot;

    mini_nand_core_init(&system_core, spy_flash_read,
                        spy_dma_write, &system_core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    mini_nand_core_init(&command_core, spy_flash_read,
                        spy_dma_write, &command_core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&system_core, 42);
    configure_valid_read(&command_core, 42);
    spy_reset(&system_core);
    submit_read(&system_core);
    spy_reset(&command_core);
    submit_read(&command_core);
    mini_nand_core_reset(&system_core);
    mini_nand_core_write(&command_core, MINI_NAND_REG_COMMAND,
                         MINI_NAND_CMD_RESET);
    system_snapshot = snapshot(&system_core);
    command_snapshot = snapshot(&command_core);
    /* 두 인스턴스의 construction opaque는 다르므로 reset 소유 상태 비교에서 정규화한다. */
    command_snapshot.dma_opaque = system_snapshot.dma_opaque;
    assert_snapshot_equal(&system_snapshot, &command_snapshot);
}

static void test_reset_preserves_backend_and_flash_handle(void)
{
    MiniNandCore core;
    MiniNandReadFn backend;
    MiniNandFlash flash;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    core.flash.reserved = 0x5a;
    backend = core.flash_read;
    flash = core.flash;
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    mini_nand_core_reset(&core);
    g_assert_true(core.flash_read == backend);
    g_assert_cmpmem(&core.flash, sizeof(core.flash), &flash, sizeof(flash));
}

static void test_post_reset_valid_read_succeeds(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    mini_nand_core_reset(&core);
    configure_valid_read(&core, 42);
    spy_reset(&core);
    submit_read(&core);
    g_assert_cmpuint(spy_flash_calls, ==, 1);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_STATUS), ==,
                     MINI_NAND_STATUS_DONE);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_READ_COUNT), ==, 1);
}

static void assert_rejected_snapshot(MiniNandCore *core,
                                     uint32_t command,
                                     MiniNandError error)
{
    CoreSnapshot expected = snapshot(core);
    CoreSnapshot actual;

    expected.status = MINI_NAND_STATUS_ERROR;
    expected.error_code = error;
    expected.result_valid = false;
    spy_reset(core);
    g_assert_cmpint(mini_nand_core_write(core, MINI_NAND_REG_COMMAND,
                                         command),
                    ==, MINI_NAND_ACCESS_OK);
    actual = snapshot(core);
    assert_snapshot_equal(&actual, &expected);
    g_assert_cmpuint(spy_flash_calls, ==, 0);
    g_assert_cmpuint(spy_dma_calls, ==, 0);
    g_assert_cmpstr(spy_events, ==, "");
}

static void test_fault_rejections_do_not_consume_sequence(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 3 });
    memset(core.page_buffer, 0xa5, sizeof(core.page_buffer));
    core.irq_status = MINI_NAND_IRQ_COMPLETE;
    g_assert_cmpint(mini_nand_core_write(&core, MINI_NAND_REG_IRQ_ENABLE,
                                         MINI_NAND_IRQ_COMPLETE),
                    ==, MINI_NAND_ACCESS_OK);

    configure_valid_read(&core, 42);
    assert_rejected_snapshot(&core, 2, MINI_NAND_ERR_INVALID_CMD);

    configure_read(&core, MINI_NAND_FLASH_PAGE_COUNT,
                   MINI_NAND_FLASH_PAGE_SIZE,
                   UINT64_C(0x0000000081000000));
    assert_rejected_snapshot(&core, MINI_NAND_CMD_READ,
                             MINI_NAND_ERR_INVALID_PAGE);

    configure_read(&core, 42, MINI_NAND_FLASH_PAGE_SIZE - 1,
                   UINT64_C(0x0000000081000000));
    assert_rejected_snapshot(&core, MINI_NAND_CMD_READ,
                             MINI_NAND_ERR_INVALID_LENGTH);

    configure_read(&core, 42, MINI_NAND_FLASH_PAGE_SIZE,
                   UINT64_C(0xfffffffffffff000));
    assert_rejected_snapshot(&core, MINI_NAND_CMD_READ,
                             MINI_NAND_ERR_DMA);
}

static void test_third_read_fault_skips_flash_and_dma(void)
{
    MiniNandCore core;
    uint8_t buffer_before[MINI_NAND_FLASH_PAGE_SIZE];

    /*
     * 들어오는 곳:
     * /mini-nand-core/fault-third-skip-fourth-normal selector다.
     * QOM 없이 core와 spy callback만 실행한다.
     * 현재 역할:
     * READ 1~2의 F->D, READ 3의 callback 0회를 증명한다.
     * buffer 보존과 READ 4 정상 복귀도 확인한다.
     * 다음에 볼 코드:
     * tests/qtest/mini-nand-test.c의 test_qmp_event_third_fault다.
     * 실제 QEMU device/QMP 경계를 보완한다.
     * 주의:
     * 이 unit은 real MMIO, guest DMA, PLIC, QAPI를 증명하지 않는다.
     */
    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 3 });
    configure_valid_read(&core, 42);

    for (uint32_t sequence = 1; sequence <= 2; sequence++) {
        spy_reset(&core);
        submit_read(&core);
        g_assert_cmpstr(spy_events, ==, "FD");
        g_assert_cmpuint(core.read_count, ==, sequence);
        g_assert_cmpuint(core.fault_policy.eligible_read_sequence, ==,
                         sequence);
        g_assert_cmpuint(core.fault_count, ==, 0);
        g_assert_cmpuint(core.status, ==, MINI_NAND_STATUS_DONE);
    }

    memset(core.page_buffer, 0xa5, sizeof(core.page_buffer));
    memcpy(buffer_before, core.page_buffer, sizeof(buffer_before));
    spy_reset(&core);
    submit_read(&core);
    g_assert_cmpuint(spy_flash_calls, ==, 0);
    g_assert_cmpuint(spy_dma_calls, ==, 0);
    g_assert_cmpstr(spy_events, ==, "");
    g_assert_cmpuint(core.read_count, ==, 3);
    g_assert_cmpuint(core.fault_policy.eligible_read_sequence, ==, 3);
    g_assert_true(core.fault_policy.fired);
    g_assert_cmpuint(core.fault_count, ==, 1);
    g_assert_cmpuint(core_read(&core, MINI_NAND_REG_FAULT_COUNT), ==, 1);
    g_assert_cmpuint(core.status, ==, MINI_NAND_STATUS_ERROR);
    g_assert_cmpuint(core.error_code, ==, MINI_NAND_ERR_UNCORRECTABLE);
    g_assert_cmpuint(core.irq_status, ==, MINI_NAND_IRQ_VALID_MASK);
    g_assert_false(core.result_valid);
    g_assert_cmpmem(core.page_buffer, sizeof(core.page_buffer),
                    buffer_before, sizeof(buffer_before));

    spy_reset(&core);
    submit_read(&core);
    g_assert_cmpstr(spy_events, ==, "FD");
    g_assert_cmpuint(core.read_count, ==, 4);
    g_assert_cmpuint(core.fault_policy.eligible_read_sequence, ==, 4);
    g_assert_cmpuint(core.fault_count, ==, 1);
    g_assert_cmpuint(core.status, ==, MINI_NAND_STATUS_DONE);
    g_assert_cmpuint(core.error_code, ==, MINI_NAND_ERR_NONE);
    g_assert_true(core.result_valid);
    assert_page_pattern(42, core.page_buffer);
}

static void test_nonmatching_dma_failure_consumes_sequence(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 3 });
    configure_valid_read(&core, 42);
    spy_reset(&core);
    spy_dma_result = false;
    submit_read(&core);

    g_assert_cmpstr(spy_events, ==, "FD");
    g_assert_cmpuint(core.fault_policy.eligible_read_sequence, ==, 1);
    g_assert_false(core.fault_policy.fired);
    g_assert_cmpuint(core.read_count, ==, 1);
    g_assert_cmpuint(core.fault_count, ==, 0);
    g_assert_cmpuint(core.status, ==, MINI_NAND_STATUS_ERROR);
    g_assert_cmpuint(core.error_code, ==, MINI_NAND_ERR_DMA);
    g_assert_false(core.result_valid);
}

static void assert_third_read_fault(MiniNandCore *core)
{
    configure_valid_read(core, 42);
    for (uint32_t sequence = 1; sequence <= 3; sequence++) {
        spy_reset(core);
        submit_read(core);
        if (sequence < 3) {
            g_assert_cmpstr(spy_events, ==, "FD");
            g_assert_cmpuint(core->status, ==, MINI_NAND_STATUS_DONE);
        } else {
            g_assert_cmpstr(spy_events, ==, "");
            g_assert_cmpuint(core->status, ==, MINI_NAND_STATUS_ERROR);
            g_assert_cmpuint(core->error_code, ==,
                             MINI_NAND_ERR_UNCORRECTABLE);
        }
    }
    g_assert_cmpuint(core->read_count, ==, 3);
    g_assert_cmpuint(core->fault_count, ==, 1);
    g_assert_true(core->fault_policy.fired);
}

static void assert_fault_reset_state(const MiniNandCore *core)
{
    assert_reset_snapshot(core);
    g_assert_cmpuint(core->fault_policy.config.fail_nth, ==, 3);
    g_assert_cmpuint(core->fault_policy.eligible_read_sequence, ==, 0);
    g_assert_false(core->fault_policy.fired);
}

static void test_command_and_system_reset_rearm_fault(void)
{
    MiniNandCore system_core;
    MiniNandCore command_core;

    mini_nand_core_init(&system_core, spy_flash_read,
                        spy_dma_write, &system_core,
                        (MiniNandFaultConfig){ .fail_nth = 3 });
    mini_nand_core_init(&command_core, spy_flash_read,
                        spy_dma_write, &command_core,
                        (MiniNandFaultConfig){ .fail_nth = 3 });
    assert_third_read_fault(&system_core);
    assert_third_read_fault(&command_core);

    mini_nand_core_reset(&system_core);
    g_assert_cmpint(mini_nand_core_write(&command_core,
                                         MINI_NAND_REG_COMMAND,
                                         MINI_NAND_CMD_RESET),
                    ==, MINI_NAND_ACCESS_OK);
    assert_fault_reset_state(&system_core);
    assert_fault_reset_state(&command_core);

    assert_third_read_fault(&system_core);
    assert_third_read_fault(&command_core);
}

static void test_qmp_fault_control_preserves_unrelated_snapshot(void)
{
    MiniNandCore core;
    MiniNandCoreSnapshot before;
    MiniNandCoreSnapshot after;

    mini_nand_core_init(&core, spy_flash_read, spy_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 3 });
    core.status = MINI_NAND_STATUS_DONE;
    core.error_code = MINI_NAND_ERR_NONE;
    core.read_count = 7;
    core.fault_count = 2;
    core.irq_status = MINI_NAND_IRQ_COMPLETE;
    core.irq_enable = MINI_NAND_IRQ_VALID_MASK;
    mini_nand_fault_policy_evaluate_read(&core.fault_policy);
    before = mini_nand_core_snapshot(&core);

    mini_nand_core_configure_fault_once(&core, 2);
    after = mini_nand_core_snapshot(&core);
    g_assert_true(after.fault.enabled);
    g_assert_cmpuint(after.fault.nth, ==, 2);
    g_assert_cmpuint(after.fault.eligible_sequence, ==, 0);
    g_assert_false(after.fault.fired);
    g_assert_cmpuint(after.status, ==, before.status);
    g_assert_cmpuint(after.error_code, ==, before.error_code);
    g_assert_cmpuint(after.read_count, ==, before.read_count);
    g_assert_cmpuint(after.fault_count, ==, before.fault_count);
    g_assert_cmpuint(after.irq_status, ==, before.irq_status);
    g_assert_cmpuint(after.irq_enable, ==, before.irq_enable);

    mini_nand_core_clear_fault(&core);
    after = mini_nand_core_snapshot(&core);
    g_assert_false(after.fault.enabled);
    g_assert_cmpuint(after.fault.nth, ==, 0);
    g_assert_cmpuint(after.fault.eligible_sequence, ==, 0);
    g_assert_false(after.fault.fired);
    g_assert_cmpuint(after.read_count, ==, before.read_count);
    g_assert_cmpuint(after.fault_count, ==, before.fault_count);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mini-nand-core/fault-rejections-preserve-state",
                    test_fault_rejections_do_not_consume_sequence);
    g_test_add_func("/mini-nand-core/fault-third-skip-fourth-normal",
                    test_third_read_fault_skips_flash_and_dma);
    g_test_add_func("/mini-nand-core/fault-dma-failure-consumes",
                    test_nonmatching_dma_failure_consumes_sequence);
    g_test_add_func("/mini-nand-core/fault-reset-rearm",
                    test_command_and_system_reset_rearm_fault);
    g_test_add_func("/mini-nand-core/qmp-control-snapshot",
                    test_qmp_fault_control_preserves_unrelated_snapshot);
    g_test_add_func("/mini-nand/core/dma-success-order-address",
                    test_dma_success_order_address);
    g_test_add_func("/mini-nand/core/dma-overflow-precedence",
                    test_dma_overflow_precedence);
    g_test_add_func("/mini-nand/core/dma-overflow-safe-boundary",
                    test_dma_overflow_safe_boundary);
    g_test_add_func("/mini-nand/core/dma-transaction-failure",
                    test_dma_transaction_failure);
    g_test_add_func("/mini-nand/core/dma-reset-preserves-seam",
                    test_dma_reset_preserves_seam);
    g_test_add_func("/mini-nand/core/dma-recovery-after-error",
                    test_dma_recovery_after_error);
    g_test_add_func("/mini-nand-core/construction-system-reset",
                    test_construction_and_system_reset);
    g_test_add_func("/mini-nand-core/register-dispositions",
                    test_register_dispositions);
    g_test_add_func("/mini-nand-core/irq-reset-mask-late-enable",
                    test_irq_reset_mask_and_late_enable);
    g_test_add_func("/mini-nand-core/irq-sticky-selective-w1c",
                    test_irq_sticky_selective_w1c_isolation);
    g_test_add_func("/mini-nand-core/valid-page-42-read",
                    test_valid_page_42_read_observes_busy_and_completes);
    g_test_add_func("/mini-nand-core/invalid-command",
                    test_invalid_command_preserves_valid_result_buffer);
    g_test_add_func("/mini-nand-core/invalid-page",
                    test_invalid_page_preserves_buffer_and_count);
    g_test_add_func("/mini-nand-core/invalid-length",
                    test_invalid_length_preserves_buffer_and_count);
    g_test_add_func("/mini-nand-core/both-invalid-page-precedence",
                    test_both_invalid_prefers_page_error);
    g_test_add_func("/mini-nand-core/recovery",
                    test_done_and_error_recover_with_valid_read);
    g_test_add_func("/mini-nand-core/command-reset",
                    test_command_reset_from_done_and_error);
    g_test_add_func("/mini-nand-core/reset-snapshot-equality",
                    test_system_and_command_reset_snapshots_match);
    g_test_add_func("/mini-nand-core/reset-preserves-backend",
                    test_reset_preserves_backend_and_flash_handle);
    g_test_add_func("/mini-nand-core/post-reset-read",
                    test_post_reset_valid_read_succeeds);
    return g_test_run();
}
