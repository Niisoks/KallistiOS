
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#include <kos/gmon.h>
#include <kos/thread.h>

#define ROUNDDOWN(x,y)	(((x)/(y))*(y))
#define ROUNDUP(x,y)	((((x)+(y)-1)/(y))*(y))

#define GMONVERSION            0x00051879
#define HISTOGRAM_INTERVAL_MS  10
#define SAMPLE_FREQ            100 /* 100 Hz (10 ms intervals) */

/* The type that holds the count for the histogram every 10 ms */
#define	HIST_COUNTER_TYPE      uint16_t

/* Each histogram counter represents 16 bytes (4 instructions) */
#define HISTFRACTION  4

/* Practical value for all archs */
#define	HASHFRACTION  2

/* Percentage of the text size we want to allocate for our BST */
#define ARCDENSITY	  2

/* GMON file header */
typedef struct gmon_hdr {
    uintptr_t lowpc;
    uintptr_t highpc;
    size_t ncnt;       /* size of histogram buffer (plus this header) */
    uint32_t version;  /* version number; 0x00051879 */
    uint32_t profrate; /* profiling clock rate */
    uint32_t spare[3]; /* reserved */
} gmon_hdr_t;

typedef struct gmon_node {
    uintptr_t selfpc; /* callee address/program counter. The caller address is in froms[] array which points to tos[] array */
    uint32_t count; /* how many times it has been called */
    uint16_t left; /* link to next entry in hash table. For tos[0] this points to the last used entry */
    uint16_t right; /* additional padding bytes, to have entries 4byte aligned */
} gmon_node_t;

/* GMON arc */
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

    size_t nfroms;
    uint16_t *froms;

    size_t nnodes;
    gmon_node_t *nodes;
    size_t node_count;

    size_t ncounters;
    HIST_COUNTER_TYPE *histogram;

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
    .nfroms = 0,
    .froms = NULL,
    .nnodes = 0,
    .nodes = NULL,
    .node_count = 0,
    .ncounters = 0,
    .histogram = NULL,
    .histogram_thread = NULL,
    .running_thread = false,
    .initialized = false
};

/* Used to write the arcs */
static void traverse_and_write(FILE *out, gmon_context_t *cxt, uint32_t index, uintptr_t from_addr) {
    gmon_arc_t arc;
    gmon_node_t *node;

    if(index == 0)
        return;
    
    /* Grab a node */
    node = &cxt->nodes[index];

    /* Traverse the left subtree */
    traverse_and_write(out, cxt, node->left, from_addr);

    /* Write the arc */
    arc.frompc = from_addr;
    arc.selfpc = node->selfpc;
    arc.count = node->count;
    fwrite(&arc, sizeof(gmon_arc_t), 1, out);

    /* Traverse the right subtree */
    traverse_and_write(out, cxt, node->right, from_addr);
}

