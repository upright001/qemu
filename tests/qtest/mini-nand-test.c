/*
 * Mini NAND controller의 firmware-visible 계약을 검증하는 QTest
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "libqtest.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qjson.h"

#define MINI_NAND_BASE              0x10110000ULL
#define MINI_NAND_DMA               0x81000000ULL
#define MINI_NAND_BAD_DMA           0x08000000ULL
#define MINI_NAND_GUARD_SIZE        64U
#define MINI_NAND_PAGE              42U
#define MINI_NAND_LENGTH            4096U

#define MINI_NAND_VERSION           0x00010000U
#define MINI_NAND_CMD_READ          0x00000001U
#define MINI_NAND_CMD_RESET         0x000000ffU
#define MINI_NAND_STATUS_IDLE       0U
#define MINI_NAND_STATUS_DONE       2U
#define MINI_NAND_STATUS_ERROR      3U
#define MINI_NAND_ERR_NONE          0U
#define MINI_NAND_ERR_INVALID_CMD   1U
#define MINI_NAND_ERR_INVALID_PAGE  2U
#define MINI_NAND_ERR_INVALID_LEN   3U
#define MINI_NAND_ERR_DMA           4U
#define MINI_NAND_ERR_UNCORRECTABLE 5U
#define MINI_NAND_IRQ_COMPLETE      (1U << 0)
#define MINI_NAND_IRQ_ERROR         (1U << 1)
#define MINI_NAND_IRQ_VALID_MASK \
    (MINI_NAND_IRQ_COMPLETE | MINI_NAND_IRQ_ERROR)

enum {
    REG_VERSION = 0x00,
    REG_COMMAND = 0x04,
    REG_PAGE = 0x08,
    REG_DMA_ADDR_LO = 0x0c,
    REG_DMA_ADDR_HI = 0x10,
    REG_LENGTH = 0x14,
    REG_STATUS = 0x18,
    REG_ERROR_CODE = 0x1c,
    REG_READ_COUNT = 0x20,
    REG_FAULT_COUNT = 0x24,
    REG_IRQ_STATUS = 0x28,
    REG_IRQ_ENABLE = 0x2c,
};

static const uint32_t snapshot_offsets[] = {
    REG_VERSION,
    REG_COMMAND,
    REG_PAGE,
    REG_DMA_ADDR_LO,
    REG_DMA_ADDR_HI,
    REG_LENGTH,
    REG_STATUS,
    REG_ERROR_CODE,
    REG_READ_COUNT,
    REG_FAULT_COUNT,
    REG_IRQ_STATUS,
    REG_IRQ_ENABLE,
};

typedef struct MiniNandSnapshot {
    uint32_t regs[G_N_ELEMENTS(snapshot_offsets)];
    bool irq_level;
    uint8_t guard_before[MINI_NAND_GUARD_SIZE];
    uint8_t dma[MINI_NAND_LENGTH];
    uint8_t guard_after[MINI_NAND_GUARD_SIZE];
} MiniNandSnapshot;

static void mini_nand_drain_pipe(int fd, GString *output)
{
    char buffer[4096];
    ssize_t length;

    for (;;) {
        length = read(fd, buffer, sizeof(buffer));
        if (length > 0) {
            g_string_append_len(output, buffer, length);
            continue;
        }
        if (length == 0) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
}

static bool mini_nand_wait_child(GPid pid, int stdout_fd, int stderr_fd,
                                 GString *stdout_output,
                                 GString *stderr_output,
                                 gint64 deadline_us, gint *wait_status)
{
    pid_t waited;

    for (;;) {
        mini_nand_drain_pipe(stdout_fd, stdout_output);
        mini_nand_drain_pipe(stderr_fd, stderr_output);

        do {
            waited = waitpid(pid, wait_status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        g_assert_cmpint(waited, >=, 0);
        if (waited == pid) {
            return true;
        }
        if (g_get_monotonic_time() >= deadline_us) {
            return false;
        }
        g_usleep(10 * 1000);
    }
}

static bool mini_nand_run_child_bounded(const gchar *const *argv,
                                        gint *wait_status,
                                        bool *timed_out,
                                        gchar **stdout_text,
                                        gchar **stderr_text,
                                        GError **error)
{
    GString *stdout_output = g_string_new(NULL);
    GString *stderr_output = g_string_new(NULL);
    GPid pid;
    int stdout_fd;
    int stderr_fd;
    int flags;
    bool reaped;
    pid_t waited;

    if (!g_spawn_async_with_pipes(NULL, (gchar **)argv, NULL,
                                  G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                  &pid, NULL, &stdout_fd, &stderr_fd, error)) {
        g_string_free(stdout_output, true);
        g_string_free(stderr_output, true);
        return false;
    }

    flags = fcntl(stdout_fd, F_GETFL);
    g_assert_cmpint(flags, >=, 0);
    g_assert_cmpint(fcntl(stdout_fd, F_SETFL, flags | O_NONBLOCK), ==, 0);
    flags = fcntl(stderr_fd, F_GETFL);
    g_assert_cmpint(flags, >=, 0);
    g_assert_cmpint(fcntl(stderr_fd, F_SETFL, flags | O_NONBLOCK), ==, 0);

    *timed_out = false;
    reaped = mini_nand_wait_child(pid, stdout_fd, stderr_fd,
                                  stdout_output, stderr_output,
                                  g_get_monotonic_time() + 3 * G_USEC_PER_SEC,
                                  wait_status);
    if (!reaped) {
        int kill_result;

        *timed_out = true;
        kill_result = kill(pid, SIGTERM);
        g_assert_true(kill_result == 0 || errno == ESRCH);
        reaped = mini_nand_wait_child(
            pid, stdout_fd, stderr_fd, stdout_output, stderr_output,
            g_get_monotonic_time() + G_USEC_PER_SEC, wait_status);
        if (!reaped) {
            kill_result = kill(pid, SIGKILL);
            g_assert_true(kill_result == 0 || errno == ESRCH);
            do {
                waited = waitpid(pid, wait_status, 0);
            } while (waited < 0 && errno == EINTR);
            g_assert_cmpint(waited, ==, pid);
        }
    }

    mini_nand_drain_pipe(stdout_fd, stdout_output);
    mini_nand_drain_pipe(stderr_fd, stderr_output);
    close(stdout_fd);
    close(stderr_fd);
    g_spawn_close_pid(pid);
    *stdout_text = g_string_free(stdout_output, false);
    *stderr_text = g_string_free(stderr_output, false);
    return true;
}

static QTestState *mini_nand_qtest_start(bool enabled, uint32_t fail_nth)
{
    return qtest_initf("-M virt,aia=none,mini-nand=%s,"
                       "mini-nand-fail-nth=%" PRIu32 " "
                       "-smp 1 -m 128M -bios none",
                       enabled ? "on" : "off", fail_nth);
}

static QTestState *mini_nand_qtest_start_prelaunch(void)
{
    QTestState *qts = qtest_initf(
        "-S -M virt,aia=none,mini-nand=on,mini-nand-fail-nth=0 "
        "-smp 1 -m 128M -bios none");
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'query-status' }");
    QDict *status = qdict_get_qdict(response, "return");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "prelaunch");
    return qts;
}

static void mini_nand_qmp_expect_success(QTestState *qts, const char *command)
{
    g_autoptr(QDict) response = qtest_qmp(
        qts, "%p", qobject_from_json(command, &error_abort));

    if (!qdict_haskey(response, "return")) {
        QDict *error = qdict_get_qdict(response, "error");
        g_test_message("QMP failure for %s: %s", command,
                       error ? qdict_get_str(error, "desc") : "missing error");
    }
    g_assert(qdict_haskey(response, "return"));
}

static void mini_nand_qmp_expect_error(QTestState *qts, const char *command,
                                       const char *description)
{
    g_autoptr(QDict) response = qtest_qmp(
        qts, "%p", qobject_from_json(command, &error_abort));
    QDict *error = qdict_get_qdict(response, "error");

    g_assert_nonnull(error);
    g_assert_cmpstr(qdict_get_str(error, "desc"), ==, description);
}

static void mini_nand_qmp_expect_any_error(QTestState *qts, const char *command)
{
    g_autoptr(QDict) response = qtest_qmp(
        qts, "%p", qobject_from_json(command, &error_abort));

    g_assert_nonnull(qdict_get_qdict(response, "error"));
}

static void mini_nand_qmp_expect_status(QTestState *qts, const char *expected)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'query-status' }");

    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(response, "return"), "status"),
                    ==, expected);
}

static void mini_nand_qmp_assert_snapshot(QTestState *qts, bool enabled,
                                          uint32_t nth, uint32_t sequence,
                                          bool fired)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'x-query-mini-nand' }");
    QDict *info = qdict_get_qdict(response, "return");

    g_assert_cmpuint(qdict_size(info), ==, 11);
    g_assert_cmpint(qdict_get_bool(info, "fault-enabled"), ==, enabled);
    g_assert_cmpuint(qdict_get_uint(info, "nth"), ==, nth);
    g_assert_true(qdict_get_bool(info, "once"));
    g_assert_cmpuint(qdict_get_uint(info, "eligible-sequence"), ==, sequence);
    g_assert_cmpint(qdict_get_bool(info, "fired"), ==, fired);
    g_assert_cmpstr(qdict_get_str(info, "status"), ==, "idle");
    g_assert_cmpstr(qdict_get_str(info, "error"), ==, "none");
    g_assert_cmpuint(qdict_get_uint(info, "read-count"), ==, 0);
    g_assert_cmpuint(qdict_get_uint(info, "fault-count"), ==, 0);
    g_assert_cmpuint(qdict_get_uint(info, "irq-status"), ==, 0);
    g_assert_cmpuint(qdict_get_uint(info, "irq-enable"), ==, 0);
}

static uint32_t mini_nand_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, MINI_NAND_BASE + offset);
}

static void mini_nand_writel(QTestState *qts, uint32_t offset,
                             uint32_t value)
{
    qtest_writel(qts, MINI_NAND_BASE + offset, value);
}

static void mini_nand_intercept_irq(QTestState *qts)
{
    qtest_irq_intercept_out_named(qts, "/machine/mini-nand-ctrl",
                                  "sysbus-irq");
    g_assert_false(qtest_get_irq(qts, 0));
}

static void mini_nand_expect_irq(QTestState *qts, bool expected)
{
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, expected);
}

static void mini_nand_enable_irq(QTestState *qts, uint32_t mask)
{
    mini_nand_writel(qts, REG_IRQ_ENABLE, mask);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_ENABLE), ==,
                    mask & MINI_NAND_IRQ_VALID_MASK);
}

static void mini_nand_program_read(QTestState *qts, uint64_t dma,
                                   uint32_t page, uint32_t length)
{
    mini_nand_writel(qts, REG_PAGE, page);
    mini_nand_writel(qts, REG_DMA_ADDR_LO, dma);
    mini_nand_writel(qts, REG_DMA_ADDR_HI, dma >> 32);
    mini_nand_writel(qts, REG_LENGTH, length);
}

static void mini_nand_command(QTestState *qts, uint32_t command)
{
    mini_nand_writel(qts, REG_COMMAND, command);
}

static void mini_nand_read(QTestState *qts, uint64_t dma)
{
    mini_nand_program_read(qts, dma, MINI_NAND_PAGE, MINI_NAND_LENGTH);
    mini_nand_command(qts, MINI_NAND_CMD_READ);
}

static void mini_nand_fill_memory(QTestState *qts, uint8_t dma_value)
{
    uint8_t guard_before[MINI_NAND_GUARD_SIZE];
    uint8_t dma[MINI_NAND_LENGTH];
    uint8_t guard_after[MINI_NAND_GUARD_SIZE];

    memset(guard_before, 0x6b, sizeof(guard_before));
    memset(dma, dma_value, sizeof(dma));
    memset(guard_after, 0xb6, sizeof(guard_after));
    qtest_bufwrite(qts, MINI_NAND_DMA - MINI_NAND_GUARD_SIZE,
                   guard_before, sizeof(guard_before));
    qtest_bufwrite(qts, MINI_NAND_DMA, dma, sizeof(dma));
    qtest_bufwrite(qts, MINI_NAND_DMA + MINI_NAND_LENGTH,
                   guard_after, sizeof(guard_after));
}

static void mini_nand_expect_memory_value(QTestState *qts,
                                          uint8_t dma_value)
{
    uint8_t guard_before[MINI_NAND_GUARD_SIZE];
    uint8_t dma[MINI_NAND_LENGTH];
    uint8_t guard_after[MINI_NAND_GUARD_SIZE];

    qtest_bufread(qts, MINI_NAND_DMA - MINI_NAND_GUARD_SIZE,
                  guard_before, sizeof(guard_before));
    qtest_bufread(qts, MINI_NAND_DMA, dma, sizeof(dma));
    qtest_bufread(qts, MINI_NAND_DMA + MINI_NAND_LENGTH,
                  guard_after, sizeof(guard_after));
    for (size_t i = 0; i < sizeof(guard_before); i++) {
        g_assert_cmphex(guard_before[i], ==, 0x6b);
    }
    for (size_t i = 0; i < sizeof(dma); i++) {
        g_assert_cmphex(dma[i], ==, dma_value);
    }
    for (size_t i = 0; i < sizeof(guard_after); i++) {
        g_assert_cmphex(guard_after[i], ==, 0xb6);
    }
}

static void mini_nand_expect_page_pattern(QTestState *qts)
{
    uint8_t dma[MINI_NAND_LENGTH];

    qtest_bufread(qts, MINI_NAND_DMA, dma, sizeof(dma));
    for (size_t offset = 0; offset < sizeof(dma); offset++) {
        g_assert_cmphex(dma[offset], ==, (MINI_NAND_PAGE + offset) & 0xff);
    }
}

static void mini_nand_snapshot(QTestState *qts, MiniNandSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    for (size_t i = 0; i < G_N_ELEMENTS(snapshot_offsets); i++) {
        snapshot->regs[i] = mini_nand_readl(qts, snapshot_offsets[i]);
    }
    snapshot->irq_level = qtest_get_irq(qts, 0);
    qtest_bufread(qts, MINI_NAND_DMA - MINI_NAND_GUARD_SIZE,
                  snapshot->guard_before, sizeof(snapshot->guard_before));
    qtest_bufread(qts, MINI_NAND_DMA, snapshot->dma,
                  sizeof(snapshot->dma));
    qtest_bufread(qts, MINI_NAND_DMA + MINI_NAND_LENGTH,
                  snapshot->guard_after, sizeof(snapshot->guard_after));
}

static void mini_nand_assert_snapshot_equal(const MiniNandSnapshot *left,
                                            const MiniNandSnapshot *right)
{
    g_assert_cmpint(memcmp(left, right, sizeof(*left)), ==, 0);
}

static bool mini_nand_machine_has_child(QTestState *qts,
                                        const char *expected_name)
{
    g_autoptr(QDict) response = NULL;
    QList *children;
    QListEntry *entry;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-list',"
                         "  'arguments': { 'path': '/machine' } }");
    g_assert(qdict_haskey(response, "return"));
    children = qobject_to(QList, qdict_get(response, "return"));
    QLIST_FOREACH_ENTRY(children, entry) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(child, "name"), expected_name)) {
            return true;
        }
    }
    return false;
}

static void test_map_off_on(void)
{
    QTestState *qts = mini_nand_qtest_start(false, 0);

    g_assert_false(mini_nand_machine_has_child(qts, "mini-nand-ctrl"));
    g_assert_cmphex(qtest_readl(qts, MINI_NAND_BASE), !=, MINI_NAND_VERSION);
    qtest_quit(qts);

    qts = mini_nand_qtest_start(true, 0);
    g_assert_true(mini_nand_machine_has_child(qts, "mini-nand-ctrl"));
    g_assert_cmphex(mini_nand_readl(qts, REG_VERSION), ==, MINI_NAND_VERSION);
    mini_nand_intercept_irq(qts);
    qtest_quit(qts);
}

static void test_map_aia_unsupported(void)
{
    const char *qemu = g_getenv("QTEST_QEMU_BINARY");
    const gchar *argv[] = {
        qemu,
        "-M", "virt,aia=aplic,mini-nand=on",
        "-accel", "qtest",
        "-smp", "1",
        "-m", "128M",
        "-bios", "none",
        "-display", "none",
        NULL,
    };
    g_autofree gchar *stdout_text = NULL;
    g_autofree gchar *stderr_text = NULL;
    g_autofree gchar *qemu_basename = NULL;
    g_autofree gchar *expected_stderr = NULL;
    g_autoptr(GError) error = NULL;
    gint wait_status;
    bool timed_out;

    g_assert_nonnull(qemu);
    qemu_basename = g_path_get_basename(qemu);
    expected_stderr = g_strdup_printf(
        "%s: mini-nand requires aia=none\n", qemu_basename);
    /* 잘못된 machine 조합이 종료하지 않더라도 test suite가 무한 대기하지 않는다. */
    g_assert_true(mini_nand_run_child_bounded(argv, &wait_status,
                                              &timed_out, &stdout_text,
                                              &stderr_text, &error));
    g_assert_no_error(error);
    g_assert_false(timed_out);
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), !=, 0);
    g_assert_cmpstr(stdout_text, ==, "");
    g_assert_cmpstr(stderr_text, ==, expected_stderr);
}

