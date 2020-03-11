/*
 * Copyright by The HDF Group.
 * Copyright by the Board of Trustees of the University of Illinois.
 * All rights reserved.
 *
 * This file is part of HDF5.  The full HDF5 copyright notice, including
 * terms governing use, modification, and redistribution, is contained in
 * the COPYING file, which can be found at the root of the source code
 * distribution tree, or in https://support.hdfgroup.org/ftp/HDF5/releases.
 * If you do not have access to either file, you may request a copy from
 * help@hdfgroup.org.
 */

#include <err.h>
#include <time.h> /* nanosleep(2) */
#include <unistd.h> /* getopt(3) */

#define H5C_FRIEND              /*suppress error about including H5Cpkg   */
#define H5F_FRIEND              /*suppress error about including H5Fpkg   */

#include "hdf5.h"

#include "H5Cpkg.h"
#include "H5Fpkg.h"
// #include "H5Iprivate.h"
#include "H5HGprivate.h"
#include "H5VLprivate.h"

#include "testhdf5.h"
#include "vfd_swmr_common.h"

enum _step {
  CREATE = 0
, LENGTHEN
, SHORTEN
, DELETE
, NSTEPS
} step_t;

static const hid_t badhid = H5I_INVALID_HID; // abbreviate
static bool caught_out_of_bounds = false;

static void
read_vl_dset(hid_t dset, hid_t type, hid_t space, char *data)
{
    if (H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &data) < 0)
        errx(EXIT_FAILURE, "%s: H5Dwrite", __func__);
}

static hid_t
open_vl_dset(hid_t file, hid_t type, const char *name)
{
    hid_t dset;

    dset = H5Dopen(file, name, H5P_DEFAULT);

    if (dset == badhid)
        errx(EXIT_FAILURE, "H5Dopen");

    return dset;
}

static void
usage(const char *progname)
{
    fprintf(stderr, "usage: %s [-W] [-V]\n", progname);
    fprintf(stderr, "\n  -W: do not wait for SIGUSR1\n");
    fprintf(stderr,   "  -n: number of test steps to perform\n");
    exit(EXIT_FAILURE);
}

bool
H5HG_trap(const char *reason)
{
    if (strcmp(reason, "out of bounds") == 0) {
        caught_out_of_bounds = true;
        return false;
    }
    return true;
}

int
main(int argc, char **argv)
{
    hid_t fapl, fid, space, type;
    hid_t dset[2];
    char content[2][96];
    char name[2][96];
    H5F_vfd_swmr_config_t config;
    sigset_t oldsigs;
    herr_t ret;
    bool wait_for_signal = true;
    const hsize_t dims = 1;
    int ch, i, ntimes = 100;
    unsigned long tmp;
    char *end;
    const struct timespec delay =
        {.tv_sec = 0, .tv_nsec = 1000 * 1000 * 1000 / 10};

    assert(H5T_C_S1 != badhid);

    while ((ch = getopt(argc, argv, "Wn:")) != -1) {
        switch(ch) {
        case 'W':
            wait_for_signal = false;
            break;
        case 'n':
            errno = 0;
            tmp = strtoul(optarg, &end, 0);
            if (end == optarg || *end != '\0')
                errx(EXIT_FAILURE, "couldn't parse `-n` argument `%s`", optarg);
            else if (errno != 0)
                err(EXIT_FAILURE, "couldn't parse `-n` argument `%s`", optarg);
            else if (tmp > INT_MAX)
                errx(EXIT_FAILURE, "`-n` argument `%lu` too large", tmp);
            ntimes = (int)tmp;
            break;
        default:
            usage(argv[0]);
            break;
        }
    }
    argv += optind;
    argc -= optind;

    if (argc > 0)
        errx(EXIT_FAILURE, "unexpected command-line arguments");

    /* Create file access property list */
    if((fapl = h5_fileaccess()) < 0)
        errx(EXIT_FAILURE, "h5_fileaccess");

    /* FOR NOW: set to use latest format, the "old" parameter is not used */
    if(H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) < 0)
        errx(EXIT_FAILURE, "H5Pset_libver_bounds");

    /*
     * Set up to open the file with VFD SWMR configured.
     */

    /* Enable page buffering */
    if(H5Pset_page_buffer_size(fapl, 4096, 100, 0) < 0)
        errx(EXIT_FAILURE, "H5Pset_page_buffer_size");

    memset(&config, 0, sizeof(config));

    config.version = H5F__CURR_VFD_SWMR_CONFIG_VERSION;
    config.tick_len = 1;
    config.max_lag = 5;
    config.writer = true;
    config.md_pages_reserved = 128;
    HDstrcpy(config.md_file_path, "./my_md_file");

    /* Enable VFD SWMR configuration */
    if(H5Pset_vfd_swmr_config(fapl, &config) < 0)
        errx(EXIT_FAILURE, "H5Pset_vfd_swmr_config");

    fid = H5Fopen("vfd_swmr_vlstr.h5", H5F_ACC_RDONLY, fapl);

    /* Create the VL string datatype and a scalar dataspace, or a
     * fixed-length string datatype and a simple dataspace.
     */
    if ((type = H5Tcopy(H5T_C_S1)) == badhid)
        errx(EXIT_FAILURE, "H5Tcopy");

    /* Create the VL string datatype and a scalar dataspace */
    if ((type = H5Tcopy(H5T_C_S1)) == badhid)
        errx(EXIT_FAILURE, "H5Tcopy");

    if (H5Tset_size(type, H5T_VARIABLE) < 0)
        errx(EXIT_FAILURE, "H5Tset_size");
    space = H5Screate(H5S_SCALAR);

    if (space == badhid)
        errx(EXIT_FAILURE, "H5Screate");

    if (fid == badhid)
        errx(EXIT_FAILURE, "H5Fcreate");

    block_signals(&oldsigs);

    /* content 1 seq 1 short
     * content 1 seq 1 long long long long long long long long
     * content 1 seq 1 medium medium medium
     */
    for (i = 0; i < ntimes; i++) {
        const int ndsets = 2;
        const int which = i % ndsets;
        fprintf(stderr, "iteration %d which %d\n", i, which);
        (void)snprintf(name[which], sizeof(name[which]),
            "dset-%d", which);
        dset[which] = open_vl_dset(fid, type, name[which]);
        read_vl_dset(dset[which], type, space, content[which]);
#if 0
        (void)snprintf(content[which], sizeof(content[which]),
            "content %d seq %d %s", which, seq, tail);
#endif
        if (caught_out_of_bounds) {
            fprintf(stderr, "caught out of bounds\n");
            break;
        }
        nanosleep(&delay, NULL);
    }

    if (wait_for_signal)
        await_signal(fid);

    restore_signals(&oldsigs);

    if (H5Pclose(fapl) < 0)
        errx(EXIT_FAILURE, "H5Pclose(fapl)");

    if (H5Tclose(type) < 0)
        errx(EXIT_FAILURE, "H5Tclose");

    if (H5Sclose(space) < 0)
        errx(EXIT_FAILURE, "H5Sclose");

    if (H5Fclose(fid) < 0)
        errx(EXIT_FAILURE, "H5Fclose");

    return EXIT_SUCCESS;
}