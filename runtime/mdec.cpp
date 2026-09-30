// MDEC (motion decoder) - minimal status emulation. CTR's gameplay doesn't use FMV;
// a full decoder can be added here if a movie path needs it.
#include "psx.h"

namespace psx {

static uint32_t status = 0x80040000;  // FIFO empty, current block 4

uint32_t mdec_read(int reg) {
    if (reg == 0) return 0;
    return status;
}
void mdec_write(int reg, uint32_t v) {
    if (reg == 1 && (v & 0x80000000)) status = 0x80040000;
    if (reg == 0) LOGD("MDEC command %08x", v);
}
void mdec_dma_in(const uint32_t*, int words) { LOGD("MDEC DMA in %d words", words); }
void mdec_dma_out(uint32_t* dst, int words) {
    for (int i = 0; i < words; i++) dst[i] = 0;
}
void mdec_init() {}

}  // namespace psx