static void test_register_reset_access(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);
    MiniNandSnapshot before;
    MiniNandSnapshot after;

    mini_nand_intercept_irq(qts);
    g_assert_cmphex(mini_nand_readl(qts, REG_VERSION), ==, MINI_NAND_VERSION);
    g_assert_cmphex(mini_nand_readl(qts, REG_COMMAND), ==, 0);
    for (uint32_t offset = REG_PAGE; offset <= REG_IRQ_ENABLE; offset += 4) {
        g_assert_cmphex(mini_nand_readl(qts, offset), ==, 0);
    }

    /* COMMAND 없이 staging register가 독립적으로 보존되는 계약을 확인한다. */
    mini_nand_writel(qts, REG_PAGE, 43);
    mini_nand_writel(qts, REG_DMA_ADDR_LO, 0x12345678);
    mini_nand_writel(qts, REG_DMA_ADDR_HI, 0x9abcdef0);
    mini_nand_writel(qts, REG_LENGTH, MINI_NAND_LENGTH);
    g_assert_cmphex(mini_nand_readl(qts, REG_PAGE), ==, 43);
    g_assert_cmphex(mini_nand_readl(qts, REG_DMA_ADDR_LO), ==, 0x12345678);
    g_assert_cmphex(mini_nand_readl(qts, REG_DMA_ADDR_HI), ==, 0x9abcdef0);
    g_assert_cmphex(mini_nand_readl(qts, REG_LENGTH), ==,
                    MINI_NAND_LENGTH);

    mini_nand_fill_memory(qts, 0xa5);
    mini_nand_enable_irq(qts, UINT32_MAX);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_ENABLE), ==,
                    MINI_NAND_IRQ_VALID_MASK);
    mini_nand_snapshot(qts, &before);

    mini_nand_writel(qts, REG_VERSION, 0xdeadbeef);
    g_assert_cmphex(mini_nand_readl(qts, REG_VERSION), ==, MINI_NAND_VERSION);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);

    mini_nand_writel(qts, 0x30, 0xdeadbeef);
    g_assert_cmphex(mini_nand_readl(qts, 0x30), ==, 0);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);

    mini_nand_writel(qts, REG_IRQ_STATUS, UINT32_MAX & ~MINI_NAND_IRQ_VALID_MASK);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==, 0);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);
    mini_nand_expect_irq(qts, false);
    qtest_quit(qts);
}

