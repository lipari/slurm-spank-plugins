/*****************************************************************************
 *
 *  Copyright (C) 2007-2015 Lawrence Livermore National Security, LLC.
 *  Produced at Lawrence Livermore National Laboratory.
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
 *             Edgar A. Leon Borja
 *             Don Lipari
 *
 *  UCRL-CODE-235358
 *
 *  This file is part of chaos-spankings, a set of spank plugins for SLURM.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ctype.h>
#include <hwloc.h>

#include <slurm/slurm.h>
#include <slurm/spank.h>


SPANK_PLUGIN(mpibind, 1);

static const char mpibind_help [] =
"\
mpibind: Automatically assign CPU and GPU affinity using best-guess defaults.\n\
\n\
The default behavior attempts to bind MPI tasks to specific processing\n\
units.  If OMP_NUM_THREADS is set, each thread will be similarly bound\n\
to a processing unit.\n\
\n\
Option Usage: --mpibind[=args...]\n\
  where args... is a period (.) separated list of one or more of the\n\
  following options:\n\
\n\
  help              Display this message\n\
  w                 Show warnings of potential problems\n\
  v(erbose)         Show warnings and more verbose info\n\
  vv                Show warnings and verbose debugging info\n\
  <range>           Restrict the application to specific cores, e.g., 0-7\n\
  off               Disable binding\n\
  on                Enable binding (used when the system default is off)\n\
\n\
The above options can also be specified in the environment variable: MPIBIND\n\
E.g., MPIBIND=w.0-9\n\
\n\n";


/*****************************************************************************
 *
 *  Global mpibind variables
 *
 ****************************************************************************/

static hwloc_topology_t topology;
static int32_t disabled = 0;       /* True if disabled by --mpibind=off       */
static int32_t enabled = 1;        /* True if enabled by configuration        */
static int32_t verbose = 0;
static uint32_t cpus = 0;          /* a bitmap of <range> specified cores     */
static uint32_t level_size = 0;    /* number of processing units available    */
static uint32_t local_rank = 0;    /* rank relative to this node              */
static uint32_t local_size = 0;    /* number of tasks to run on this node     */
static uint32_t local_threads = 0; /* number of threads to run on this node   */
static uint32_t num_cores = 0;     /* number of physical cores available      */
static uint32_t num_threads = 0;
static uint32_t rank = 0;

/*****************************************************************************
 *
 *  Forward declarations
 *
 ****************************************************************************/

static int32_t parse_user_option (int32_t val, const char *optarg,
                                  int32_t remote);


/*****************************************************************************
 *
 *  SPANK plugin options:
 *
 ****************************************************************************/

struct spank_option spank_options [] = {
    { "mpibind", "[args]",
      "Automatic, best guess CPU affinity for SMP machines "
      "(args=`help' for more info)",
      2, 0, (spank_opt_cb_f) parse_user_option
    },
    SPANK_OPTIONS_TABLE_END
};


/*****************************************************************************
 *
 *  Utility functions
 *
 ****************************************************************************/

