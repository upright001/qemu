#include "qemu/osdep.h"
#include "hw/misc/mini-nand-flash.h"

static void assert_page_pattern(uint32_t page, const uint8_t *data)
{
    for (size_t offset = 0; offset < MINI_NAND_FLASH_PAGE_SIZE; offset++) {
        g_assert_cmpuint(data[offset], ==, (page + offset) & 0xff);
    }
}

static void test_geometry(void)
{
    g_assert_cmpuint(MINI_NAND_FLASH_PAGE_SIZE, ==, 4096);
    g_assert_cmpuint(MINI_NAND_FLASH_PAGE_COUNT, ==, 64);
    g_assert_cmpuint(MINI_NAND_FLASH_TOTAL_SIZE, ==, 262144);
    g_assert_cmpuint(sizeof(MiniNandFlash), ==, 1);
}

static void test_valid_pages(void)
{
    static const uint32_t pages[] = { 0, 42, 63 };
    MiniNandFlash flash = { 0 };

    for (size_t index = 0; index < G_N_ELEMENTS(pages); index++) {
        struct {
            uint8_t before;
            uint8_t data[MINI_NAND_FLASH_PAGE_SIZE];
            uint8_t after;
        } output = { .before = 0x3c, .after = 0xc3 };

        memset(output.data, 0xa5, sizeof(output.data));
        g_assert_cmpint(mini_nand_flash_read(&flash, pages[index],
                                             output.data,
                                             sizeof(output.data)),
                        ==, MINI_NAND_FLASH_OK);
        g_assert_cmphex(output.before, ==, 0x3c);
        g_assert_cmphex(output.after, ==, 0xc3);
        assert_page_pattern(pages[index], output.data);
    }

    g_assert_cmphex(flash.reserved, ==, 0);
}

static void test_page_42_pattern_and_wrap(void)
{
    MiniNandFlash flash = { 0 };
    uint8_t data[MINI_NAND_FLASH_PAGE_SIZE];

    g_assert_cmpint(mini_nand_flash_read(&flash, 42, data, sizeof(data)),
                    ==, MINI_NAND_FLASH_OK);
    g_assert_cmphex(data[0], ==, 0x2a);
    g_assert_cmphex(data[1], ==, 0x2b);
    g_assert_cmphex(data[2], ==, 0x2c);
    g_assert_cmphex(data[3], ==, 0x2d);
    g_assert_cmphex(data[213], ==, 0xff);
    g_assert_cmphex(data[214], ==, 0x00);
    g_assert_cmphex(data[215], ==, 0x01);
    assert_page_pattern(42, data);
}

static void test_repeated_read_is_identical(void)
{
    MiniNandFlash flash = { 0 };
    MiniNandFlash original = flash;
    uint8_t first[MINI_NAND_FLASH_PAGE_SIZE];
    uint8_t second[MINI_NAND_FLASH_PAGE_SIZE];

    memset(first, 0x11, sizeof(first));
    memset(second, 0xee, sizeof(second));
    g_assert_cmpint(mini_nand_flash_read(&flash, 42, first, sizeof(first)),
                    ==, MINI_NAND_FLASH_OK);
    g_assert_cmpint(mini_nand_flash_read(&flash, 42, second,
                                         sizeof(second)),
                    ==, MINI_NAND_FLASH_OK);
    g_assert_cmpmem(first, sizeof(first), second, sizeof(second));
    g_assert_cmpmem(&flash, sizeof(flash), &original, sizeof(original));
}

static void test_invalid_page_preserves_buffer(void)
{
    MiniNandFlash flash = { 0 };
    MiniNandFlash original = flash;
    uint8_t data[MINI_NAND_FLASH_PAGE_SIZE];
    uint8_t sentinel[MINI_NAND_FLASH_PAGE_SIZE];

    memset(data, 0xa5, sizeof(data));
    memset(sentinel, 0xa5, sizeof(sentinel));
    g_assert_cmpint(mini_nand_flash_read(&flash, 64, data, sizeof(data)),
                    ==, MINI_NAND_FLASH_INVALID_PAGE);
    g_assert_cmpmem(data, sizeof(data), sentinel, sizeof(sentinel));
    g_assert_cmpmem(&flash, sizeof(flash), &original, sizeof(original));
}

static void test_invalid_lengths_preserve_buffer(void)
{
    static const size_t lengths[] = { 0, 4095, 4097 };
    MiniNandFlash flash = { 0 };
    MiniNandFlash original = flash;
    uint8_t data[MINI_NAND_FLASH_PAGE_SIZE + 1];
    uint8_t sentinel[MINI_NAND_FLASH_PAGE_SIZE + 1];

    memset(sentinel, 0xa5, sizeof(sentinel));
    for (size_t index = 0; index < G_N_ELEMENTS(lengths); index++) {
        memcpy(data, sentinel, sizeof(data));
        g_assert_cmpint(mini_nand_flash_read(&flash, 42, data,
                                             lengths[index]),
                        ==, MINI_NAND_FLASH_INVALID_LENGTH);
        g_assert_cmpmem(data, sizeof(data), sentinel, sizeof(sentinel));
        g_assert_cmpmem(&flash, sizeof(flash), &original, sizeof(original));
    }
}

static void test_both_invalid_prefers_page_error(void)
{
    MiniNandFlash flash = { 0 };
    MiniNandFlash original = flash;
    uint8_t data[MINI_NAND_FLASH_PAGE_SIZE];
    uint8_t sentinel[MINI_NAND_FLASH_PAGE_SIZE];

    memset(data, 0xa5, sizeof(data));
    memset(sentinel, 0xa5, sizeof(sentinel));
    g_assert_cmpint(mini_nand_flash_read(&flash, 64, data, 4095),
                    ==, MINI_NAND_FLASH_INVALID_PAGE);
    g_assert_cmpmem(data, sizeof(data), sentinel, sizeof(sentinel));
    g_assert_cmpmem(&flash, sizeof(flash), &original, sizeof(original));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mini-nand-flash/geometry", test_geometry);
    g_test_add_func("/mini-nand-flash/valid-pages", test_valid_pages);
    g_test_add_func("/mini-nand-flash/page-42-pattern-wrap",
                    test_page_42_pattern_and_wrap);
    g_test_add_func("/mini-nand-flash/repeated-read",
                    test_repeated_read_is_identical);
    g_test_add_func("/mini-nand-flash/invalid-page",
                    test_invalid_page_preserves_buffer);
    g_test_add_func("/mini-nand-flash/invalid-lengths",
                    test_invalid_lengths_preserve_buffer);
    g_test_add_func("/mini-nand-flash/both-invalid",
                    test_both_invalid_prefers_page_error);
    return g_test_run();
}
