/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - device.c                                                *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2016 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "device.h"

#include "ai/ai_controller.h"
#include "memory/memory.h"
#include "pi/pi_controller.h"
#include "r4300/r4300_core.h"
#include "rdp/rdp_core.h"
#include "ri/ri_controller.h"
#include "rsp/rsp_core.h"
#include "si/si_controller.h"
#include "vi/vi_controller.h"

#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))


static void read_open_bus(void* opaque, uint32_t address, uint32_t* value)
{
    *value = (address & 0xffff);
    *value |= (*value << 16);
}

static void write_open_bus(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
}

static unsigned int dd_rom_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    return /* length / 8 */0x1000;
}

static unsigned int dd_rom_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    return /* length / 8 */0x1000;
}


static void get_pi_dma_handler(struct device* dev, uint32_t address, void** opaque, const struct pi_dma_handler** handler)
{
#define RW(o, x) \
    do { \
    static const struct pi_dma_handler h = { x ## _dma_read, x ## _dma_write }; \
    *opaque = (o); \
    *handler = &h; \
    } while(0)

    if (address >= MM_CART_ROM) {
        if (address >= MM_CART_DOM3) {
            /* 0x1fd00000 - 0x7fffffff : dom3 addr2, cart rom (Paper Mario (U)) ??? */
            RW(&dev->cart, cart_dom3);
        }
        else {
            /* 0x10000000 - 0x1fbfffff : dom1 addr2, cart rom */
            RW(&dev->cart.cart_rom, cart_rom);
        }
    }
    else if (address >= MM_CART_DOM2) {
        /* 0x08000000 - 0x0fffffff : dom2 addr2, cart save */
        RW(&dev->cart, cart_dom2);
    }
    else if (address >= MM_DD_ROM) {
        /* 0x06000000 - 0x07ffffff : dom1 addr1, dd rom */
        RW(NULL, dd_rom);
    }
#undef RW
}

void init_device(struct device* dev,
    /* memory */
    void* base,
    /* r4300 */
    unsigned int emumode,
    unsigned int count_per_op,
    int no_compiled_jump,
    int special_rom,
    int randomize_interrupt,
    /* ai */
    void* aout, const struct audio_out_backend_interface* iaout,
    /* ri */
    size_t dram_size,
    /* si */
    void* jbds[PIF_CHANNELS_COUNT],
    const struct joybus_device_interface* ijbds[PIF_CHANNELS_COUNT],
    /* vi */
    unsigned int vi_clock, unsigned int expected_refresh_rate,
    /* cart */
    void* af_rtc_clock, const struct clock_backend_interface* iaf_rtc_clock,
    size_t rom_size,
    uint16_t eeprom_type,
    void* eeprom_storage, const struct storage_backend_interface* ieeprom_storage,
    void* flashram_storage, const struct storage_backend_interface* iflashram_storage,
    void* sram_storage, const struct storage_backend_interface* isram_storage)
{
    struct interrupt_handler interrupt_handlers[] = {
        { &dev->vi,        vi_vertical_interrupt_event }, /* VI */
        { &dev->r4300,     compare_int_handler         }, /* COMPARE */
        { &dev->r4300,     check_int_handler           }, /* CHECK */
        { &dev->si,        si_end_of_dma_event         }, /* SI */
        { &dev->pi,        pi_end_of_dma_event         }, /* PI */
        { &dev->r4300.cp0, special_int_handler         }, /* SPECIAL */
        { &dev->ai,        ai_end_of_dma_event         }, /* AI */
        { &dev->sp,        rsp_interrupt_event         }, /* SP */
        { &dev->dp,        rdp_interrupt_event         }, /* DP */
        { &dev->si.pif,    hw2_int_handler             }, /* HW2 */
        { dev,             nmi_int_handler             }, /* NMI */
        { dev,             reset_hard_handler          }  /* reset_hard */
    };

#define R(x) read_ ## x
#define W(x) write_ ## x
#define RW(x) R(x), W(x)
#define A(x,m) (x), (x) | (m)
    struct mem_mapping mappings[] = {
        /* clear mappings */
        { 0x00000000, 0xffffffff, M64P_MEM_NOTHING, { NULL, RW(open_bus) } },
        /* memory map */
        { A(MM_RDRAM_DRAM, dram_size-1), M64P_MEM_RDRAM, { &dev->ri, RW(rdram_dram) } },
        { A(MM_RDRAM_REGS, 0xffff), M64P_MEM_RDRAMREG, { &dev->ri, RW(rdram_regs) } },
        { A(MM_RSP_MEM, 0xffff), M64P_MEM_RSPMEM, { &dev->sp, RW(rsp_mem) } },
        { A(MM_RSP_REGS, 0xffff), M64P_MEM_RSPREG, { &dev->sp, RW(rsp_regs) } },
        { A(MM_RSP_REGS2, 0xffff), M64P_MEM_RSP, { &dev->sp, RW(rsp_regs2) } },
        { A(MM_DPC_REGS, 0xffff), M64P_MEM_DP, { &dev->dp, RW(dpc_regs) } },
        { A(MM_DPS_REGS, 0xffff), M64P_MEM_DPS, { &dev->dp, RW(dps_regs) } },
        { A(MM_MI_REGS, 0xffff), M64P_MEM_MI, { &dev->r4300, RW(mi_regs) } },
        { A(MM_VI_REGS, 0xffff), M64P_MEM_VI, { &dev->vi, RW(vi_regs) } },
        { A(MM_AI_REGS, 0xffff), M64P_MEM_AI, { &dev->ai, RW(ai_regs) } },
        { A(MM_PI_REGS, 0xffff), M64P_MEM_PI, { &dev->pi, RW(pi_regs) } },
        { A(MM_RI_REGS, 0xffff), M64P_MEM_RI, { &dev->ri, RW(ri_regs) } },
        { A(MM_SI_REGS, 0xffff), M64P_MEM_SI, { &dev->si, RW(si_regs) } },
        { A(MM_CART_DOM2, 0x1ffff), M64P_MEM_FLASHRAMSTAT, { &dev->cart, RW(cart_dom2)  } },
        { A(MM_CART_ROM, rom_size-1), M64P_MEM_ROM, { &dev->cart.cart_rom, RW(cart_rom) } },
        { A(MM_PIF_MEM, 0xffff), M64P_MEM_PIF, { &dev->si, RW(pif_ram) } }
    };

    struct mem_handler dbg_handler = { &dev->r4300, RW(with_bp_checks) };
#undef A
#undef R
#undef W
#undef RW

    init_memory(&dev->mem, mappings, ARRAY_SIZE(mappings), base, &dbg_handler);
    init_r4300(&dev->r4300, &dev->mem, &dev->ri, interrupt_handlers,
            emumode, count_per_op, no_compiled_jump, special_rom, randomize_interrupt);
    init_rdp(&dev->dp, &dev->r4300, &dev->sp, &dev->ri);
    init_rsp(&dev->sp, mem_base_u32(base, MM_RSP_MEM), &dev->r4300, &dev->dp, &dev->ri);
    init_ai(&dev->ai, &dev->r4300, &dev->ri, &dev->vi, aout, iaout);
    init_pi(&dev->pi,
            dev, get_pi_dma_handler,
            &dev->r4300, &dev->ri);
    init_ri(&dev->ri, mem_base_u32(base, MM_RDRAM_DRAM), dram_size);
    init_si(&dev->si,
        (uint8_t*)mem_base_u32(base, MM_PIF_MEM),
        jbds, ijbds,
        (uint8_t*)mem_base_u32(base, MM_CART_ROM + 0x40),
        &dev->r4300, &dev->ri);
    init_vi(&dev->vi, vi_clock, expected_refresh_rate, &dev->r4300);

    init_cart(&dev->cart,
            af_rtc_clock, iaf_rtc_clock,
            (uint8_t*)mem_base_u32(base, MM_CART_ROM), rom_size,
            &dev->r4300,
            &dev->ri.rdram, &dev->si.pif.cic,
            eeprom_type, eeprom_storage, ieeprom_storage,
            flashram_storage, iflashram_storage,
            sram_storage, isram_storage);
}

void poweron_device(struct device* dev)
{
    size_t i;

    poweron_r4300(&dev->r4300);
    poweron_rdp(&dev->dp);
    poweron_rsp(&dev->sp);
    poweron_ai(&dev->ai);
    poweron_pi(&dev->pi);
    poweron_ri(&dev->ri);
    poweron_si(&dev->si);
    poweron_vi(&dev->vi);

    poweron_cart(&dev->cart);

    /* poweron for controllers */
    for(i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
        struct pif_channel* channel = &dev->si.pif.channels[i];

        if ((channel->ijbd != NULL) && (channel->ijbd->poweron != NULL)) {
            channel->ijbd->poweron(channel->jbd);
        }
    }
}

void run_device(struct device* dev)
{
    /* device execution is driven by the r4300 */
    run_r4300(&dev->r4300);
}

void stop_device(struct device* dev)
{
    /* set stop flag so that r4300 execution will be stopped at next interrupt */
    *r4300_stop(&dev->r4300) = 1;
}

void hard_reset_device(struct device* dev)
{
    /* set reset hard flag so reset_hard will be called at next interrupt */
    dev->r4300.reset_hard_job = 1;
}

void soft_reset_device(struct device* dev)
{
    reset_pif(&dev->si.pif, 1); /* Soft reset */
}