static int parse_option (const char *opt, int32_t remote)
{
    char *endptr = NULL;
    int32_t i, rc = 0;
    int64_t start;
    int64_t end;

    if (strncmp (opt, "on", 3) == 0)
        disabled = 0;
    else if (strncmp (opt, "off", 4) == 0)
        disabled = 1;
    else if (!strncmp (opt, "vv", 3)) {
        verbose = 3;
        if (remote)
            slurm_debug2 ("mpibind: setting 'vv' verbosity");
        else
            printf ("mpibind: setting 'vv' verbosity\n");
    } else if (!strncmp (opt, "v", 2) || !strncmp (opt, "verbose", 8))
        verbose = 2;
    else if (!strncmp (opt, "w", 2))
        verbose = 1;
    else if (isdigit (opt[0])) {
        level_size = 0;
        while (opt[0]) {
            start = strtol (opt, &endptr, 10);
            if (endptr[0]) {
                if (!strncmp (endptr, "-", 1)) {
                    opt = endptr + 1;
                    if (opt[0]) {
                        end = strtol (opt, &endptr, 10);
                        for (i = start; i <= end; i++) {
                            cpus |= 1 << i;
                            level_size++;
                        }
                        opt = endptr;
                    } else {
                        rc = -1;
                        goto ret;
                    }
                } else if (!strncmp (endptr, ",", 1)) {
                    cpus |= 1 << start;
                    level_size++;
                    opt = endptr + 1;
                } else {
                    rc = -1;
                    goto ret;
                }
            } else {
                cpus |= 1 << start;
                level_size++;
                break;
            }
        }
        if (verbose > 1) {
            if (remote)
                slurm_debug ("mpibind: cpus is 0x%x", cpus);
            else
                printf ("mpibind: cpus is 0x%x\n", cpus);
        }
    } else if ((strncmp (opt, "help", 5) == 0) && !remote) {
        fprintf (stderr, mpibind_help);
        exit (0);
    } else {
        fprintf (stderr, "mpibind: invalid option: %s\n", opt);
        rc = -1;
    }
ret:
    return rc;
}

static int parse_user_option (int32_t val, const char *arg, int32_t remote)
{
    char *dot = NULL;
    char *opt;
    int32_t rc = -1;

    if (arg == NULL)
        return (0);

    opt = strdup (arg);
    while ((dot = strstr (opt, "."))) {
        *dot = '\0';
        if (parse_option (opt, remote))
            goto ret;
        opt = dot + 1;
    }
    if (parse_option (opt, remote))
        goto ret;

    rc = 0;
ret:
    return rc;
}

static int get_local_env ()
{
    char *val = NULL;
    int32_t rc = -1;

    if (disabled)
        return 0;

    if ((val = getenv ("MPIBIND"))) {
        if (verbose > 1)
            printf ("mpibind: processing MPIBIND=%s\n", val);
        /* This next call is essentially a validation exercise.  The
         * MPIBIND options will be parsed and validated and the user
         * will be informed or alerted at their requested
         * verbosity. The actual options specified in MPIBIND will be
         * processed in get_remote_env(). */
        rc = parse_user_option (0, val, 0);
    } else {
        rc = 0;
    }

    /* Need the number of threads for the 'mem' policy */
    if ((val = getenv ("OMP_NUM_THREADS"))) {
        num_threads = strtol (val, NULL, 10);
        if (verbose > 1)
            printf ("mpibind: found OMP_NUM_THREADS=%u\n", num_threads);
    } else {
        /* for this case, num_threads will serve only to indicate
         * that OMP_NUM_THREADS was not set */
        num_threads = 0;
        if (verbose)
            printf ("mpibind: OMP_NUM_THREADS not defined\n");
    }

    return rc;
}

