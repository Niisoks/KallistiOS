
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#include <kos/gmon.h>
#include <kos/thread.h>

/* GMON file header */
typedef struct gmon_hdr {
    uintptr_t lowpc;
    uintptr_t highpc;
    size_t ncnt;       /* size of sample buffer (plus this header) */
    uint32_t version;  /* version number; 0x00051879 */
    uint32_t profrate; /* profiling clock rate */
    uint32_t spare[3]; /* reserved */
} gmon_hdr_t;

/* GMON arcs */
typedef struct gmon_arc {
    uintptr_t frompc;
    uintptr_t selfpc;
    size_t count;
} gmon_arc_t;

/* GMON profiling states */
typedef enum gmon_state {
    GMON_PROF_ON,
    GMON_PROF_BUSY,
    GMON_PROF_ERROR,
    GMON_PROF_OFF
} gmon_state_t;

/* GMON context */
typedef struct gmon_context {
    gmon_state_t state;
    uintptr_t lowpc;
    uintptr_t highpc;
    uintptr_t pc;
    size_t textsize;
    uint32_t hashfraction;

    size_t narcs;
    gmon_arc_t *arcs;

    size_t nsamples;
    uint32_t *samples;

    kthread_t *histogram_thread;

    volatile bool running_thread;
    volatile bool initialized;
} gmon_context_t;

static gmon_context_t g_context = {
    .state = GMON_PROF_OFF,
    .lowpc = 0,
    .highpc = 0,
    .pc = 0,
    .textsize = 0,
    .hashfraction = 0,
    .narcs = 0,
    .arcs = NULL,
    .nsamples = 0,
    .samples = NULL,
    .histogram_thread = NULL,
    .running_thread = false,
    .initialized = false
};

#define ROUNDDOWN(x,y)	(((x)/(y))*(y))
#define ROUNDUP(x,y)	((((x)+(y)-1)/(y))*(y))

#define MAIN_THREAD_TID        1
#define GMONVERSION            0x00051879
#define HISTOGRAM_INTERVAL_MS  10
#define SAMPLE_FREQ            100 /* 100 Hz (10 ms intervals) */

#define	HISTCOUNTER	unsigned short

/* One histogram per eight bytes of text space */
#define HISTFRACTION    4

#define	HASHFRACTION	2

#define ARCDENSITY	3

#define MINARCS		50

#define MINARCS		50

static void *histogram_callback(void *arg) {
    (void)arg;
    uintptr_t pc;
    size_t index;
    gmon_context_t *cxt = &g_context;

    while(cxt->running_thread) {

        if(cxt->state == GMON_PROF_ON) {
            pc = thd_by_tid(1)->context.pc;//cxt->pc;

            if(pc >= cxt->lowpc && pc <= cxt->highpc) {
                index = (pc - cxt->lowpc) / cxt->hashfraction; // We want these to be shifts aka power of two
                //printf("pc: %p, e: %d\n", (void *)pc, index);
                cxt->samples[index]++;
            }
            else {
                printf("Outside text range pc(histogram_callback): %p\n", (void *)pc);
            }

            thd_sleep(HISTOGRAM_INTERVAL_MS);
        }

    }

    return NULL;
}

void moncontrol(bool mode) {
    gmon_context_t *cxt = &g_context;

    /* Don't change the state if we ran into an error */
    if(cxt->state == GMON_PROF_ERROR)
        return;

    /* Treat start request as stop if error or gmon not initialized */
    if(mode && cxt->initialized) {
        /* Start */
        cxt->state = GMON_PROF_ON;
    } else {
        /* Stop */
        cxt->state = GMON_PROF_OFF;
    }
}

void _mcount(uintptr_t frompc, uintptr_t selfpc) {
    size_t index;
    gmon_context_t *cxt = &g_context;

    if(cxt->state != GMON_PROF_ON)
        return;

    /* Make sure call is from within text section */
    if(frompc >= cxt->lowpc && frompc <= cxt->highpc) {
        cxt->pc = selfpc; /* Save the last place we are */
        index = (frompc - cxt->lowpc) / cxt->hashfraction;
        cxt->arcs[index].frompc = frompc;
        cxt->arcs[index].selfpc = selfpc;
        cxt->arcs[index].count++;
    }
}

