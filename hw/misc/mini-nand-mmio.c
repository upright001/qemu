#include "qemu/osdep.h"
#include "hw/misc/mini-nand-core.h"
#include "hw/misc/mini-nand-mmio.h"
#include "qemu/log.h"
#include "qemu/range.h"
#include "system/dma.h"

void mini_nand_mmio_adapter_init(MiniNandMmioAdapter *adapter,
                                 MiniNandCore *core,
                                 MiniNandMmioPostWriteFn post_write,
                                 void *post_write_opaque)
{
    adapter->core = core;
    adapter->post_write = post_write;
    adapter->post_write_opaque = post_write_opaque;
}

bool mini_nand_mmio_dma_write(void *opaque,
                              uint64_t address,
                              const uint8_t *source,
                              size_t length)
{
    AddressSpace *address_space = opaque;

    return dma_memory_write(address_space, address, source, length,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}

static const char *mini_nand_access_class(MiniNandAccessResult result)
{
    switch (result) {
    case MINI_NAND_ACCESS_READ_ONLY:
        return "read-only";
    case MINI_NAND_ACCESS_RESERVED:
        return "reserved";
    case MINI_NAND_ACCESS_UNDEFINED:
        return "undefined";
    case MINI_NAND_ACCESS_OK:
    default:
        return "unknown";
    }
}

static uint64_t mini_nand_mmio_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    MiniNandMmioAdapter *adapter = opaque;
    MiniNandAccessResult result;

    return mini_nand_core_read(adapter->core, offset, &result);
}

static void mini_nand_mmio_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    MiniNandMmioAdapter *adapter = opaque;
    MiniNandAccessResult result;

    result = mini_nand_core_write(adapter->core, offset, value);
    if (result != MINI_NAND_ACCESS_OK) {
        /*
         * register policy의 trace는 MMIO 경계가 소유한다.
         * core는 QEMU logger에 의존하지 않는다.
         * 따라서 core만 단독으로 test할 수 있다.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      MINI_NAND_MMIO_NAME
                      ": rejected write offset=0x%" HWADDR_PRIx
                      " class=%s\n",
                      offset, mini_nand_access_class(result));
        return;
    }

    /*
     * Adapter는 IRQ에 영향을 주는 register 목록을 복제하지 않는다.
     * 성공한 모든 write 뒤 generic callback으로 상위 계층이 상태를 재평가한다.
     */
    if (adapter->post_write) {
        adapter->post_write(adapter->post_write_opaque);
    }
}

const MemoryRegionOps mini_nand_mmio_ops = {
    .read = mini_nand_mmio_read,
    .write = mini_nand_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .impl.unaligned = false,
};

static bool mini_nand_mmio_range_wraps(const MemMapEntry *entry)
{
    return entry->size != 0 &&
           entry->base > HWADDR_MAX - (entry->size - 1);
}

bool mini_nand_mmio_range_is_free(const MemMapEntry *map,
                                  size_t entries,
                                  size_t self_index)
{
    const MemMapEntry *self;

    if (!map || self_index >= entries) {
        return false;
    }

    self = &map[self_index];
    if (self->size == 0 || mini_nand_mmio_range_wraps(self)) {
        return false;
    }

    for (size_t index = 0; index < entries; index++) {
        const MemMapEntry *peer = &map[index];

        if (index == self_index || peer->size == 0) {
            continue;
        }

        /*
         * ranges_overlap()은 wrapping range에서 정의되지 않는다.
         * 계산 전에 이를 거부한다.
         * 따라서 board map 검사는 항상 보수적이다.
         */
        if (mini_nand_mmio_range_wraps(peer) ||
            ranges_overlap(self->base, self->size,
                           peer->base, peer->size)) {
            return false;
        }
    }
    return true;
}