static int get_remote_env (spank_t sp)
{
    char  val[64];
    int32_t rc = -1;

    if (disabled)
        return 0;

    /* Turn off verbosity for all but rank 0 */
    if ((spank_get_item (sp, S_TASK_ID, &rank) == ESPANK_SUCCESS)) {
        if (rank)
            verbose = 0;
    } else {
        slurm_error ("mpibind: Failed to retrieve global rank from environment");
        goto ret;
    }

    if ((spank_getenv (sp, "OMPI_COMM_WORLD_LOCAL_RANK", val, sizeof (val)) ==
         ESPANK_SUCCESS) ||
        (spank_getenv (sp, "SLURM_LOCALID", val, sizeof (val)) ==
         ESPANK_SUCCESS)) {
        local_rank = strtol (val, NULL, 10);
        if (verbose > 1)
            slurm_error ("mpibind: retrieved local rank %u", local_rank);
    } else {
        slurm_error ("mpibind: Failed to retrieve local rank from environment");
        goto ret;
    }

    if (spank_get_item (sp, S_JOB_LOCAL_TASK_COUNT, &local_size) ==
        ESPANK_SUCCESS) {
        if (verbose > 1)
            slurm_error ("mpibind: retrieved local size %u", local_size);
    } else {
        slurm_error ("mpibind: Failed to retrieve local size from environment");
        goto ret;
    }

    /* Need the number of threads for the 'mem' policy */
    if (spank_getenv (sp, "OMP_NUM_THREADS", val, sizeof (val)) ==
        ESPANK_SUCCESS) {
        num_threads = strtol (val, NULL, 10);
        if (verbose > 1)
            slurm_error ("mpibind: found OMP_NUM_THREADS=%u", num_threads);
    } else {
        /* for this case, num_threads will serve only to indicate
         * that OMP_NUM_THREADS was not set */
        num_threads = 0;
        if (verbose)
            slurm_error ("mpibind: OMP_NUM_THREADS not defined");
    }

    if (spank_getenv (sp, "MPIBIND", val, sizeof (val)) == ESPANK_SUCCESS) {
        if (verbose > 1)
            slurm_error ("mpibind: processing MPIBIND=%s", val);
        rc = parse_user_option (0, val, 1);
    } else {
        rc = 0;
    }
ret:
    return rc;
}

/*
 * str2int () is specialized to parse the SLURM_JOB_CPUS_PER_NODE
 * value.  The format of the value can be:
 *   a simple integer
 *   a replicated integer, example:  36(x2)
 *   a comma delimted combo of the both of the above, example: 20,13,1(x2)
 *
 * This function will return the simple interger if the value is one
 * of the first two forms, but -1 if it is the third.  The rationalle
 * is that we cannot accurately bind when not all of a node's CPUS
 * have been allocated to the job.
 */
static int32_t str2int (const char *str)
{
    char *p;
    long l = -1;

    errno = 0;
    l = strtol (str, &p, 10);

    if (errno || (p && (*p != '(') && (*p != '\0')))
        return (-1);

    return ((int32_t) l);
}

/*
 *  Return 1 if job has allocated sufficient CPUs on this node
 */
static int job_is_exclusive (spank_t sp)
{
    char val[16];
    int32_t n;

    if (spank_getenv (sp, "SLURM_JOB_CPUS_PER_NODE", val, sizeof (val)) !=
        ESPANK_SUCCESS) {
        if (verbose)
            fprintf (stderr, "mpibind: failed to find SLURM_JOB_CPUS_PER_NODE "
                     "in env\n");
        return (0);
    } else if ((n = str2int (val)) < 0) {
        if (verbose)
            fprintf (stderr, "mpibind: disabled for SLURM_JOB_CPUS_PER_NODE=%s"
                     "\n", val);
        return (0);
    }

    return (n >= level_size);
}

/*
 *  Return 1 if this step is a batch script
 */
static int job_step_is_batch (spank_t sp)
{
    uint32_t stepid;

    if (spank_get_item (sp, S_JOB_STEPID, &stepid) != ESPANK_SUCCESS) {
        slurm_error ("mpibind: failed to get job stepid!");
        return (0);
    }

    if (stepid == 0xfffffffe)
        return (1);
    return (0);
}

static void display_cpubind (char *message)
{
    char *str = NULL;
    hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();

    if (!hwloc_get_cpubind (topology, cpuset, 0)) {
        hwloc_bitmap_asprintf (&str, cpuset);
        slurm_error ("mpibind: %s %s", message, str);
        hwloc_bitmap_free (cpuset);
        free (str);
    }
}

/*
 * decimate_cpuset() reduces a cpuset down to one processing unit per
 * thread.  All unneded processing units' bits are cleared.
 */