static void test_register_invalid_access_preserves_state(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);
    MiniNandSnapshot before;
    MiniNandSnapshot after;

    mini_nand_intercept_irq(qts);
    mini_nand_fill_memory(qts, 0xa5);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_COMPLETE);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_expect_irq(qts, true);
    mini_nand_snapshot(qts, &before);

    (void)qtest_readb(qts, MINI_NAND_BASE + REG_PAGE);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);
    qtest_writeb(qts, MINI_NAND_BASE + REG_PAGE, 43);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);
    (void)qtest_readw(qts, MINI_NAND_BASE + REG_PAGE);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);
    qtest_writew(qts, MINI_NAND_BASE + REG_PAGE, 43);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);
    (void)qtest_readl(qts, MINI_NAND_BASE + REG_PAGE + 1);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);
    qtest_writel(qts, MINI_NAND_BASE + REG_PAGE + 1, 43);
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);

    /* Valid 32-bit control은 같은 대상 register를 실제로 바꿔 test를 비자명하게 한다. */
    mini_nand_writel(qts, REG_PAGE, 43);
    mini_nand_snapshot(qts, &after);
    g_assert_cmpint(memcmp(&before, &after, sizeof(before)), !=, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_PAGE), ==, 43);
    qtest_quit(qts);
}

