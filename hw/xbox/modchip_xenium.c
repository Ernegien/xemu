/*
 * Xenium Modchip - https://github.com/Ryzee119/OpenXenium
 *
 * Copyright (c) 2021 Mike Davis
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "sysemu/sysemu.h"
#include "hw/char/serial.h"
#include "hw/isa/isa.h"
#include "qapi/error.h"

#define XENIUM_REGISTER_BASE    0xEE
    #define XENIUM_REGISTER0    0
    #define XENIUM_REGISTER1    1

#define DEBUG
#ifdef DEBUG
# define DPRINTF(format, ...) printf(format, ## __VA_ARGS__)
#else
# define DPRINTF(format, ...) do { } while (0)
#endif

typedef struct XeniumState {
    ISADevice dev;
    MemoryRegion io;

    // SPI
    bool sck;
    bool cs;
    bool mosi;
    bool miso_1;    // pin 1
    bool miso_4;    // pin 4

    unsigned char led;              // XXXXXBGR
    unsigned short bank_control;    // determines flash address mask

    bool recovery;  // 0 is active
} XeniumState;

#define XENIUM_DEVICE(obj) \
    OBJECT_CHECK(XeniumState, (obj), "modchip-xenium")

uint32_t xenium_mask_flash_address(uint32_t address, const char* mask) {
    uint32_t translation = address;
    for (int i = 0; i < 3; i++) {
        uint32_t bitval = 1 << (20 - i);
        switch(mask[i]) {
            case '1': translation |= bitval; break;
            case '0': translation &= ~bitval; break;
            case 'X': /* do nothing */ break;
            default: assert(false);
        }
    }
}

// TODO: not sure of the best way to enforce this yet...
uint32_t xenium_translate_flash_address(uint32_t address, uint8_t bank_control) {
    uint32_t translation = address;
    switch (bank_control) {
        case 0: return xenium_mask_flash_address(address, "XXX");  // TSOP
        case 1: return xenium_mask_flash_address(address, "110");  // XeniumOS Cromwell loader
        case 2: return xenium_mask_flash_address(address, "10X");  // XeniumOS
        case 3: return xenium_mask_flash_address(address, "000");  // BANK1 (USER BIOS 256kB)
        case 4: return xenium_mask_flash_address(address, "001");  // BANK2 (USER BIOS 256kB)
        case 5: return xenium_mask_flash_address(address, "010");  // BANK3 (USER BIOS 256kB)
        case 6: return xenium_mask_flash_address(address, "011");  // BANK4 (USER BIOS 256kB)
        case 7: return xenium_mask_flash_address(address, "00X");  // BANK1 (USER BIOS 512kB)
        case 8: return xenium_mask_flash_address(address, "01X");  // BANK2 (USER BIOS 512kB)
        case 9: return xenium_mask_flash_address(address, "0XX");  // BANK1 (USER BIOS 1MB)
        case 10: return xenium_mask_flash_address(address, "111"); // RECOVERY
        default: assert(false);
    }
}

static void xenium_io_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned int size)
{
    XeniumState *s = opaque;

    DPRINTF("%s: Write 0x%X to IO register 0x%X\n",
        __func__, val, XENIUM_REGISTER_BASE + addr);

    switch(addr) {
        case XENIUM_REGISTER0:
            assert((val >> 3) == 0);    // un-known/used
            s->led = val;
            DPRINTF("%s: Set LED color(s) to ", __func__);
            if (val & (1 << 0)) DPRINTF("Red ");
            if (val & (1 << 1)) DPRINTF("Green ");
            if (val & (1 << 2)) DPRINTF("Blue ");
            DPRINTF("\n");
        break;
        case XENIUM_REGISTER1:
            assert((val & (1 << 7)) == 0);    // un-known/used
            s->sck = val & (1 << 6);
            s->cs = val & (1 << 5);
            s->mosi = val & (1 << 4);
            s->bank_control = val & 0xF;
        break;
        default: assert(false);
    }
}

static uint64_t xenium_io_read(void *opaque, hwaddr addr, unsigned int size)
{
    XeniumState *s = opaque;
    uint32_t val = 0;

    switch(addr) {
        case XENIUM_REGISTER0:
            val = 0x55;     // genuine xenium!
        break;
        case XENIUM_REGISTER1:
            val = (s->recovery << 7) |
                (s->miso_1 << 5) |
                (s->miso_4 << 4) |
                s->bank_control;
        break;
        default: assert(false);
    }

    DPRINTF("%s: Read 0x%X from IO register 0x%X\n",
        __func__, val, XENIUM_REGISTER_BASE + addr);

    return val;
}

static const MemoryRegionOps xenium_io_ops = {
    .read  = xenium_io_read,
    .write = xenium_io_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void xenium_realize(DeviceState *dev, Error **errp)
{
    XeniumState *s = XENIUM_DEVICE(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    // default state
    s->bank_control = 1;    // regular cromwell bootloader
    s->recovery = 1;        // inactive
    s->led = 1;             // red

    memory_region_init_io(&s->io, OBJECT(s), &xenium_io_ops, s, "modchip-xenium", 2);   // 0xEE & 0xEF
    isa_register_ioport(isa, &s->io, XENIUM_REGISTER_BASE);
}

static Property xenium_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_xenium = {
    .name = "modchip-xenium",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

static void xenium_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = xenium_realize;
    dc->vmsd = &vmstate_xenium;
    device_class_set_props(dc, xenium_properties);
}

static void xenium_initfn(Object *o)
{
    XeniumState *self = XENIUM_DEVICE(o);
}

static const TypeInfo xenium_type_info = {
    .name          = "modchip-xenium",
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(XeniumState),
    .instance_init = xenium_initfn,
    .class_init    = xenium_class_init,
};

static void xenium_register_types(void)
{
    type_register_static(&xenium_type_info);
}

type_init(xenium_register_types)
