/* KallistiOS ##version##

   pvr_dma.c
   Copyright (C) 2002 Roger Cattermole
   Copyright (C) 2004 Megan Potter
   Copyright (C) 2023 Ruslan Rostovtsev
   Copyright (C) 2023, 2024 Andress Barajas

   http://www.boob.co.uk
 */

#include <stdio.h>
#include <errno.h>
#include <dc/pvr.h>
#include <dc/asic.h>
#include <dc/dmac.h>
#include <dc/sq.h>
#include <kos/thread.h>
#include <kos/sem.h>

#include "pvr_internal.h"

/* TA DMA Registers */
typedef struct {
    uintptr_t  dest_addr;   /* PVR destination address */
    size_t     size;        /* Size in bytes; Must be a multiple of 32 */
    uint32_t   start;       /* DMA start */
} ta_dma_ctrl_t;

#define TA_DMA_REG_BASE 0xa05f6800
static volatile ta_dma_ctrl_t * const ta_dma = (ta_dma_ctrl_t *)TA_DMA_REG_BASE;
static vuint32 *ta_dma_lmmode0 = (vuint32 *)0xa05f6884;
static vuint32 *ta_dma_lmmode1 = (vuint32 *)0xa05f6888;

/* Signaling semaphores for TA DMA */
static semaphore_t ta_dma_done;
static int32_t ta_dma_blocking;
static pvr_dma_callback_t ta_dma_callback;
static void *ta_dma_cbdata;

/* PVR DMA Registers */
typedef struct {
    uintptr_t  pvr_addr;  /* PVR address */
    uintptr_t  sh4_addr;  /* SH-4 address */
    size_t     size;      /* Size in bytes; Must be a multiple of 32 */
    uint32_t   dir;       /* 0: sh4->PVR; 1: PVR->sh4 */
    uint32_t   trigger;   /* DMA trigger select; 0-CPU, 1-HW */
    uint32_t   enable;    /* DMA enable */
    uint32_t   start;     /* DMA start */
} pvr_dma_ctrl_t;

#define PVR_DMA_REG_BASE 0xa05f7c00
static volatile pvr_dma_ctrl_t * const pvr_dma = (pvr_dma_ctrl_t *)PVR_DMA_REG_BASE;
static vuint32 *pvr_dma_pro = (vuint32 *)0xa05f7c80;

/* Signaling semaphore for PVR DMA */
static semaphore_t pvr_dma_done;
static int32_t pvr_dma_blocking;
static pvr_dma_callback_t pvr_dma_callback;
static void *pvr_dma_cbdata;

#define CPU_TRIGGER       0
#define HARDWARE_TRIGGER  1

/* Protection register code. */
#define PVR_DMA_UNLOCK_CODE    0x6702
/* All PVR memory protection values. */
#define PVR_DMA_UNLOCK_ALLMEM  (PVR_DMA_UNLOCK_CODE << 16 | 0x007F)
#define PVR_DMA_LOCK_ALLMEM    (PVR_DMA_UNLOCK_CODE << 16 | 0x7F00)

static void ta_dma_irq_hnd(uint32_t code, void *data) {
    (void)code;
    (void)data;

    if(DMAC_DMATCR2 != 0)
        dbglog(DBG_INFO, "ta_dma_irq_hnd: The dma did not complete successfully\n");

    /* Call the callback, if any. */
    if(ta_dma_callback) {
        /* This song and dance is necessary because the handler
           could chain to itself. */
        pvr_dma_callback_t cb = ta_dma_callback;
        void *d = ta_dma_cbdata;

        ta_dma_callback = NULL;
        ta_dma_cbdata = 0;

        cb(d);
    }

    /* Signal the calling thread to continue, if any. */
    if(ta_dma_blocking) {
        sem_signal(&ta_dma_done);
        thd_schedule(1, 0);
        ta_dma_blocking = 0;
    }
}

static void pvr_dma_irq_hnd(uint32_t code, void *data) {
    (void)code;
    (void)data;

    /* Call the callback, if any. */
    if(pvr_dma_callback) {
        /* This song and dance is necessary because the handler
           could chain to itself. */
        pvr_dma_callback_t cb = pvr_dma_callback;
        void *d = pvr_dma_cbdata;

        pvr_dma_callback = NULL;
        pvr_dma_cbdata = 0;

        cb(d);
    }

    /* Signal the calling thread to continue, if any. */
    if(pvr_dma_blocking) {
        sem_signal(&pvr_dma_done);
        thd_schedule(1, 0);
        pvr_dma_blocking = 0;
    }
}