static void test_irq_event_before_enable(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);

    mini_nand_intercept_irq(qts);
    mini_nand_read(qts, MINI_NAND_DMA);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_COMPLETE);
    mini_nand_expect_irq(qts, false);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_COMPLETE);
    mini_nand_expect_irq(qts, true);
    qtest_quit(qts);
}

static void test_irq_enable_before_event(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);

    mini_nand_intercept_irq(qts);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_COMPLETE);
    mini_nand_expect_irq(qts, false);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_expect_irq(qts, true);
    qtest_quit(qts);
}

static void test_irq_disable_preserves_status(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);

    mini_nand_intercept_irq(qts);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_COMPLETE);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_expect_irq(qts, true);
    mini_nand_enable_irq(qts, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_COMPLETE);
    mini_nand_expect_irq(qts, false);
    qtest_quit(qts);
}

static void test_read_complete_dma(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);
    uint8_t guard_before[MINI_NAND_GUARD_SIZE];
    uint8_t guard_after[MINI_NAND_GUARD_SIZE];

    mini_nand_intercept_irq(qts);
    mini_nand_fill_memory(qts, 0xa5);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_COMPLETE);
    mini_nand_expect_irq(qts, false);
    mini_nand_read(qts, MINI_NAND_DMA);
    g_assert_cmphex(mini_nand_readl(qts, REG_STATUS), ==,
                    MINI_NAND_STATUS_DONE);
    g_assert_cmphex(mini_nand_readl(qts, REG_ERROR_CODE), ==,
                    MINI_NAND_ERR_NONE);
    g_assert_cmphex(mini_nand_readl(qts, REG_READ_COUNT), ==, 1);
    g_assert_cmphex(mini_nand_readl(qts, REG_FAULT_COUNT), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_COMPLETE);
    mini_nand_expect_irq(qts, true);
    mini_nand_expect_page_pattern(qts);
    qtest_bufread(qts, MINI_NAND_DMA - MINI_NAND_GUARD_SIZE,
                  guard_before, sizeof(guard_before));
    qtest_bufread(qts, MINI_NAND_DMA + MINI_NAND_LENGTH,
                  guard_after, sizeof(guard_after));
    for (size_t i = 0; i < sizeof(guard_before); i++) {
        g_assert_cmphex(guard_before[i], ==, 0x6b);
        g_assert_cmphex(guard_after[i], ==, 0xb6);
    }
    qtest_quit(qts);
}

