#include "qemu/osdep.h"
#include <glib/gstdio.h>

#include "hw/misc/mini-nand-core.h"
#include "hw/misc/mini-nand-mmio.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/rcu.h"

typedef struct AddressSpaceRwCapture {
    AddressSpace *address_space;
    hwaddr address;
    MemTxAttrs attrs;
    void *buffer;
    hwaddr length;
    bool is_write;
    unsigned int calls;
    MemTxResult result;
} AddressSpaceRwCapture;

static AddressSpaceRwCapture dma_capture;

MemTxResult address_space_rw(AddressSpace *as, hwaddr addr,
                             MemTxAttrs attrs, void *buf,
                             hwaddr len, bool is_write)
{
    dma_capture.address_space = as;
    dma_capture.address = addr;
    dma_capture.attrs = attrs;
    dma_capture.buffer = buf;
    dma_capture.length = len;
    dma_capture.is_write = is_write;
    dma_capture.calls++;
    return dma_capture.result;
}

typedef struct LogCapture {
    bool temp_created;
    bool log_configured;
    bool log_restored;
    bool file_read;
    bool file_removed;
    char *contents;
    char *error_message;
} LogCapture;

static bool test_dma_write(void *opaque, uint64_t address,
                           const uint8_t *source, size_t length)
{
    (void)opaque;
    (void)address;
    (void)source;
    (void)length;
    return true;
}

static void assert_dma_capture(AddressSpace *address_space,
                               uint64_t address,
                               const uint8_t *source)
{
    g_assert_cmpuint(dma_capture.calls, ==, 1);
    g_assert_true(dma_capture.address_space == address_space);
    g_assert_cmphex(dma_capture.address, ==, address);
    g_assert_true(dma_capture.buffer == source);
    g_assert_cmpmem(dma_capture.buffer, dma_capture.length,
                    source, MINI_NAND_FLASH_PAGE_SIZE);
    g_assert_cmpuint(dma_capture.length, ==, MINI_NAND_FLASH_PAGE_SIZE);
    g_assert_true(dma_capture.is_write);
    g_assert_true(dma_capture.attrs.unspecified);
    g_assert_false(dma_capture.attrs.secure);
    g_assert_cmpuint(dma_capture.attrs.space, ==, 0);
    g_assert_false(dma_capture.attrs.user);
    g_assert_false(dma_capture.attrs.memory);
    g_assert_false(dma_capture.attrs.debug);
    g_assert_cmpuint(dma_capture.attrs.requester_id, ==, 0);
    g_assert_cmpuint(dma_capture.attrs.pid, ==, 0);
    g_assert_cmpuint(dma_capture.attrs.address_type, ==, 0);
    g_assert_cmpuint(dma_capture.attrs._reserved1, ==, 0);
    g_assert_cmpuint(dma_capture.attrs._reserved2, ==, 0);
}

static void test_dma_adapter_arguments_and_results(void)
{
    static uint8_t source[MINI_NAND_FLASH_PAGE_SIZE];
    static AddressSpace address_space;
    static const MemTxResult failures[] = {
        MEMTX_ERROR,
        MEMTX_DECODE_ERROR,
    };
    const uint64_t address = UINT64_C(0x1234567881000000);

    for (size_t index = 0; index < sizeof(source); index++) {
        source[index] = (uint8_t)(index ^ 0xa5);
    }

    dma_capture = (AddressSpaceRwCapture) { .result = MEMTX_OK };
    g_assert_true(mini_nand_mmio_dma_write(&address_space, address,
                                           source, sizeof(source)));
    assert_dma_capture(&address_space, address, source);

    for (size_t index = 0; index < G_N_ELEMENTS(failures); index++) {
        dma_capture = (AddressSpaceRwCapture) { .result = failures[index] };
        g_assert_false(mini_nand_mmio_dma_write(&address_space, address,
                                                source, sizeof(source)));
        assert_dma_capture(&address_space, address, source);
    }
}

static void test_descriptor_contract(void)
{
    g_assert_cmpuint(MINI_NAND_MMIO_SIZE, ==, 0x1000);
    g_assert_cmpint(mini_nand_mmio_ops.endianness, ==,
                    DEVICE_LITTLE_ENDIAN);
    g_assert_cmpuint(mini_nand_mmio_ops.valid.min_access_size, ==, 4);
    g_assert_cmpuint(mini_nand_mmio_ops.valid.max_access_size, ==, 4);
    g_assert_false(mini_nand_mmio_ops.valid.unaligned);
    g_assert_cmpuint(mini_nand_mmio_ops.impl.min_access_size, ==, 4);
    g_assert_cmpuint(mini_nand_mmio_ops.impl.max_access_size, ==, 4);
    g_assert_false(mini_nand_mmio_ops.impl.unaligned);
}