static void *histogram_callback(void *arg) {
    (void)arg;
    uintptr_t pc;
    uint32_t index;
    gmon_context_t *cxt = &g_context;

    while(cxt->running_thread) {

        if(cxt->state == GMON_PROF_ON) {
            /* Use the PC that we saved earlier in _mcount() */
            pc = cxt->pc; 

            /* If function is within the .text section */
            if(pc >= cxt->lowpc && pc <= cxt->highpc) {
                /* Compute the index in the histogram */
                index = (pc - cxt->lowpc) / HISTFRACTION;

                /* Increment the histogram counter */
                cxt->histogram[index]++;
            }

            /* Sleep for 10ms before the next sample */
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

/* Called each time we enter a function */
void _mcount(uintptr_t frompc, uintptr_t selfpc) {
    uint32_t index;
    uint16_t node_index;
    uint16_t *index_ptr;
    gmon_node_t *node;
    gmon_context_t *cxt = &g_context;

    if(cxt->state != GMON_PROF_ON)
        return;

    /* If function is within the .text section */
    if(frompc >= cxt->lowpc && frompc <= cxt->highpc) {
        cxt->pc = selfpc; /* Save the last place we are */

        /* Calculate index into 'froms' array */
        index = (frompc - cxt->lowpc) / HASHFRACTION;
        index_ptr = &cxt->froms[index];

        /* Grab index into node array */
	    node_index = *index_ptr;

        /* Node doesnt exist? */
        if(node_index == 0) {
            goto create_node;
        }

        /* Try and find the node */
        node = &cxt->nodes[node_index];

        while(true) {
            /* Found the node */
            if(node->selfpc == selfpc) {
                node->count++;
                return;
            }
            /* Check if selfpc is less */
            else if(selfpc < node->selfpc) {
                if(node->left == 0) {
                    index_ptr = &node->left;
                    goto create_node;
                }

                node = &cxt->nodes[node->left];
            }
            else {
                if(node->right == 0) {
                    index_ptr = &node->right;
                    goto create_node;
                }

                node = &cxt->nodes[node->right];
            }
        }

create_node:
        node_index = ++cxt->node_count;
        if(node_index >= cxt->nnodes)
            /* halt further profiling */
            goto overflow;

        *index_ptr = node_index;
        node = &cxt->nodes[node_index];
        node->selfpc = selfpc;
        node->count = 1;
        node->left = 0;
        node->right = 0;
        return;

overflow:
        cxt->state = GMON_PROF_ERROR;
        //ERR("mcount: call graph buffer size limit exceeded, gmon.out will not be generated\n");
        return;
    }
}

void _monstartup(uintptr_t lowpc, uintptr_t highpc) {
    size_t counter_size = 0;
    size_t froms_size = 0;
    size_t nodes_size = 0;
    gmon_context_t *cxt = &g_context;

    /* Exit early if we already initialized */
    if(cxt->initialized)
        return;

    cxt->lowpc = ROUNDDOWN(lowpc, HISTFRACTION * sizeof(HIST_COUNTER_TYPE));
    cxt->highpc = ROUNDUP(highpc, HISTFRACTION * sizeof(HIST_COUNTER_TYPE));
    cxt->textsize = cxt->highpc - cxt->lowpc;

    /* Calculate the number of counters, rounding up to ensure no remainder,
       ensuring that all of textsize is covered without any part being left out */
    cxt->ncounters = (cxt->textsize + HISTFRACTION - 1) / HISTFRACTION;
    counter_size = cxt->ncounters * sizeof(HIST_COUNTER_TYPE);

    cxt->nfroms = (cxt->textsize + HASHFRACTION - 1) / HASHFRACTION;
    froms_size = cxt->nfroms * sizeof(uint16_t);

    cxt->nnodes = (cxt->textsize * ARCDENSITY) / 100;
    nodes_size = cxt->nnodes * sizeof(gmon_node_t);

    printf("Low: %p, High: %p\n", (void *)cxt->lowpc, (void *)cxt->highpc);
    printf("Textsize: %d\n", cxt->textsize);
    printf("nfroms: %d\n", cxt->nfroms);
    printf("nnodes: %d\n", cxt->nnodes);
    printf("ncounters: %d\n", cxt->ncounters);
    printf("[Bytes allocated] Froms: %d bytes, Nodes: %d bytes, Histogram: %d bytes, Total: %d\n",
        froms_size,
        nodes_size,
        counter_size,
        counter_size + froms_size + nodes_size);

    if(posix_memalign((void**)&cxt->histogram, 32, counter_size + froms_size + nodes_size)) {
        cxt->state = GMON_PROF_ERROR;
        fprintf(stderr, "_monstartup: Unable to allocate memory.\n");
        return;
    }

    memset(cxt->histogram, 0, counter_size + froms_size + nodes_size);
    cxt->froms = (uint16_t *)((uintptr_t)cxt->histogram + counter_size);
    cxt->nodes = (gmon_node_t *)((uintptr_t)cxt->froms + froms_size);

    printf("%p %p %p\n", (void *)cxt->histogram, (void *)cxt->froms, (void *)cxt->nodes);

    /* Initialize histogram thread related members */
    cxt->running_thread = true;
    cxt->histogram_thread = thd_create(false, histogram_callback, NULL);
    thd_set_prio(cxt->histogram_thread, PRIO_DEFAULT / 2);

    cxt->initialized = true;

    /* Turn profiling on */
    moncontrol(true);
}

void _mcleanup(void) {
    char *env;
    char buf[64];
    size_t len;
    FILE *out = NULL;
    gmon_hdr_t hdr;
    uintptr_t from_addr;
    uint32_t from_index;
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
    hdr.ncnt = sizeof(hdr) + (sizeof(HIST_COUNTER_TYPE) * cxt->ncounters);
    hdr.version = GMONVERSION;
    hdr.profrate = SAMPLE_FREQ;
    hdr.spare[0] = 0;
    hdr.spare[1] = 0;
    hdr.spare[2] = 0;
    fwrite(&hdr, sizeof(hdr), 1, out);

    /* Write Histogram */
    fwrite(cxt->histogram, sizeof(HIST_COUNTER_TYPE), cxt->ncounters, out);

    /* Write Arcs */
    for(from_index = 0; from_index < cxt->nfroms; from_index++) {
        /* Skip if no BST built for this index */
        if(cxt->froms[from_index] == 0)
	        continue;
        
        /* Construct from address by performing reciprical of hash to insert */
        from_addr = (HASHFRACTION * from_index) + cxt->lowpc;

        /* Write out the arcs in the BST */
        traverse_and_write(out, cxt, cxt->froms[from_index], from_addr);
    }

    /* Close file */
    fclose(out);

cleanup:
    /* Free the memory */
    free(cxt->histogram);

    /* Reset buffer to initial state for safety */
    memset(cxt, 0, sizeof(g_context));

    /* Somewhat confusingly, ON=0, OFF=3 */
    cxt->state = GMON_PROF_OFF;
}