typedef void (*MiniNandRejectSetup)(QTestState *qts);

static void mini_nand_reject_invalid_command(QTestState *qts)
{
    mini_nand_program_read(qts, MINI_NAND_DMA, MINI_NAND_PAGE,
                           MINI_NAND_LENGTH);
    mini_nand_command(qts, 0x00000002);
}

static void mini_nand_reject_invalid_page(QTestState *qts)
{
    mini_nand_program_read(qts, MINI_NAND_DMA, 64, MINI_NAND_LENGTH);
    mini_nand_command(qts, MINI_NAND_CMD_READ);
}

static void mini_nand_reject_invalid_length(QTestState *qts)
{
    mini_nand_program_read(qts, MINI_NAND_DMA, MINI_NAND_PAGE,
                           MINI_NAND_LENGTH - 1);
    mini_nand_command(qts, MINI_NAND_CMD_READ);
}

static void mini_nand_reject_dma_overflow(QTestState *qts)
{
    mini_nand_program_read(qts, UINT64_C(0xfffffffffffff000),
                           MINI_NAND_PAGE, MINI_NAND_LENGTH);
    mini_nand_command(qts, MINI_NAND_CMD_READ);
}

static void mini_nand_test_rejection(MiniNandRejectSetup setup,
                                     uint32_t expected_error)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);

    mini_nand_intercept_irq(qts);
    mini_nand_fill_memory(qts, 0xa5);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_VALID_MASK);
    mini_nand_expect_irq(qts, false);
    setup(qts);
    g_assert_cmphex(mini_nand_readl(qts, REG_STATUS), ==,
                    MINI_NAND_STATUS_ERROR);
    g_assert_cmphex(mini_nand_readl(qts, REG_ERROR_CODE), ==,
                    expected_error);
    g_assert_cmphex(mini_nand_readl(qts, REG_READ_COUNT), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_FAULT_COUNT), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==, 0);
    mini_nand_expect_irq(qts, false);
    mini_nand_expect_memory_value(qts, 0xa5);
    qtest_quit(qts);
}

static void test_reject_invalid_command(void)
{
    mini_nand_test_rejection(mini_nand_reject_invalid_command,
                             MINI_NAND_ERR_INVALID_CMD);
}

static void test_reject_invalid_page(void)
{
    mini_nand_test_rejection(mini_nand_reject_invalid_page,
                             MINI_NAND_ERR_INVALID_PAGE);
}

static void test_reject_invalid_length(void)
{
    mini_nand_test_rejection(mini_nand_reject_invalid_length,
                             MINI_NAND_ERR_INVALID_LEN);
}

static void test_reject_dma_overflow(void)
{
    mini_nand_test_rejection(mini_nand_reject_dma_overflow,
                             MINI_NAND_ERR_DMA);
}

static void test_error_dma_transaction(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);

    mini_nand_intercept_irq(qts);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_ERROR);
    mini_nand_expect_irq(qts, false);
    mini_nand_read(qts, MINI_NAND_BAD_DMA);
    g_assert_cmphex(mini_nand_readl(qts, REG_STATUS), ==,
                    MINI_NAND_STATUS_ERROR);
    g_assert_cmphex(mini_nand_readl(qts, REG_ERROR_CODE), ==,
                    MINI_NAND_ERR_DMA);
    g_assert_cmphex(mini_nand_readl(qts, REG_READ_COUNT), ==, 1);
    g_assert_cmphex(mini_nand_readl(qts, REG_FAULT_COUNT), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_ERROR);
    mini_nand_expect_irq(qts, true);
    qtest_quit(qts);
}

static void mini_nand_run_to_third_fault(QTestState *qts)
{
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_fill_memory(qts, 0xa5);
    mini_nand_read(qts, MINI_NAND_DMA);
}

static void mini_nand_expect_third_fault(QTestState *qts)
{
    g_assert_cmphex(mini_nand_readl(qts, REG_STATUS), ==,
                    MINI_NAND_STATUS_ERROR);
    g_assert_cmphex(mini_nand_readl(qts, REG_ERROR_CODE), ==,
                    MINI_NAND_ERR_UNCORRECTABLE);
    g_assert_cmphex(mini_nand_readl(qts, REG_READ_COUNT), ==, 3);
    g_assert_cmphex(mini_nand_readl(qts, REG_FAULT_COUNT), ==, 1);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_VALID_MASK);
    mini_nand_expect_irq(qts, true);
    mini_nand_expect_memory_value(qts, 0xa5);
}

static void test_error_third_read_fault(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 3);

    mini_nand_intercept_irq(qts);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_ERROR);
    mini_nand_expect_irq(qts, false);
    mini_nand_run_to_third_fault(qts);
    mini_nand_expect_third_fault(qts);
    qtest_quit(qts);
}