static uintptr_t pvr_dest_addr(uintptr_t dest, pvr_dma_mode_t type) {
    uintptr_t dest_addr;
    uintptr_t masked_dest = (uintptr_t)dest & 0xFFFFFF;

    switch(type) {
        case PVR_DMA_TA:
            dest_addr = masked_dest | PVR_TA_INPUT;
            break;

        case PVR_DMA_YUV:
            dest_addr = masked_dest | PVR_TA_YUV_CONV;
            break;

        case PVR_DMA_VRAM64:
            dest_addr = masked_dest | PVR_TA_TEX_MEM;
            break;

        case PVR_DMA_VRAM32:
            dest_addr = masked_dest | PVR_TA_TEX_MEM_32;
            break;

        case PVR_DMA_VRAM64_SB:
            dest_addr = masked_dest | PVR_RAM_BASE_64_P0;
            break;

        case PVR_DMA_VRAM32_SB:
            dest_addr = masked_dest | PVR_RAM_BASE_32_P0;
            break;

        case PVR_DMA_REGISTERS:
            dest_addr = masked_dest | PVR_RAM_BASE_32_P0;
            break;

        default:
            dest_addr = dest;
            break;
    }

    return dest_addr;
}

int pvr_dma_transfer(void *sh4, uintptr_t pvr, size_t count, pvr_dma_mode_t type,
                    int block, int dir, pvr_dma_callback_t callback, void *cbdata) {
    uintptr_t src_addr;

    if(type >= PVR_DMA_VRAM32_SB) {
        pvr_dma_blocking = block;
        pvr_dma_callback = callback;
        pvr_dma_cbdata = cbdata;

        /* Make sure we're not already DMA'ing */
        if(pvr_dma->start != 0) {
            dbglog(DBG_ERROR, "pvr_dma: Previous DMA has not finished\n");
            errno = EINPROGRESS;
            return -1;
        }

        /* Setup PVR DMA */
        pvr_dma->pvr_addr = (uintptr_t)pvr;
        pvr_dma->sh4_addr = (uintptr_t)sh4;
        pvr_dma->size = count;
        pvr_dma->dir = dir;
        pvr_dma->trigger = CPU_TRIGGER;

        /* Start the DMA transfer */
        pvr_dma->enable = 1;         
        pvr_dma->start = 1;          

        /* Block if necessary */
        if(block)
            sem_wait(&pvr_dma_done);
    } else {
        src_addr = ((uintptr_t)sh4);

        /* Check for 32-byte alignment */
        if(src_addr & 0x1F) {
            dbglog(DBG_ERROR, "ta_dma: src is not 32-byte aligned\n");
            errno = EFAULT;
            return -1;
        }

        ta_dma_blocking = block;
        ta_dma_callback = callback;
        ta_dma_cbdata = cbdata;

        /* Make sure we're not already DMA'ing */
        if(ta_dma->start != 0) {
            dbglog(DBG_ERROR, "ta_dma: Previous DMA has not finished\n");
            errno = EINPROGRESS;
            return -1;
        }

        if(DMAC_CHCR2 & 0x1)  /* DE bit set so we must clear it */
            DMAC_CHCR2 &= ~0x1;

        if(DMAC_CHCR2 & 0x2)  /* TE bit set so we must clear it */
            DMAC_CHCR2 &= ~0x2;

        /* Set up DMA transfer */
        DMAC_SAR2 = src_addr;
        DMAC_DMATCR2 = count / 32;
        DMAC_CHCR2 = 0x12c1;

        if((DMAC_DMAOR & DMAOR_STATUS_MASK) != DMAOR_NORMAL_OPERATION) {
            dbglog(DBG_ERROR, "ta_dma: Failed DMAOR check\n");
            errno = EIO;
            return -1;
        }

        /* Start TA DMA */
        ta_dma->dest_addr = pvr_dest_addr(pvr, type);
        ta_dma->size = count;
        ta_dma->start = 1;

        /* Block if necessary */
        if(block)
            sem_wait(&ta_dma_done);
    }
    
    return 0;
}

int pvr_txr_load_dma(void *src, pvr_ptr_t dest, size_t count, int block,
                    pvr_dma_callback_t callback, void *cbdata) {
    return pvr_dma_transfer(src, (uintptr_t)dest, count, PVR_DMA_VRAM64, block, 
                            DIR_NA, callback, cbdata);
}

int pvr_dma_load_ta(void *src, size_t count, int block, 
                    pvr_dma_callback_t callback, void *cbdata) {
    return pvr_dma_transfer(src, (uintptr_t)0, count, PVR_DMA_TA, block, 
                            DIR_NA, callback, cbdata);
}

int pvr_dma_yuv_conv(void *src, size_t count, int block,
                    pvr_dma_callback_t callback, void *cbdata) {
    return pvr_dma_transfer(src, (uintptr_t)0, count, PVR_DMA_YUV, block, 
                            DIR_NA, callback, cbdata);
}

int pvr_dma_load_txr(void *src, pvr_ptr_t pvr, size_t count, int block,
                    pvr_dma_callback_t callback, void *cbdata) {
    return pvr_dma_transfer(src, (uintptr_t)pvr, count, PVR_DMA_VRAM64_SB, block, 
                            PVR_DMA_TO_PVR, callback, cbdata);
}