static uint32_t adapter_read(MiniNandCore *core, hwaddr offset)
{
    return mini_nand_mmio_ops.read(core, offset, 4);
}

static void adapter_write(MiniNandCore *core, hwaddr offset, uint32_t value)
{
    mini_nand_mmio_ops.write(core, offset, value, 4);
}

static void assert_rejected_write_preserves_core(MiniNandCore *core,
                                                 hwaddr offset)
{
    MiniNandCore before = *core;

    adapter_write(core, offset, 0xa5a5a5a5);
    g_assert_cmpmem(core, sizeof(*core), &before, sizeof(before));
}

static void test_callbacks_delegate_and_preserve_rejected_writes(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, mini_nand_flash_read,
                        test_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    adapter_write(&core, MINI_NAND_REG_PAGE, 42);
    adapter_write(&core, MINI_NAND_REG_DMA_ADDR_LO, 0x81000000);
    adapter_write(&core, MINI_NAND_REG_DMA_ADDR_HI, 0x12345678);
    adapter_write(&core, MINI_NAND_REG_LENGTH, MINI_NAND_FLASH_PAGE_SIZE);

    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_VERSION), ==,
                     MINI_NAND_VERSION);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_COMMAND), ==, 0);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_PAGE), ==, 42);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_DMA_ADDR_LO), ==,
                     0x81000000);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_DMA_ADDR_HI), ==,
                     0x12345678);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_LENGTH), ==,
                     MINI_NAND_FLASH_PAGE_SIZE);

    adapter_write(&core, MINI_NAND_REG_COMMAND, MINI_NAND_CMD_READ);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_STATUS), ==,
                     MINI_NAND_STATUS_DONE);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_ERROR_CODE), ==,
                     MINI_NAND_ERR_NONE);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_READ_COUNT), ==, 1);
    g_assert_true(core.result_valid);
    g_assert_cmpuint(core.page_buffer[0], ==, 0x2a);
    g_assert_cmpuint(core.page_buffer[1], ==, 0x2b);
    g_assert_cmpuint(core.page_buffer[MINI_NAND_FLASH_PAGE_SIZE - 1], ==,
                     0x29);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_COMMAND), ==, 0);
    g_assert_cmpuint(adapter_read(&core, MINI_NAND_REG_IRQ_STATUS), ==, 0);
    g_assert_cmpuint(adapter_read(&core, 0x30), ==, 0);

    assert_rejected_write_preserves_core(&core, MINI_NAND_REG_VERSION);
    assert_rejected_write_preserves_core(&core, MINI_NAND_REG_IRQ_STATUS);
    assert_rejected_write_preserves_core(&core, 0x30);
}

static void exercise_log_contract(void)
{
    MiniNandCore core;

    mini_nand_core_init(&core, mini_nand_flash_read,
                        test_dma_write, &core,
                        (MiniNandFaultConfig){ .fail_nth = 0 });
    adapter_write(&core, MINI_NAND_REG_PAGE, 42);
    adapter_read(&core, MINI_NAND_REG_VERSION);
    adapter_read(&core, MINI_NAND_REG_COMMAND);
    adapter_read(&core, MINI_NAND_REG_PAGE);
    adapter_read(&core, MINI_NAND_REG_IRQ_STATUS);
    adapter_read(&core, 0x30);
    adapter_write(&core, MINI_NAND_REG_VERSION, 1);
    adapter_write(&core, MINI_NAND_REG_IRQ_STATUS, 1);
    adapter_write(&core, 0x30, 1);
}

static void save_error_message(LogCapture *capture, const char *message)
{
    if (!capture->error_message && message) {
        capture->error_message = g_strdup(message);
    }
}

static LogCapture capture_adapter_log(int mask)
{
    LogCapture capture = { 0 };
    g_autofree char *path = NULL;
    Error *configure_error = NULL;
    Error *restore_error = NULL;
    GError *file_error = NULL;
    int fd;

    fd = g_file_open_tmp("mini-nand-mmio-log.XXXXXX", &path, &file_error);
    if (fd >= 0) {
        capture.temp_created = close(fd) == 0;
    } else {
        save_error_message(&capture, file_error->message);
        g_clear_error(&file_error);
    }

    if (capture.temp_created) {
        capture.log_configured = qemu_set_log_filename_flags(
            path, mask, &configure_error);
        if (capture.log_configured) {
            exercise_log_contract();
        }
    }
    if (configure_error) {
        save_error_message(&capture, error_get_pretty(configure_error));
        error_free(configure_error);
    }

    /*
     * 전역 logger를 먼저 원복하고 RCU close까지 끝낸다.
     * 이후 assertion이 실패해도
     * 다음 test에 설정이 새지 않는다.
     */
    capture.log_restored = qemu_set_log_filename_flags(
        NULL, 0, &restore_error);
    drain_call_rcu();
    if (restore_error) {
        save_error_message(&capture, error_get_pretty(restore_error));
        error_free(restore_error);
    }

    if (path) {
        capture.file_read = g_file_get_contents(path, &capture.contents,
                                                NULL, &file_error);
        if (file_error) {
            save_error_message(&capture, file_error->message);
            g_clear_error(&file_error);
        }
        capture.file_removed = g_remove(path) == 0;
    }
    return capture;
}