static void test_irq_selective_w1c(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 3);
    MiniNandSnapshot before;
    MiniNandSnapshot after;

    mini_nand_intercept_irq(qts);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_VALID_MASK);
    mini_nand_run_to_third_fault(qts);
    mini_nand_expect_irq(qts, true);
    mini_nand_snapshot(qts, &before);

    mini_nand_writel(qts, REG_IRQ_STATUS, MINI_NAND_IRQ_COMPLETE);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_ERROR);
    mini_nand_expect_irq(qts, true);
    mini_nand_snapshot(qts, &after);
    before.regs[10] = MINI_NAND_IRQ_ERROR;
    mini_nand_assert_snapshot_equal(&before, &after);

    mini_nand_writel(qts, REG_IRQ_STATUS, MINI_NAND_IRQ_ERROR);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==, 0);
    mini_nand_expect_irq(qts, false);
    qtest_quit(qts);
}

static void mini_nand_expect_reset_state(QTestState *qts)
{
    g_assert_cmphex(mini_nand_readl(qts, REG_PAGE), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_DMA_ADDR_LO), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_DMA_ADDR_HI), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_LENGTH), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_STATUS), ==,
                    MINI_NAND_STATUS_IDLE);
    g_assert_cmphex(mini_nand_readl(qts, REG_ERROR_CODE), ==,
                    MINI_NAND_ERR_NONE);
    g_assert_cmphex(mini_nand_readl(qts, REG_READ_COUNT), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_FAULT_COUNT), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==, 0);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_ENABLE), ==, 0);
    mini_nand_expect_irq(qts, false);
}

static void mini_nand_seed_pending_reset(QTestState *qts)
{
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_VALID_MASK);
    mini_nand_run_to_third_fault(qts);
    mini_nand_expect_third_fault(qts);
}

static void mini_nand_verify_reset_rearm(QTestState *qts)
{
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_ERROR);
    mini_nand_expect_irq(qts, false);
    mini_nand_run_to_third_fault(qts);
    mini_nand_expect_third_fault(qts);
}

static void test_reset_command(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 3);
    MiniNandSnapshot memory_before;
    MiniNandSnapshot memory_after;

    mini_nand_intercept_irq(qts);
    mini_nand_seed_pending_reset(qts);
    mini_nand_snapshot(qts, &memory_before);
    mini_nand_command(qts, MINI_NAND_CMD_RESET);
    mini_nand_expect_reset_state(qts);
    mini_nand_snapshot(qts, &memory_after);
    g_assert_cmpmem(memory_before.guard_before,
                    sizeof(memory_before.guard_before),
                    memory_after.guard_before,
                    sizeof(memory_after.guard_before));
    g_assert_cmpmem(memory_before.dma, sizeof(memory_before.dma),
                    memory_after.dma, sizeof(memory_after.dma));
    g_assert_cmpmem(memory_before.guard_after,
                    sizeof(memory_before.guard_after),
                    memory_after.guard_after,
                    sizeof(memory_after.guard_after));
    mini_nand_verify_reset_rearm(qts);
    qtest_quit(qts);
}

static void test_reset_system(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 3);
    MiniNandSnapshot memory_before;
    MiniNandSnapshot memory_after;

    mini_nand_intercept_irq(qts);
    mini_nand_seed_pending_reset(qts);
    mini_nand_snapshot(qts, &memory_before);
    qtest_system_reset(qts);
    mini_nand_expect_reset_state(qts);
    mini_nand_snapshot(qts, &memory_after);
    g_assert_cmpmem(memory_before.guard_before,
                    sizeof(memory_before.guard_before),
                    memory_after.guard_before,
                    sizeof(memory_after.guard_before));
    g_assert_cmpmem(memory_before.dma, sizeof(memory_before.dma),
                    memory_after.dma, sizeof(memory_after.dma));
    g_assert_cmpmem(memory_before.guard_after,
                    sizeof(memory_before.guard_after),
                    memory_after.guard_after,
                    sizeof(memory_after.guard_after));
    mini_nand_verify_reset_rearm(qts);
    qtest_quit(qts);
}

static void test_trace_sequence(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 3);

    mini_nand_intercept_irq(qts);
    mini_nand_enable_irq(qts, MINI_NAND_IRQ_VALID_MASK);
    mini_nand_expect_irq(qts, false);

    mini_nand_read(qts, MINI_NAND_DMA);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_COMPLETE);
    mini_nand_reject_invalid_command(qts);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_COMPLETE);
    mini_nand_read(qts, MINI_NAND_DMA);
    g_assert_cmphex(mini_nand_readl(qts, REG_IRQ_STATUS), ==,
                    MINI_NAND_IRQ_COMPLETE);
    mini_nand_fill_memory(qts, 0xa5);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_expect_third_fault(qts);

    g_print("M5_TRACE_SNAPSHOT status=0x%08x error=0x%08x "
            "read_count=0x%08x fault_count=0x%08x irq_status=0x%08x "
            "irq_enable=0x%08x irq_level=%d sentinel_ok=1 "
            "guard_before_ok=1 guard_after_ok=1\n",
            mini_nand_readl(qts, REG_STATUS),
            mini_nand_readl(qts, REG_ERROR_CODE),
            mini_nand_readl(qts, REG_READ_COUNT),
            mini_nand_readl(qts, REG_FAULT_COUNT),
            mini_nand_readl(qts, REG_IRQ_STATUS),
            mini_nand_readl(qts, REG_IRQ_ENABLE),
            qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_qmp_schema_query_exact(void)
{
    QTestState *qts = mini_nand_qtest_start_prelaunch();
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'x-query-mini-nand' }");
    QDict *info = qdict_get_qdict(response, "return");
    const char *keys[] = {
        "fault-enabled", "nth", "once", "eligible-sequence", "fired",
        "status", "error", "read-count", "fault-count", "irq-status",
        "irq-enable",
    };

    g_assert_cmpuint(qdict_size(info), ==, 11);
    for (size_t index = 0; index < G_N_ELEMENTS(keys); index++) {
        g_assert(qdict_haskey(info, keys[index]));
    }
    g_assert_false(qdict_get_bool(info, "fault-enabled"));
    g_assert_cmpuint(qdict_get_uint(info, "nth"), ==, 0);
    g_assert_true(qdict_get_bool(info, "once"));
    g_assert_cmpuint(qdict_get_uint(info, "eligible-sequence"), ==, 0);
    g_assert_false(qdict_get_bool(info, "fired"));
    g_assert_cmpstr(qdict_get_str(info, "status"), ==, "idle");
    g_assert_cmpstr(qdict_get_str(info, "error"), ==, "none");
    qtest_quit(qts);
}