void __monstartup(uintptr_t lowpc, uintptr_t highpc) {
    gmon_context_t *cxt = &g_context;

    /* Exit early if we already initialized */
    if(cxt->initialized)
        return;

    cxt->initialized = true;

    cxt->lowpc = ROUNDDOWN(lowpc, HISTFRACTION);
    cxt->highpc = ROUNDUP(highpc, HISTFRACTION);
    cxt->textsize = cxt->highpc - cxt->lowpc;
    cxt->hashfraction = HISTFRACTION;

    cxt->narcs = (cxt->textsize + cxt->hashfraction - 1) / cxt->hashfraction;
    cxt->arcs = (gmon_arc_t *)calloc(cxt->narcs, sizeof(gmon_arc_t));
    if(!cxt->arcs) {
        cxt->state = GMON_PROF_ERROR;
        return;
    }

    cxt->nsamples = (cxt->textsize + cxt->hashfraction - 1) / cxt->hashfraction;
    cxt->samples = (uint32_t *)calloc(cxt->nsamples, sizeof(uint32_t));
    if(!cxt->samples) {
        free(cxt->arcs);
        cxt->arcs = NULL;
        cxt->state = GMON_PROF_ERROR;
        return;
    }

    printf("Low: %p, High: %p\n", (void *)cxt->lowpc, (void *)cxt->highpc);
    printf("Textsize: %d\n", cxt->textsize);
    printf("narcs: %d\n", cxt->narcs);
    printf("nsamples: %d\n", cxt->nsamples);
    printf("[Bytes allocated] Arcs: %d bytes, Histogram: %d bytes, Total: %d\n",
        sizeof(gmon_arc_t) * cxt->narcs,
        cxt->nsamples * sizeof(uint32_t),
        sizeof(gmon_arc_t) * cxt->narcs + cxt->nsamples * sizeof(uint32_t));

    /* Initialize histogram thread related members */
    cxt->running_thread = true;
    cxt->histogram_thread = thd_create(false, histogram_callback, NULL);
    thd_set_prio(cxt->histogram_thread, PRIO_DEFAULT / 2);

    /* Turn profiling on */
    moncontrol(true);
}

void _mcleanup(void) {
    size_t i;
    char *env;
    char buf[64];
    size_t len;
    FILE *out = NULL;
    gmon_hdr_t hdr;
    gmon_context_t *cxt = &g_context;

    /* Stop profiling, stop running histogram thread and join */
    moncontrol(false);
    cxt->running_thread = false;
    thd_join(cxt->histogram_thread, NULL);

    /* Dont output if uninitialized or we encountered an error */
    if(!cxt->initialized || cxt->state == GMON_PROF_ERROR)
        return;

    /* Check if the output file should be named something else */
    env = getenv(GMON_OUT_PREFIX);
    if(env) {
        len = snprintf(buf, sizeof(buf), "%s.%u", env, KOS_PID);
        if(len > 0 && len < sizeof(buf))
            out = fopen(buf, "wb");
    }
    
    /* Fall back to default output file name */
    if(!out) {
        out = fopen("/pc/gmon.out", "wb");
        if(!out) {
            fprintf(stderr, "/pc/gmon.out not opened\n");
            goto cleanup;
        }
    }

    /* Write GMON header */
    memset(&hdr, 0, sizeof(hdr));
    hdr.lowpc = cxt->lowpc;
    hdr.highpc = cxt->highpc;
    hdr.ncnt = sizeof(hdr) + (sizeof(uint32_t) * cxt->nsamples);
    hdr.version = GMONVERSION;
    hdr.profrate = SAMPLE_FREQ;
    hdr.spare[0] = 0;
    hdr.spare[1] = 0;
    hdr.spare[2] = 0;
    fwrite(&hdr, sizeof(hdr), 1, out);

    /* Write Histogram */
    fwrite(cxt->samples, sizeof(uint32_t), cxt->nsamples, out);

    /* Write Arcs */
    for(i = 0; i < cxt->narcs; i++) {
        if (cxt->arcs[i].count > 0)
            fwrite(&cxt->arcs[i], sizeof(gmon_arc_t), 1, out);
    }

    /* Close file */
    fclose(out);

cleanup:
    /* Free the memory */
    free(cxt->arcs);
    free(cxt->samples);

    /* Reset buffer to initial state for safety */
    memset(cxt, 0, sizeof(g_context));
    /* Somewhat confusingly, ON=0, OFF=3 */
    cxt->state = GMON_PROF_OFF;
}