static void decimate_cpuset (hwloc_cpuset_t cpuset)
{
    int bits = hwloc_bitmap_weight (cpuset);
    int i;
    uint32_t thread = num_threads;

    if (bits <= num_threads)
        return;

    i = hwloc_bitmap_first (cpuset);
    do {
        if (thread)
            thread--;
        else
            hwloc_bitmap_clr (cpuset, i);
        i = hwloc_bitmap_next (cpuset, i);
    } while (i > 0);
}

/* Assumes an equal number of gpus per numa node AND that the gpu id's
 * are ordered by increasing numa node ids, e.g.,
 *  GPU 0,1 go with numanode 0
 *  GPU 2,3 go with numanode 1
 */
static void decimate_gpusets (hwloc_cpuset_t *gpusets, uint32_t numaobjs,
                              uint32_t gpus)
{
    uint32_t numa, gpu;
    uint32_t gpuspernuma;
    uint32_t bits, bitspergpu;
    uint32_t start, end;
    uint32_t startbit, endbit;
    hwloc_bitmap_t bit_mask = hwloc_bitmap_alloc();

    if (!(gpusets && numaobjs && gpus))
        goto ret;
    gpuspernuma = gpus / numaobjs;

    for (numa = 0; numa < numaobjs; numa++) {
        start = numa * gpuspernuma;
        end = start + gpuspernuma;
        for (gpu = start; gpu < end; gpu++) {
            hwloc_bitmap_zero (bit_mask);
            bits = hwloc_bitmap_weight (gpusets[gpu]);
            bitspergpu = bits / gpuspernuma;
            startbit = hwloc_bitmap_first (gpusets[gpu]) +
                (gpu - start) * bitspergpu;
            endbit = startbit + bitspergpu - 1;
            hwloc_bitmap_set_range (bit_mask, startbit, endbit);
            hwloc_bitmap_and (gpusets[gpu], gpusets[gpu], bit_mask);
            if (!local_rank && verbose > 2) {
                char *str = NULL;
                hwloc_bitmap_asprintf (&str, gpusets[gpu]);
                slurm_error ("mpibind: GPU %u has cpuset %s", gpu, str);
                free (str);
            }
        }
    }
ret:
    hwloc_bitmap_free (bit_mask);
    return;
}

static char *get_gomp_str (hwloc_cpuset_t cpuset)
{
    char *str = NULL;
    int32_t i, j, rc;

    i = hwloc_bitmap_first (cpuset);
    j = num_threads;

    while ((i != -1) && (j > 0)) {
        if (str)
            rc = asprintf (&str, "%s,%d", str, i);
        else
            rc = asprintf (&str, "%d", i);
        if (rc < 0) {
            str = NULL;
            break;
        }
        i = hwloc_bitmap_next (cpuset, i);
        j--;
    }

    return str;
}

static char *get_cuda_str (int32_t gpus, uint32_t gpu_bits)
{
    char *str = NULL;
    int32_t i, rc;

    for (i = 0; i < gpus; i++) {
        if ((1 << i) & gpu_bits) {
            if (str)
                rc = asprintf (&str, "%s,%d", str, i);
            else
                rc = asprintf (&str, "%d", i);
            if (rc < 0) {
                str = NULL;
                break;
            }
        }
    }

    return str;
}

/*****************************************************************************
 *
 *  SPANK callback functions:
 *
 ****************************************************************************/

int slurm_spank_init (spank_t sp, int ac, char **av)
{
    int i;
    uint32_t hwloc_version = hwloc_get_api_version ();

    if (hwloc_version != HWLOC_API_VERSION) {
        if (verbose)
            slurm_error ("mpibind plugin written for hwloc API 0x%x but running"
                         "with hwloc library 0x%x", HWLOC_API_VERSION,
                         hwloc_version);
    }

    if (!spank_remote (sp))
        return (0);

    for (i = 0; i < ac; i++) {
        if (strncmp ("mpibind=", av[i], 8) == 0) {
            const char *opt = av[i] + 8;
            if (strncmp (opt, "off", 4) == 0)
                disabled = 1;
            else
                slurm_error ("mpibind: ignoring invalid option \"%s\"", av[i]);
            break;
        } else {
            slurm_error ("mpibind: ignoring invalid option \"%s\"", av[i]);
        }
    }

    return (0);
}

