// Hardware registers: every guest access from 0xFD000000 up arrives here
// (cpu.h) and goes to its device. A device without a handler is plain
// memory, and its first access is logged.
#include "xbox.h"
#include <string.h>

uint32_t nv2a_mmio_read(uint32_t o, int size);
void nv2a_mmio_write(uint32_t o, uint32_t v, int size);
uint32_t apu_mmio_read(uint32_t o, int size);
void apu_mmio_write(uint32_t o, uint32_t v, int size);

uint32_t ohci_mmio_read(uint32_t o, int size);
void ohci_mmio_write(uint32_t o, uint32_t v, int size);

uint32_t aci_read(uint32_t o, int size);
void aci_write(uint32_t o, uint32_t v, int size);

#define ACI_BASE 0xFEC00000u
#define ACI_SIZE 0x00000200u
#define OHCI_BASE 0xFED00000u
#define OHCI_SIZE 0x00001000u
#define APU_BASE 0xFE800000u
#define APU_SIZE 0x00080000u

static uint8_t logged[4096];   // 1 MB pages

static void first(uint32_t a, const char *what, uint32_t v)
{
    if (logged[a >> 20]) return;
    logged[a >> 20] = 1;
    hle_log("mmio: first %s at 0x%08x (0x%x)\n", what, a, v);
}

uint32_t mmio_read(uint32_t a, int size)
{
    if (a - NV2A_BASE < NV2A_SIZE) return nv2a_mmio_read(a - NV2A_BASE, size);
    if (a - APU_BASE < APU_SIZE) return apu_mmio_read(a - APU_BASE, size);
    if (a - OHCI_BASE < OHCI_SIZE) return ohci_mmio_read(a - OHCI_BASE, size);
    if (a - ACI_BASE < ACI_SIZE) return aci_read(a - ACI_BASE, size);
    uint32_t v = 0;
    memcpy(&v, M + a, size);
    first(a, "read", v);
    return v;
}

void mmio_write(uint32_t a, uint32_t v, int size)
{
    if (a - NV2A_BASE < NV2A_SIZE) {
        nv2a_mmio_write(a - NV2A_BASE, v, size);
        return;
    }
    if (a - APU_BASE < APU_SIZE) {
        apu_mmio_write(a - APU_BASE, v, size);
        return;
    }
    if (a - OHCI_BASE < OHCI_SIZE) {
        ohci_mmio_write(a - OHCI_BASE, v, size);
        return;
    }
    if (a - ACI_BASE < ACI_SIZE) {
        aci_write(a - ACI_BASE, v, size);
        return;
    }
    first(a, "write", v);
    memcpy(M + a, &v, size);
}
