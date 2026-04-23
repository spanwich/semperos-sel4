/* FPT-179: PCI configuration-space access helpers shared between DTUBridge's
 * E1000 driver and the VirtIO-PCI net driver. These were previously static
 * in DTUBridge.c; extracted here so the VirtIO driver file can reuse them
 * without duplication. */
#ifndef DTUBRIDGE_PCI_HELPERS_H
#define DTUBRIDGE_PCI_HELPERS_H

#include <stdint.h>

#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_CAP_PTR         0x34
#define PCI_BAR0            0x10
#define PCI_BAR(n)          (PCI_BAR0 + (n) * 4)

#define PCI_CMD_MEM_SPACE   0x0002
#define PCI_CMD_BUS_MASTER  0x0004

#define PCI_STATUS_CAP_LIST 0x0010

#define PCI_CAP_ID_VENDOR   0x09  /* vendor-specific (VirtIO uses this) */

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint8_t  pci_cfg_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);
void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val);

#endif /* DTUBRIDGE_PCI_HELPERS_H */