static void assert_capture_lifecycle(const LogCapture *capture)
{
    if (capture->error_message) {
        g_test_message("log capture error: %s", capture->error_message);
    }
    g_assert_true(capture->temp_created);
    g_assert_true(capture->log_configured);
    g_assert_true(capture->log_restored);
    g_assert_true(capture->file_read);
    g_assert_true(capture->file_removed);
}

static void clear_log_capture(LogCapture *capture)
{
    g_free(capture->contents);
    g_free(capture->error_message);
}

static void test_rejected_write_logging_contract(void)
{
    static const char expected_guest_log[] =
        "mini-nand-mmio: rejected write offset=0x0 class=read-only\n"
        "mini-nand-mmio: rejected write offset=0x28 class=reserved\n"
        "mini-nand-mmio: rejected write offset=0x30 class=undefined\n";
    LogCapture guest = capture_adapter_log(LOG_GUEST_ERROR);
    LogCapture unimp = capture_adapter_log(LOG_UNIMP);

    assert_capture_lifecycle(&guest);
    assert_capture_lifecycle(&unimp);
    g_assert_cmpstr(guest.contents, ==, expected_guest_log);
    g_assert_cmpstr(unimp.contents, ==, "");
    clear_log_capture(&guest);
    clear_log_capture(&unimp);
}

static void test_mmio_range_contract(void)
{
    const MemMapEntry non_overlap[] = {
        { .base = 0x1000, .size = 0x100 },
        { .base = 0x2000, .size = 0x100 },
    };
    const MemMapEntry left_touch[] = {
        { .base = 0xf00, .size = 0x100 },
        { .base = 0x1000, .size = 0x100 },
    };
    const MemMapEntry right_touch[] = {
        { .base = 0x1000, .size = 0x100 },
        { .base = 0x1100, .size = 0x100 },
    };
    const MemMapEntry overlap[] = {
        { .base = 0x1000, .size = 0x100 },
        { .base = 0x10ff, .size = 0x100 },
    };
    const MemMapEntry zero_self[] = {
        { .base = 0x1000, .size = 0 },
        { .base = 0x2000, .size = 0x100 },
    };
    const MemMapEntry zero_peer[] = {
        { .base = 0x1000, .size = 0x100 },
        { .base = 0x1000, .size = 0 },
    };
    const MemMapEntry wrapping_self[] = {
        { .base = UINT64_MAX - 7, .size = 16 },
        { .base = 0x1000, .size = 0x100 },
    };
    const MemMapEntry wrapping_peer[] = {
        { .base = 0x1000, .size = 0x100 },
        { .base = UINT64_MAX - 7, .size = 16 },
    };

    g_assert_true(mini_nand_mmio_range_is_free(
        non_overlap, G_N_ELEMENTS(non_overlap), 0));
    g_assert_true(mini_nand_mmio_range_is_free(
        left_touch, G_N_ELEMENTS(left_touch), 1));
    g_assert_true(mini_nand_mmio_range_is_free(
        right_touch, G_N_ELEMENTS(right_touch), 0));
    g_assert_false(mini_nand_mmio_range_is_free(
        overlap, G_N_ELEMENTS(overlap), 0));
    g_assert_false(mini_nand_mmio_range_is_free(
        zero_self, G_N_ELEMENTS(zero_self), 0));
    g_assert_true(mini_nand_mmio_range_is_free(
        zero_peer, G_N_ELEMENTS(zero_peer), 0));
    g_assert_false(mini_nand_mmio_range_is_free(
        wrapping_self, G_N_ELEMENTS(wrapping_self), 0));
    g_assert_false(mini_nand_mmio_range_is_free(
        wrapping_peer, G_N_ELEMENTS(wrapping_peer), 0));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mini-nand-ctrl/dma-adapter",
                    test_dma_adapter_arguments_and_results);
    g_test_add_func("/mini-nand-ctrl/descriptor",
                    test_descriptor_contract);
    g_test_add_func("/mini-nand-ctrl/callbacks",
                    test_callbacks_delegate_and_preserve_rejected_writes);
    g_test_add_func("/mini-nand-ctrl/logging",
                    test_rejected_write_logging_contract);
    g_test_add_func("/mini-nand-ctrl/range",
                    test_mmio_range_contract);
    return g_test_run();
}