int slurm_spank_init_post_opt (spank_t sp, int32_t ac, char **av)
{
    if (!spank_remote (sp))
        return (get_local_env ());

    return (0);
}

/*
 *  Use the slurm_spank_user_init callback to check for exclusivity
 *   because user options are processed prior to calling here.
 *   Otherwise, we would not be able to use the `verbose' flag.
 */
int slurm_spank_user_init (spank_t sp, int32_t ac, char **av)
{
    if (!spank_remote (sp))
        return (0);

    /*  Enable mpibind operation only if we make it through the
     *   following checks.
     */
    enabled = 0;

    /*
     *  In some versions of SLURM, batch script job steps appear as if
     *   the user explicitly set --cpus-per-task, and this may cause
     *   unexpected behavior. It is much safer to just disable mpibind
     *   behavior for batch scripts.
     */
    if (job_step_is_batch (sp))
        return (0);

    if (!job_is_exclusive (sp)) {
        if (verbose)
            fprintf (stderr, "mpibind: Disabling. "
                     "(job doesn't have exclusive access to this node)\n");
        return (0);
    }
    enabled = 1;

    /* Allocate and initialize topology object. */
    hwloc_topology_init (&topology);

    hwloc_topology_set_flags (topology, HWLOC_TOPOLOGY_FLAG_IO_DEVICES);

    /* Perform the topology detection. */
    hwloc_topology_load (topology);

    return (0);
}

