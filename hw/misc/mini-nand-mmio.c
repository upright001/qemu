#include "qemu/osdep.h"
#include "hw/misc/mini-nand-core.h"
#include "hw/misc/mini-nand-mmio.h"
#include "qemu/log.h"
#include "qemu/range.h"
#include "system/dma.h"
#include "trace.h"

void mini_nand_mmio_adapter_init(MiniNandMmioAdapter *adapter,
                                 MiniNandCore *core,
                                 MiniNandMmioPostWriteFn post_write,
                                 void *post_write_opaque)
{
    adapter->core = core;
    adapter->post_write = post_write;
    adapter->post_write_opaque = post_write_opaque;
    adapter->fault_observer = NULL;
    adapter->fault_observer_opaque = NULL;
}

void mini_nand_mmio_set_fault_observer(MiniNandMmioAdapter *adapter,
                                       MiniNandMmioFaultObserver observer,
                                       void *opaque)
{
    adapter->fault_observer = observer;
    adapter->fault_observer_opaque = opaque;
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
    MiniNandCore *core = adapter->core;
    MiniNandAccessResult result;
    uint32_t command = value;
    uint32_t request_page = 0;
    uint64_t request_dma_address = 0;
    uint32_t request_length = 0;
    uint32_t previous_read_count = 0;
    uint32_t previous_fault_count = 0;
    MiniNandCoreSnapshot before;
    MiniNandCoreSnapshot after;
    bool is_command = offset == MINI_NAND_REG_COMMAND;

    if (is_command) {
        before = mini_nand_core_snapshot(core);
        request_page = core->page;
        request_dma_address = ((uint64_t)core->dma_addr_hi << 32) |
                              core->dma_addr_lo;
        request_length = core->length;
        previous_read_count = core->read_count;
        previous_fault_count = core->fault_count;
        trace_mini_nand_command(command, request_page,
                                request_dma_address, request_length);
    }

    result = mini_nand_core_write(core, offset, value);
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

    if (is_command && command != MINI_NAND_CMD_RESET) {
        bool operation_started = core->read_count != previous_read_count;

        /*
         * Core가 terminal state, counter와 DMA side effect를 모두 확정한 뒤
         * trace를 남기고 마지막에 line sync를 요청한다. Trace가 켜져도
         * firmware-visible 상태 전이 순서가 바뀌지 않게 하는 경계다.
        */
        if (core->fault_count != previous_fault_count) {
            trace_mini_nand_fault(request_page,
                                  core->fault_policy.eligible_read_sequence,
                                  core->read_count, core->fault_count);
        }
        if (core->status == MINI_NAND_STATUS_DONE && operation_started) {
            trace_mini_nand_completion(request_page, core->status,
                                       core->read_count, core->fault_count,
                                       core->irq_status);
        } else if (core->status == MINI_NAND_STATUS_ERROR) {
            trace_mini_nand_error(request_page, core->error_code,
                                  operation_started, core->read_count,
                                  core->fault_count, core->irq_status);
        }
    }

    /*
     * Adapter는 IRQ에 영향을 주는 register 목록을 복제하지 않는다.
     * 성공한 모든 write 뒤 generic callback으로 상위 계층이 상태를 재평가한다.
     */
    if (adapter->post_write) {
        adapter->post_write(adapter->post_write_opaque);
    }
    if (is_command) {
        after = mini_nand_core_snapshot(core);
        /* IRQ sync 뒤의 좁은 predicate만 once fault event를 외부로 보낸다. */
        if (adapter->fault_observer && command == MINI_NAND_CMD_READ &&
            !before.fault.fired && after.fault.fired && after.fault.enabled &&
            after.fault.once &&
            after.fault.eligible_sequence == after.fault.nth &&
            after.error_code == MINI_NAND_ERR_UNCORRECTABLE &&
            (uint32_t)(after.fault_count - before.fault_count) == 1U) {
            adapter->fault_observer(adapter->fault_observer_opaque, command,
                                    request_page, after.fault.eligible_sequence,
                                    after.error_code);
        }
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