static void test_qmp_prelaunch_set_clear_query(void)
{
    QTestState *qts = mini_nand_qtest_start_prelaunch();

    mini_nand_qmp_assert_snapshot(qts, false, 0, 0, false);
    mini_nand_qmp_expect_success(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                 "'arguments': { 'nth': 3, 'once': true } }");
    mini_nand_qmp_assert_snapshot(qts, true, 3, 0, false);
    mini_nand_qmp_expect_success(qts, "{ 'execute': 'x-mini-nand-clear-fault' }");
    mini_nand_qmp_assert_snapshot(qts, false, 0, 0, false);
    qtest_quit(qts);
}

static void test_qmp_validation_precedence(void)
{
    QTestState *qts = mini_nand_qtest_start_prelaunch();

    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                "'arguments': { 'nth': 0, 'once': false } }",
                                "nth must be greater than zero");
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                "'arguments': { 'nth': 3, 'once': false } }",
                                "once must be true");
    mini_nand_qmp_expect_any_error(qts,
        "{ 'execute': 'x-mini-nand-set-fault', 'arguments': { 'once': true } }");
    mini_nand_qmp_expect_any_error(qts,
        "{ 'execute': 'x-mini-nand-set-fault', 'arguments': { 'nth': 3, "
        "'once': true, 'extra': 1 } }");
    mini_nand_qmp_expect_any_error(qts,
        "{ 'execute': 'x-mini-nand-set-fault', 'arguments': { 'nth': 'bad', "
        "'once': true } }");
    qtest_quit(qts);
    qts = qtest_initf("-S -M virt,aia=none,mini-nand=off -smp 1 -m 128M "
                      "-bios none");
    mini_nand_qmp_expect_status(qts, "prelaunch");
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                "'arguments': { 'nth': 3, 'once': true } }",
                                "MiniNand controller '/machine/mini-nand-ctrl' is not available");
    qtest_quit(qts);
}

static void test_qmp_runstate_reject(void)
{
    QTestState *qts = mini_nand_qtest_start(true, 0);

    mini_nand_qmp_expect_status(qts, "running");
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                "'arguments': { 'nth': 3, 'once': true } }",
                                "command is available only in prelaunch");
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-mini-nand-clear-fault' }",
                                "command is available only in prelaunch");
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-query-mini-nand' }",
                                "command is available only in prelaunch");
    qtest_qmp_assert_success(qts, "{ 'execute': 'stop' }");
    mini_nand_qmp_expect_status(qts, "paused");
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                "'arguments': { 'nth': 3, 'once': true } }",
                                "command is available only in prelaunch");
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-mini-nand-clear-fault' }",
                                "command is available only in prelaunch");
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-query-mini-nand' }",
                                "command is available only in prelaunch");
    qtest_quit(qts);
}

static void test_qmp_no_side_effects(void)
{
    QTestState *qts = mini_nand_qtest_start_prelaunch();
    MiniNandSnapshot before;
    MiniNandSnapshot after;

    mini_nand_intercept_irq(qts);
    mini_nand_fill_memory(qts, 0xa5);
    mini_nand_snapshot(qts, &before);
    mini_nand_qmp_expect_error(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                "'arguments': { 'nth': 0, 'once': true } }",
                                "nth must be greater than zero");
    mini_nand_qmp_expect_success(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                 "'arguments': { 'nth': 3, 'once': true } }");
    mini_nand_qmp_expect_success(qts, "{ 'execute': 'x-mini-nand-clear-fault' }");
    mini_nand_snapshot(qts, &after);
    mini_nand_assert_snapshot_equal(&before, &after);
    qtest_quit(qts);
}

static void test_qmp_event_third_fault(void)
{
    QTestState *qts = mini_nand_qtest_start_prelaunch();
    g_autoptr(QDict) event = NULL;

    mini_nand_qmp_expect_success(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                 "'arguments': { 'nth': 3, 'once': true } }");
    mini_nand_qmp_expect_success(qts, "{ 'execute': 'cont' }");
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    event = qtest_qmp_eventwait_ref(qts, "MINI_NAND_FAULT_INJECTED");
    QDict *data = qdict_get_qdict(event, "data");
    g_assert_cmpuint(qdict_size(data), ==, 4);
    g_assert_cmpstr(qdict_get_str(data, "operation"), ==, "read");
    g_assert_cmpuint(qdict_get_uint(data, "page"), ==,
                     MINI_NAND_PAGE);
    g_assert_cmpuint(qdict_get_uint(data, "sequence"), ==, 3);
    g_assert_cmpstr(qdict_get_str(data, "error"), ==, "uncorrectable");
    g_assert(qdict_haskey(event, "timestamp"));
    QDict *timestamp = qdict_get_qdict(event, "timestamp");
    g_assert_cmpuint(qdict_size(timestamp), ==, 2);
    g_assert_cmpuint(qdict_get_uint(timestamp, "seconds"), >, 0);
    g_assert_cmpuint(qdict_get_uint(timestamp, "microseconds"), <, 1000000);
    mini_nand_read(qts, MINI_NAND_DMA);
    g_assert_cmpuint(mini_nand_readl(qts, REG_FAULT_COUNT), ==, 1);
    g_assert_null(qtest_qmp_event_ref(qts, "MINI_NAND_FAULT_INJECTED"));
    qtest_quit(qts);
}