int pvr_dma_download_txr(void *sh4, pvr_ptr_t pvr, size_t count, int block,
                    pvr_dma_callback_t callback, void *cbdata) {
    return pvr_dma_transfer(sh4, (uintptr_t)pvr, count, PVR_DMA_VRAM64_SB, block, 
                            PVR_DMA_TO_SH4, callback, cbdata);
}

int pvr_dma_load_pal(void *sh4, pvr_ptr_t pvr, size_t count, int block,
                    pvr_dma_callback_t callback, void *cbdata) {
    return pvr_dma_transfer(sh4, (uintptr_t)pvr, count, PVR_DMA_REGISTERS, block, 
                            PVR_DMA_TO_PVR, callback, cbdata);
}
int pvr_dma_ready(dma_type_t dma) {
    return (dma == DMA_TA) ? ta_dma->start == 0 : pvr_dma->start == 0;
}

void pvr_dma_init(void) {
    /* Create an initially blocked semaphore */
    sem_init(&ta_dma_done, 0);
    ta_dma_blocking = 0;
    ta_dma_callback = NULL;
    ta_dma_cbdata = 0;

    /* Use 2x32-bit TA->VRAM buses for PVR_TA_TEX_MEM */
    *ta_dma_lmmode0 = 0;

    /* Use single 32-bit TA->VRAM bus for PVR_TA_TEX_MEM_32 */
    *ta_dma_lmmode1 = 1;

    /* Hook the necessary interrupts for TA DMA */
    asic_evt_set_handler(ASIC_EVT_TA_DMA, ta_dma_irq_hnd, NULL);
    asic_evt_enable(ASIC_EVT_TA_DMA, ASIC_IRQ_DEFAULT);

    /* Create an initially blocked semaphore */
    sem_init(&pvr_dma_done, 0);
    pvr_dma_blocking = 0;
    pvr_dma_callback = NULL;
    pvr_dma_cbdata = 0;

    /* Hook the necessary interrupts for PVR DMA */
    asic_evt_set_handler(ASIC_EVT_PVR_DMA, pvr_dma_irq_hnd, NULL);
    asic_evt_enable(ASIC_EVT_PVR_DMA, ASIC_IRQ_DEFAULT);

    *pvr_dma_pro = PVR_DMA_UNLOCK_ALLMEM;
}

void pvr_dma_shutdown(void) {
    /* Need to ensure that no TA DMA is in progress */
    ta_dma->start = 0;

    /* Clean up for TA DMA */
    asic_evt_disable(ASIC_EVT_TA_DMA, ASIC_IRQ_DEFAULT);
    asic_evt_remove_handler(ASIC_EVT_TA_DMA);
    sem_destroy(&ta_dma_done);

    /* Need to ensure that no PVR DMA is in progress */
    pvr_dma->enable = 0;

    /* Clean up PVR DMA */
    asic_evt_disable(ASIC_EVT_PVR_DMA, ASIC_IRQ_DEFAULT);
    asic_evt_remove_handler(ASIC_EVT_PVR_DMA);
    sem_destroy(&pvr_dma_done);

    *pvr_dma_pro = PVR_DMA_LOCK_ALLMEM;
}

static int check_dma_state(pvr_dma_mode_t type, const char *func_name) {
    if(type >= PVR_DMA_VRAM32_SB && pvr_dma->start != 0) {
        dbglog(DBG_ERROR, "%s: PVR DMA has not finished\n", func_name);
        errno = EINPROGRESS;
        return -1;
    } else if(ta_dma->start != 0) {
        dbglog(DBG_ERROR, "%s: TA DMA has not finished\n", func_name);
        errno = EINPROGRESS;
        return -1;
    }

    return 0;
}

/* Copies n bytes from src to PVR dest, dest must be 32-byte aligned */
void *pvr_sq_load(void *dest, const void *src, size_t n, pvr_dma_mode_t type) {
    void *dma_area_ptr;

    if(check_dma_state(type, "pvr_sq_load") < 0)
        return NULL;

    dma_area_ptr = (void *)pvr_dest_addr((uintptr_t)dest, type);
    sq_cpy(dma_area_ptr, src, n);

    return dest;
}

/* Fills n bytes at PVR dest with 16-bit c, dest must be 32-byte aligned */
void *pvr_sq_set16(void *dest, uint32_t c, size_t n, pvr_dma_mode_t type) {
    void *dma_area_ptr;

    if(check_dma_state(type, "pvr_sq_set16") < 0)
        return NULL;

    dma_area_ptr = (void *)pvr_dest_addr((uintptr_t)dest, type);
    sq_set16(dma_area_ptr, c, n);

    return dest;
}

/* Fills n bytes at PVR dest with 32-bit c, dest must be 32-byte aligned */
void *pvr_sq_set32(void *dest, uint32_t c, size_t n, pvr_dma_mode_t type) {
    void *dma_area_ptr;

    if(check_dma_state(type, "pvr_sq_set32") < 0)
        return NULL;

    dma_area_ptr = (void *)pvr_dest_addr((uintptr_t)dest, type);
    sq_set32(dma_area_ptr, c, n);

    return dest;
}