int slurm_spank_task_init (spank_t sp, int32_t ac, char **av)
{
    char *str;
    float num_pus_per_task;
    hwloc_cpuset_t *cpusets = NULL;
    hwloc_cpuset_t *gpusets = NULL;
    hwloc_cpuset_t cpuset;
    hwloc_obj_t obj;
    int32_t gpus = 0;
    int32_t i;
    int32_t index;
    int32_t numaobjs;
    uint32_t gpu_bits = 0;

    if (!spank_remote (sp))
        return (0);

    get_remote_env (sp);

    if (!enabled || disabled)
        return (0);

    if (verbose > 1) {
        display_cpubind ("starting binding");
    }

    local_threads = local_size;
    if (num_threads)
        local_threads *= num_threads;

    cpuset = hwloc_bitmap_alloc();

    /*
     * The following creates an array of cpusets, one for each
     * processing unit required.
     *
     * If the user has specified specific cores using the <range>
     * option, 'cpus' will contain a bit map of those cores.  In that
     * case, restrict the application to specific cores by only
     * populating the cpuset array for those cores.
     *
     * Otherwise, search the resources by depth.  Descend only as far
     * as we need to obtain at least one processing unit for each
     * thread.
     */

    if (cpus) {
        int32_t coreobjs = hwloc_get_nbobjs_by_type (topology, HWLOC_OBJ_CORE);
        int j = 0;

        /* level_size has been set in process_opt() */
        num_cores = level_size;
        cpusets = calloc (level_size, sizeof (hwloc_cpuset_t));

        for (i = 0; i < coreobjs; i++) {
            if (cpus & (1 << i)) {
                obj = hwloc_get_obj_by_type (topology, HWLOC_OBJ_CORE, i);
                if (obj) {
                    cpusets[j] = hwloc_bitmap_dup (obj->cpuset);
                } else {
                    slurm_error ("mpibind: failed to get core %d", i);
                    return (ESPANK_ERROR);
                }
                j++;
            }
        }
    } else {
        uint32_t depth;
        uint32_t topodepth = hwloc_topology_get_depth (topology);
        num_cores = hwloc_get_nbobjs_by_type (topology, HWLOC_OBJ_CORE);

        for (depth = 0; depth < topodepth; depth++) {
            level_size = hwloc_get_nbobjs_by_depth (topology, depth);
            if (level_size >= local_threads)
                break;
        }
        if (depth == topodepth)
            depth--;

        cpusets = calloc (level_size, sizeof (hwloc_cpuset_t));

        for (i = 0; i < level_size; i++) {
            obj = hwloc_get_obj_by_depth (topology, depth, i);
            if (obj) {
                cpusets[i] = hwloc_bitmap_dup (obj->cpuset);
            } else {
                slurm_error ("mpibind: failed to get object %d at depth %d", i,
                             depth);
                return (ESPANK_ERROR);
            }
        }
    }

    for (obj = hwloc_get_next_osdev (topology, NULL); obj;
         obj = hwloc_get_next_osdev (topology, obj)) {
        if (!strncmp (obj->name, "ib0", 3)) {
            /* NIC Affinity support goes here */
        }
    }

    /* count the GPUS */
    for (obj = hwloc_get_next_osdev (topology, NULL); obj;
         obj = hwloc_get_next_osdev (topology, obj)) {
        if (obj->attr->osdev.type == HWLOC_OBJ_OSDEV_GPU) {
            gpus++;
        }
    }

    if (gpus) {
        gpusets = calloc (gpus, sizeof (hwloc_cpuset_t));
        gpus = 0;
        for (obj = hwloc_get_next_osdev (topology, NULL); obj;
             obj = hwloc_get_next_osdev (topology, obj)) {
            if (obj->attr->osdev.type == HWLOC_OBJ_OSDEV_GPU) {
                hwloc_obj_t ancestor;
#if HWLOC_API_VERSION < 0x00010b00
                ancestor = hwloc_get_ancestor_obj_by_type (topology,
                                                          HWLOC_OBJ_NODE, obj);
#else
                ancestor = hwloc_get_ancestor_obj_by_type (topology,
                                                        HWLOC_OBJ_NUMANODE, obj);
#endif
                if (!ancestor)
                    /* The parent of GPUs on KNL nodes may be the
                     * machine instead of a NUMA node*/
                    ancestor = hwloc_get_ancestor_obj_by_type (topology,
                                                        HWLOC_OBJ_MACHINE, obj);
                if (ancestor) {
                    gpusets[gpus] = hwloc_bitmap_dup (ancestor->cpuset);
                    gpus++;
                } else {
                    slurm_error ("mpibind: failed to find ancestor of GPU obj");
                    return (ESPANK_ERROR);
                }
            }
        }
#if HWLOC_API_VERSION < 0x00010b00
        numaobjs = hwloc_get_nbobjs_by_type (topology, HWLOC_OBJ_NODE);
#else
        numaobjs = hwloc_get_nbobjs_by_type (topology, HWLOC_OBJ_NUMANODE);
#endif
        decimate_gpusets (gpusets, numaobjs, gpus);
    }

    /*
     * num_pus_per_task will be < 1.0 when pu's are over-committed.
     */
    num_pus_per_task = (float) level_size / local_size;

    if (!local_rank && verbose > 2)
        slurm_error ("mpibind: level size: %u, local size: %u, pus per task %f",
                     level_size, local_size, num_pus_per_task);

    /*
     * If the user did not set it, set the OMP_NUM_THREADS environment
     * variable to the number of cores this task will have.
     */
    if (!num_threads) {
        int rc;
        num_threads = num_cores / local_size;
        if (!num_threads)
            num_threads = 1;
        rc = asprintf (&str, "%u", num_threads);
        if (rc > 0) {
            spank_setenv (sp, "OMP_NUM_THREADS", str, 0);
            if (verbose > 2)
                slurm_error ("mpibind: setting OMP_NUM_THREADS to %s", str);
            free (str);
        } else if (verbose)
	  slurm_error ("mpibind: failed to set OMP_NUM_THREADS");
    }

    /*
     * Create the cpuset to which this task will be bound.  The
     * resulting cpuset will be the union of cpusets[] elements.
     *
     * Note: num_pus_per_task is a float value.  It allows us to
     * select cpusets[] elements that span the full range of available
     * cpusets[].  Otherwise there could be an uneven distribution of
     * tasks to NUMA nodes for example.
     */
    index = (int32_t) (local_rank * num_pus_per_task);
    if (num_pus_per_task < 1.0)
        num_pus_per_task = 1.0;

    for (i = index; i < index + (int32_t) num_pus_per_task; i++) {
        hwloc_bitmap_or (cpuset, cpuset, cpusets[i]);
        if (gpus) {
            int32_t j;
            hwloc_bitmap_t result = hwloc_bitmap_alloc();

            for (j = 0; j < gpus; j++) {
                hwloc_bitmap_and (result, cpusets[i], gpusets[j]);
                if (!hwloc_bitmap_iszero (result)) {
                    gpu_bits |= (1 << j);
                }
            }
            hwloc_bitmap_free (result);
        }
    }

    if (verbose) {
        /* An MPI task with threads should not span more than one NUMA domain */
#if HWLOC_API_VERSION < 0x00010b00
        numaobjs = hwloc_get_nbobjs_inside_cpuset_by_type (topology, cpuset,
                                                           HWLOC_OBJ_NODE);
#else
        numaobjs = hwloc_get_nbobjs_inside_cpuset_by_type (topology, cpuset,
                                                           HWLOC_OBJ_NUMANODE);
#endif
        if ((local_size < numaobjs) && (num_threads > 1)) {
            slurm_error ("mpibind: rank %d spans %d NUMA domains",
                         local_rank, numaobjs);
        }
    }

    if (num_threads == 1)
        hwloc_bitmap_singlify (cpuset);
    else
        decimate_cpuset (cpuset);

    hwloc_bitmap_asprintf (&str, cpuset);
    if (verbose > 2)
        slurm_error ("mpibind: resulting cpuset %s", str);

    if (hwloc_set_cpubind (topology, cpuset, 0)) {
        if (verbose)
            slurm_error ("mpibind: could not bind to cpuset %s: %s", str,
                         strerror (errno));
    } else if (verbose > 2) {
        slurm_error ("mpibind: bound cpuset %s", str);
    }
    free (str);

    /*
     * Construct the list of cpus to which each thread will be bound
     * and assign it to the GOMP_CPU_AFFINITY environment variable.
     */
    if ((str = get_gomp_str (cpuset))) {
        spank_setenv (sp, "GOMP_CPU_AFFINITY", str, 1);
        if (verbose > 1)
            slurm_error ("mpibind: GOMP_CPU_AFFINITY=%s", str);
        free (str);
    }

    /*
     * Perform an analogous population of the CUDA_VISIBLE_DEVICES
     * environment variable that we did for GOMP_CPU_AFFINITY above.
     */
    if (gpus) {
        if  ((str = get_cuda_str (gpus, gpu_bits))) {
            spank_setenv (sp, "CUDA_VISIBLE_DEVICES", str, 1);
            if (verbose > 1)
                slurm_error ("mpibind: CUDA_VISIBLE_DEVICES=%s", str);
            free (str);
        }

        /* Free our gpusets */
        for (i = 0; i < gpus; i++) {
            hwloc_bitmap_free (gpusets[i]);
        }
        free (gpusets);
    }

    if (verbose > 1) {
        display_cpubind ("resulting binding");
    }

    /* Free our cpusets */
    for (i = 0; i < level_size; i++) {
        hwloc_bitmap_free (cpusets[i]);
    }
    free (cpusets);
    hwloc_bitmap_free (cpuset);

    /* Destroy topology object. */
    hwloc_topology_destroy (topology);

    return (0);
}


/*
 * vi: ts=4 sw=4 expandtab
 */
