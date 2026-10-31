#include "gfx_platform.h"
#include "platform_memory.h"
#if !defined(__riscv) || __riscv_xlen != 32
#error "This adapter is for the recorded RV32 Sapphire platform, not a host model"
#endif
#ifdef MIRROR_KEY3
#define GFX_IO_HOT __attribute__((section(".text.key3_submit_hot"),noinline,noclone))
#else
#define GFX_IO_HOT
#endif
static uint32_t read_word(uint32_t address) {
    return *(volatile uint32_t *)(uintptr_t)address;
}
static GFX_IO_HOT uint32_t read_reg(void *ctx, uint32_t offset) {
    (void)ctx;
    return read_word(GFX_MMIO_BASE + offset);
}
static GFX_IO_HOT void write_reg(void *ctx, uint32_t offset, uint32_t value) {
    (void)ctx;
    *(volatile uint32_t *)(uintptr_t)(GFX_MMIO_BASE + offset) = value;
}
static GFX_IO_HOT void publish(void *ctx) {
    (void)ctx;
    __asm__ volatile("fence iorw,iorw" ::: "memory");
}
static int sync_memory(void *ctx, uint32_t address, size_t length) {
    (void)ctx;
    if (!gfx_ti60_sync_range(address,length))
        return GFX_EINVAL;
    /* Only the hash-pinned D011 write-through cache. This is not a generic
     * write-back clean. The vendor CSR 0x810 helper traps on this SoC.
     * Whole-cache maintenance is conservative; optimize only after measurement. */
    __asm__ volatile("fence rw,rw\n.word 0x500f\nfence rw,rw" ::: "memory");
    return GFX_OK;
}
static GFX_IO_HOT uint64_t ticks(void *ctx) {
    uint32_t hi, lo, again;
    (void)ctx;
    do {
        hi = read_word(GFX_MTIME_LO_ADDR + 4);
        lo = read_word(GFX_MTIME_LO_ADDR);
        again = read_word(GFX_MTIME_LO_ADDR + 4);
    } while (hi != again);
    return ((uint64_t)hi << 32) | lo;
}
static void idle(void *ctx) {
    (void)ctx;
    __asm__ volatile("nop"); /* Polling baseline: WFI could wait indefinitely. */
}
gfx_io gfx_ti60_io(void) {
    return (gfx_io){.ctx=0, .read=read_reg, .write=write_reg, .publish=publish,
        .sync_device=sync_memory, .sync_cpu=sync_memory, .ticks=ticks, .idle=idle};
}
int gfx_ti60_init(gfx_device *device) {
    int result = gfx_init(device, gfx_ti60_io(), GFX_POOL_START, GFX_POOL_END);
    if (result == GFX_OK) write_reg(0, GFX_REG_IRQ_ENABLE, 0);
    return result;
}