static void test_qmp_reset_preserve_rearm(void)
{
    QTestState *qts = mini_nand_qtest_start_prelaunch();
    MiniNandSnapshot memory_before;
    MiniNandSnapshot memory_after;

    mini_nand_qmp_expect_success(qts, "{ 'execute': 'x-mini-nand-set-fault', "
                                 "'arguments': { 'nth': 3, 'once': true } }");
    mini_nand_qmp_expect_success(qts, "{ 'execute': 'cont' }");
    mini_nand_intercept_irq(qts);
    mini_nand_fill_memory(qts, 0xa5);
    mini_nand_snapshot(qts, &memory_before);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    qtest_qmp_eventwait(qts, "MINI_NAND_FAULT_INJECTED");
    mini_nand_command(qts, MINI_NAND_CMD_RESET);
    g_assert_cmpuint(mini_nand_readl(qts, REG_READ_COUNT), ==, 0);
    g_assert_cmpuint(mini_nand_readl(qts, REG_IRQ_STATUS), ==, 0);
    mini_nand_expect_irq(qts, false);
    mini_nand_snapshot(qts, &memory_after);
    g_assert_cmpmem(memory_before.dma, sizeof(memory_before.dma),
                    memory_after.dma, sizeof(memory_after.dma));
    g_assert_cmpmem(memory_before.guard_before, sizeof(memory_before.guard_before),
                    memory_after.guard_before, sizeof(memory_after.guard_before));
    g_assert_cmpmem(memory_before.guard_after, sizeof(memory_before.guard_after),
                    memory_after.guard_after, sizeof(memory_after.guard_after));
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    qtest_qmp_eventwait(qts, "MINI_NAND_FAULT_INJECTED");
    mini_nand_snapshot(qts, &memory_before);
    qtest_system_reset(qts);
    mini_nand_expect_reset_state(qts);
    mini_nand_snapshot(qts, &memory_after);
    g_assert_cmpmem(memory_before.dma, sizeof(memory_before.dma),
                    memory_after.dma, sizeof(memory_after.dma));
    g_assert_cmpmem(memory_before.guard_before, sizeof(memory_before.guard_before),
                    memory_after.guard_before, sizeof(memory_after.guard_before));
    g_assert_cmpmem(memory_before.guard_after, sizeof(memory_before.guard_after),
                    memory_after.guard_after, sizeof(memory_after.guard_after));
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    mini_nand_read(qts, MINI_NAND_DMA);
    qtest_qmp_eventwait(qts, "MINI_NAND_FAULT_INJECTED");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mini-nand/map/off-on", test_map_off_on);
    g_test_add_func("/mini-nand/map/aia-unsupported",
                    test_map_aia_unsupported);
    g_test_add_func("/mini-nand/register/reset-access",
                    test_register_reset_access);
    g_test_add_func("/mini-nand/register/invalid-access-preserves-state",
                    test_register_invalid_access_preserves_state);
    g_test_add_func("/mini-nand/irq/event-before-enable",
                    test_irq_event_before_enable);
    g_test_add_func("/mini-nand/irq/enable-before-event",
                    test_irq_enable_before_event);
    g_test_add_func("/mini-nand/irq/disable-preserves-status",
                    test_irq_disable_preserves_status);
    g_test_add_func("/mini-nand/read/complete-dma", test_read_complete_dma);
    g_test_add_func("/mini-nand/reject/invalid-command",
                    test_reject_invalid_command);
    g_test_add_func("/mini-nand/reject/invalid-page",
                    test_reject_invalid_page);
    g_test_add_func("/mini-nand/reject/invalid-length",
                    test_reject_invalid_length);
    g_test_add_func("/mini-nand/reject/dma-overflow",
                    test_reject_dma_overflow);
    g_test_add_func("/mini-nand/error/dma-transaction",
                    test_error_dma_transaction);
    g_test_add_func("/mini-nand/error/third-read-fault",
                    test_error_third_read_fault);
    g_test_add_func("/mini-nand/irq/selective-w1c",
                    test_irq_selective_w1c);
    g_test_add_func("/mini-nand/reset/command", test_reset_command);
    g_test_add_func("/mini-nand/reset/system", test_reset_system);
    g_test_add_func("/mini-nand/trace/sequence", test_trace_sequence);
    g_test_add_func("/mini-nand/qmp/schema-query-exact",
                    test_qmp_schema_query_exact);
    g_test_add_func("/mini-nand/qmp/prelaunch-set-clear-query",
                    test_qmp_prelaunch_set_clear_query);
    g_test_add_func("/mini-nand/qmp/validation-precedence",
                    test_qmp_validation_precedence);
    g_test_add_func("/mini-nand/qmp/runstate-reject",
                    test_qmp_runstate_reject);
    g_test_add_func("/mini-nand/qmp/no-side-effects", test_qmp_no_side_effects);
    g_test_add_func("/mini-nand/qmp/event-third-fault", test_qmp_event_third_fault);
    g_test_add_func("/mini-nand/qmp/reset-preserve-rearm",
                    test_qmp_reset_preserve_rearm);
    return g_test_run();
}
