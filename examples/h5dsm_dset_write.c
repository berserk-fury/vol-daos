#include "h5dsm_example.h"
#include <time.h>

int main(int argc, char *argv[]) {
    uuid_t pool_uuid;
    char *pool_grp = NULL;
    hid_t file = -1, dset = -1, fapl = -1;
    H5_daos_snap_id_t snap_id;
    int buf[4][6];
    int i, j;

    (void)MPI_Init(&argc, &argv);

    /* Seed random number generator */
    srand(time(NULL));

    if(argc < 4 || argc > 5)
        PRINTF_ERROR("argc must be 4 or 5\n");

    /* Parse UUID */
    if(0 != uuid_parse(argv[1], pool_uuid))
        ERROR;

    /* Initialize VOL */
    if(H5daos_init(pool_uuid, pool_grp, getenv("DAOS_SVCL") ? getenv("DAOS_SVCL") : "0") < 0)
        ERROR;

    /* Set up FAPL */
    if((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        ERROR;
    if(H5Pset_fapl_daos(fapl, MPI_COMM_WORLD, MPI_INFO_NULL) < 0)
        ERROR;
    if(H5Pset_all_coll_metadata_ops(fapl, true) < 0)
        ERROR;

    /* Open file */
    if((file = H5Fopen(argv[2], H5F_ACC_RDWR, fapl)) < 0)
        ERROR;

    /* Open dataset */
    if((dset = H5Dopen2(file, argv[3], H5P_DEFAULT)) < 0)
        ERROR;

    /* Fill and print buffer */
    printf("Writing data. Buffer is:\n");
    for(i = 0; i < 4; i++) {
        for(j = 0; j < 6; j++) {
            buf[i][j] = rand() % 10;
            printf("%d ", buf[i][j]);
        }
        printf("\n");
    }

    /* Write data */
    if(H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0)
        ERROR;

#ifdef DV_HAVE_SNAP_OPEN_ID
    /* Save snapshot if requested */
    if(argc == 5) {
        if(H5daos_snap_create(file, &snap_id) < 0)
            ERROR;
        printf("Saved snapshot: snap_id = %llu\n", (long long unsigned)snap_id);
    } /* end if */
#endif

    /* Close */
    if(H5Dclose(dset) < 0)
        ERROR;
    if(H5Fclose(file) < 0)
        ERROR;
    if(H5Pclose(fapl) < 0)
        ERROR;

    printf("Success\n");

    (void)MPI_Finalize();
    return 0;

error:
    H5E_BEGIN_TRY {
        H5Dclose(dset);
        H5Fclose(file);
        H5Pclose(fapl);
    } H5E_END_TRY;

    (void)MPI_Finalize();
    return 1;
}

