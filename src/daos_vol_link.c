/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of the HDF5 DAOS VOL connector. The full copyright      *
 * notice, including terms governing use, modification, and redistribution,  *
 * is contained in the COPYING file, which can be found at the root of the   *
 * source code distribution tree.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose: The DAOS VOL connector where access is forwarded to the DAOS
 * library. Link routines.
 */

#include "daos_vol.h"           /* DAOS connector                          */

#include "util/daos_vol_err.h"  /* DAOS connector error handling           */
#include "util/daos_vol_mem.h"  /* DAOS connector memory management        */

/****************/
/* Local Macros */
/****************/

#define H5_DAOS_HARD_LINK_VAL_SIZE (H5_DAOS_ENCODED_OID_SIZE + 1)
#define H5_DAOS_RECURSE_LINK_PATH_BUF_INIT 1024

/*
 * Given an H5_daos_link_val_t and the link's type, encodes
 * the link's value into the given buffer.
 */
#define H5_DAOS_ENCODE_LINK_VALUE(link_val_buf, link_val_buf_size, link_val, link_type) \
H5_DAOS_ENCODE_LINK_VALUE_##link_type(link_val_buf, link_val_buf_size, link_val, link_type)

#define H5_DAOS_ENCODE_LINK_VALUE_H5L_TYPE_HARD(link_val_buf, link_val_buf_size, link_val, link_type) \
do {                                                                                                  \
    uint8_t *p = link_val_buf;                                                                        \
                                                                                                      \
    assert(H5_DAOS_HARD_LINK_VAL_SIZE <= link_val_buf_size);                                          \
                                                                                                      \
    /* Encode link type */                                                                            \
    *p++ = (uint8_t)H5L_TYPE_HARD;                                                                    \
                                                                                                      \
    /* Encode OID */                                                                                  \
    UINT64ENCODE(p, link_val.target.hard.lo)                                                          \
    UINT64ENCODE(p, link_val.target.hard.hi)                                                          \
} while(0)

#define H5_DAOS_ENCODE_LINK_VALUE_H5L_TYPE_SOFT(link_val_buf, link_val_buf_size, link_val, link_type) \
do {                                                                                                  \
    uint8_t *p = link_val_buf;                                                                        \
                                                                                                      \
    /* Encode link type */                                                                            \
    *p++ = (uint8_t)H5L_TYPE_SOFT;                                                                    \
                                                                                                      \
    /* Copy target name */                                                                            \
    (void)memcpy(p, link_val.target.soft, link_val_buf_size - 1);                                     \
} while(0)

/*
 * Given an H5_daos_link_val_t, uses this to fill out the
 * link type and link object address (hard link) or link
 * value size (soft/external link) fields of an H5L_info2_t
 * for a link.
 */
#define H5_DAOS_LINK_VAL_TO_INFO(LINK_VAL, LINK_INFO, ERR)                                 \
do {                                                                                       \
    (LINK_INFO).type = (LINK_VAL).type;                                                        \
    if((LINK_VAL).type == H5L_TYPE_HARD) {                                                   \
        if(H5_daos_oid_to_token((LINK_VAL).target.hard, &(LINK_INFO).u.token) < 0)             \
            D_GOTO_ERROR(H5E_SYM, H5E_CANTGET, ERR, "can't get link target object token"); \
    }                                                                                      \
    else {                                                                                 \
        assert((LINK_VAL).type == H5L_TYPE_SOFT);                                            \
        (LINK_INFO).u.val_size = strlen((LINK_VAL).target.soft) + 1;                           \
    }                                                                                      \
} while(0)

/************************************/
/* Local Type and Struct Definition */
/************************************/

/* Task user data for reading a link from a group */
typedef struct H5_daos_link_read_ud_t {
    H5_daos_md_rw_cb_ud_t md_rw_cb_ud; /* Must be first */
    const char **name;
    size_t *name_len;
    H5_daos_link_val_t *link_val;
    hbool_t *link_read; /* Whether the link exists */
    tse_task_t *read_metatask;
} H5_daos_link_read_ud_t;

/* Task user data for writing a link to a group */
typedef struct H5_daos_link_write_ud_t {
    H5_daos_md_rw_cb_ud_t md_rw_cb_ud; /* Must be first */
    H5_daos_link_val_t link_val;
    unsigned rc;
    char *link_name_buf;
    size_t link_name_buf_size;
    uint8_t *link_val_buf;
    size_t link_val_buf_size;
    uint8_t prev_max_corder_buf[H5_DAOS_ENCODED_CRT_ORDER_SIZE];
    uint64_t max_corder;
    tse_task_t *link_write_task;
    tse_task_t *update_task;
} H5_daos_link_write_ud_t;

/* Task user data for writing link creation order info to a group */
typedef struct H5_daos_link_write_corder_ud_t {
    H5_daos_md_rw_cb_ud_t md_rw_cb_ud; /* Must be first */
    H5_daos_link_write_ud_t *link_write_ud;
    uint8_t nlinks_old_buf[H5_DAOS_ENCODED_NUM_LINKS_SIZE];
    uint8_t nlinks_new_buf[H5_DAOS_ENCODED_NUM_LINKS_SIZE];
    uint8_t max_corder_new_buf[H5_DAOS_ENCODED_CRT_ORDER_SIZE];
    uint8_t corder_target_buf[H5_DAOS_CRT_ORDER_TO_LINK_TRGT_BUF_SIZE];
    hsize_t nlinks;
    tse_task_t *write_corder_task;
} H5_daos_link_write_corder_ud_t;

/* Task user data for link copy */
typedef struct H5_daos_link_copy_ud_t {
    H5_daos_req_t *req;
    H5_daos_obj_t *target_obj;
    const char *new_link_name;
    size_t new_link_name_len;
    H5_daos_link_val_t link_val;
    tse_task_t *copy_task;
    char *src_path_buf;
    char *dst_path_buf;
} H5_daos_link_copy_ud_t;

/* Private data struct for H5_daos_link_follow */
typedef struct H5_daos_link_follow_ud_t {
    H5_daos_req_t *req;
    H5_daos_group_t *grp;
    tse_task_t *follow_task;
    const char *name;
    size_t name_len;
    hbool_t crt_missing_grp;
    H5_daos_link_val_t link_val;
    daos_obj_id_t *oid;
    hbool_t link_read;
    hbool_t *link_exists;
    char *path_buf;
} H5_daos_link_follow_ud_t;

/* User data struct for link get info */
typedef struct H5_daos_link_get_info_ud_t {
    H5_daos_req_t *req;
    H5_daos_obj_t *target_obj;
    const char *target_name;
    char *path_buf;
    size_t target_name_len;
    H5L_info2_t *link_info_out;
    H5_daos_link_val_t *link_val_out;
    H5_daos_link_val_t local_link_val;
} H5_daos_link_get_info_ud_t;

/* User data struct for link get val */
typedef struct H5_daos_link_get_val_ud_t {
    H5_daos_req_t *req;
    H5_daos_obj_t *target_obj;
    const char *target_name;
    char *path_buf;
    size_t target_name_len;
    void *out_buf;
    size_t out_buf_size;
    H5_daos_link_val_t local_link_val;
} H5_daos_link_get_val_ud_t;

/* A struct used to operate on a single link during link iteration */
typedef struct H5_daos_link_iter_op_ud_t {
    H5_daos_iter_ud_t *iter_ud;
    H5_daos_link_val_t link_val;
    H5L_info2_t linfo;
    char *link_path;
    size_t link_path_len;
    char *char_replace_loc;
    char char_replace_char;
    tse_task_t *op_task;
} H5_daos_link_iter_op_ud_t;

/* User data struct for iteration by creation order */
typedef struct H5_daos_link_ibco_ud_t {
    H5_daos_iter_data_t *iter_data;
    H5_daos_group_t     *target_grp;
    hsize_t             grp_nlinks;
    hsize_t             crt_idx;
    const char          *link_name;
    size_t              link_name_len;
    char                *name_buf;
    size_t              name_buf_size;
    H5_daos_link_val_t  link_val;
    H5L_info2_t         linfo;
    hbool_t             base_iter;
    char                *null_replace_loc;
    tse_task_t          *ibco_metatask;
} H5_daos_link_ibco_ud_t;

/*
 * A link iteration callback function data structure. It is
 * passed during link iteration when retrieving a link's name
 * by a given creation order index value.
 */
typedef struct H5_daos_link_find_name_by_idx_ud_t {
    char *link_name_out;
    size_t link_name_out_size;
    uint64_t target_link_idx;
    uint64_t cur_link_idx;
} H5_daos_link_find_name_by_idx_ud_t;

/* User data struct for link get name by index with automatic asynchronous
 * name buffer allocation */
typedef struct H5_daos_link_gnbi_alloc_ud_t {
    H5_daos_req_t *req;
    tse_task_t *gnbi_task;
    H5_daos_group_t *target_grp;
    H5_index_t index_type;
    H5_iter_order_t iter_order;
    uint64_t idx;
    const char **link_name;
    size_t *link_name_size;
    char **link_name_buf;
    size_t *link_name_buf_size;
    size_t cur_link_name_size;
} H5_daos_link_gnbi_alloc_ud_t;

/* User data struct for link get name by creation order */
typedef struct H5_daos_link_gnbc_ud_t {
    H5_daos_md_rw_cb_ud_t md_rw_cb_ud;
    uint8_t idx_buf[H5_DAOS_ENCODED_CRT_ORDER_SIZE];
    tse_task_t *gnbc_task;
    hsize_t grp_nlinks;
    H5_iter_order_t iter_order;
    uint64_t index;
    size_t *link_name_size;
    char *link_name_out;
    size_t link_name_out_size;
} H5_daos_link_gnbc_ud_t;

/* User data struct for link get name by name order */
typedef struct H5_daos_link_gnbn_ud_t {
    H5_daos_req_t *req;
    tse_task_t *gnbn_task;
    H5_daos_link_find_name_by_idx_ud_t iter_cb_ud;
    hsize_t grp_nlinks;
    hid_t target_grp_id;
    H5_daos_group_t *target_grp;
    H5_iter_order_t iter_order;
    uint64_t index;
    size_t *link_name_size;
    char *link_name_out;
    size_t link_name_out_size;
} H5_daos_link_gnbn_ud_t;

/* User data struct for link get creation index by name */
typedef struct H5_daos_link_gcbn_ud_t {
    H5_daos_md_rw_cb_ud_t md_rw_cb_ud;
    uint8_t crt_order_buf[H5_DAOS_ENCODED_CRT_ORDER_SIZE];
    uint64_t *crt_order;
} H5_daos_link_gcbn_ud_t;

/*
 * A link iteration callback function data structure. It is
 * passed during link iteration when retrieving a link's
 * creation order index value by the given link's name.
 */
typedef struct H5_daos_link_crt_idx_iter_ud_t {
    const char *target_link_name;
    uint64_t *link_idx_out;
} H5_daos_link_crt_idx_iter_ud_t;

/********************/
/* Local Prototypes */
/********************/

static int H5_daos_link_read_comp_cb(tse_task_t *task, void *args);
static herr_t H5_daos_link_read(H5_daos_group_t *grp, const char *name,
    size_t name_len, H5_daos_req_t *req, H5_daos_link_val_t *val,
    hbool_t *link_read, tse_task_t **first_task, tse_task_t **dep_task);
static int H5_daos_link_read_ln_prep_cb(tse_task_t *task, void *args);
static herr_t H5_daos_link_read_late_name(H5_daos_group_t *grp,
    const char **name, size_t *name_len, H5_daos_req_t *req,
    H5_daos_link_val_t *val, hbool_t *link_read,
    tse_task_t **first_task, tse_task_t **dep_task);

static int H5_daos_link_write_task(tse_task_t *task);
static int H5_daos_link_write_end_task(tse_task_t *task);
static int H5_daos_link_write_prep_cb(tse_task_t *task, void *args);
static int H5_daos_link_write_comp_cb(tse_task_t *task, void *args);

static int H5_daos_link_wr_corder_info_task(tse_task_t *task);
static herr_t H5_daos_link_write_corder_info(H5_daos_group_t *target_grp,
    uint64_t new_max_corder, H5_daos_link_write_ud_t *link_write_ud,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task);
static int H5_daos_link_write_corder_comp_cb(tse_task_t *task, void *args);

static int H5_daos_link_copy_task(tse_task_t *task);
static int H5_daos_link_copy_end_task(tse_task_t *task);

static int H5_daos_link_follow_end(tse_task_t *task);
static int H5_daos_link_follow_task(tse_task_t *task);

static int H5_daos_link_get_info_end_task(tse_task_t *task);
static int H5_daos_link_get_info(H5_daos_item_t *item, const H5VL_loc_params_t *loc_params,
    H5L_info2_t *link_info_out, H5_daos_link_val_t *link_val_out, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task);

static herr_t H5_daos_link_get_val(H5_daos_item_t *item, const H5VL_loc_params_t *loc_params,
    void *out_buf, size_t out_buf_size, H5_daos_req_t *req, tse_task_t **first_task,
    tse_task_t **dep_task);

static int H5_daos_link_iterate_list_comp_cb(tse_task_t *task, void *args);
static int H5_daos_link_iterate_op_task(tse_task_t *task);
static int H5_daos_link_iter_op_end(tse_task_t *task);
static herr_t H5_daos_link_iterate_by_name_order(H5_daos_group_t *target_grp, H5_daos_iter_data_t *iter_data,
    tse_task_t **first_task, tse_task_t **dep_task);
static int H5_daos_link_ibco_end_task(tse_task_t *task);
static int H5_daos_link_ibco_op_task(tse_task_t *task);
static int H5_daos_link_ibco_task2(tse_task_t *task);
static int H5_daos_link_ibco_task(tse_task_t *task);
static int H5_daos_link_ibco_helper(H5_daos_group_t *target_grp,
    H5_daos_iter_data_t *iter_data, hbool_t base_iter,
    tse_task_t **first_task, tse_task_t **dep_task);
static herr_t H5_daos_link_iterate_by_crt_order(H5_daos_group_t *target_grp,
    H5_daos_iter_data_t *iter_data, tse_task_t **first_task,
    tse_task_t **dep_task);

static herr_t H5_daos_link_delete(H5_daos_item_t *item, const H5VL_loc_params_t *loc_params,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task);

static int H5_daos_link_gnbi_alloc_task(tse_task_t *task);
static int H5_daos_link_gnbc_task(tse_task_t *task);
static int H5_daos_link_gnbc_comp_cb(tse_task_t *task, void *args);
static herr_t H5_daos_link_get_name_by_crt_order(H5_daos_group_t *target_grp,
    H5_iter_order_t iter_order, uint64_t index, size_t *link_name_size,
    char *link_name_out, size_t link_name_out_size, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task);
static int H5_daos_link_gnbn_task(tse_task_t *task);
static int H5_daos_link_gnbn_end_task(tse_task_t *task);
static herr_t H5_daos_link_get_name_by_name_order(H5_daos_group_t *target_grp,
    H5_iter_order_t iter_order, uint64_t index, size_t *link_name_size,
    char *link_name_out, size_t link_name_out_size, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task);
static herr_t H5_daos_link_get_name_by_name_order_cb(hid_t group, const char *name, const H5L_info2_t *info, void *op_data);

static herr_t H5_daos_link_remove_from_crt_idx(H5_daos_group_t *target_grp,
    const H5VL_loc_params_t *loc_params, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task);
static herr_t H5_daos_link_remove_from_crt_idx_name_cb(hid_t group, const char *name, const H5L_info2_t *info, void *op_data);

static herr_t H5_daos_link_shift_crt_idx_keys_down(H5_daos_group_t *target_grp, uint64_t idx_begin, uint64_t idx_end);

static int H5_daos_link_gcbn_comp_cb(tse_task_t *task, void *args);

static uint64_t H5_daos_hash_obj_id(dv_hash_table_key_t obj_id_lo);
static int H5_daos_cmp_obj_id(dv_hash_table_key_t obj_id_lo1, dv_hash_table_key_t obj_id_lo2);
static void H5_daos_free_visited_link_hash_table_key(dv_hash_table_key_t value);



/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_read_comp_cb
 *
 * Purpose:     Complete callback for asynchronous metadata fetch for link
 *              reads.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              April, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_read_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_link_read_ud_t *udata;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link read task");

    assert(udata->md_rw_cb_ud.req);
    assert(udata->md_rw_cb_ud.req->file);
    assert(udata->read_metatask);
    assert(!udata->md_rw_cb_ud.req->file->closed);
    assert(udata->md_rw_cb_ud.obj->item.type == H5I_GROUP);

    /* Check for buffer not large enough */
    if(task->dt_result == -DER_REC2BIG) {
        tse_task_t *read_task;

        /* Verify iod size makes sense */
        if(udata->md_rw_cb_ud.sg_iov[0].iov_buf_len != H5_DAOS_LINK_VAL_BUF_SIZE)
            D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "buffer length does not match expected value");
        if(udata->md_rw_cb_ud.iod[0].iod_size <= H5_DAOS_LINK_VAL_BUF_SIZE)
            D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "invalid iod_size returned from DAOS (buffer should have been large enough)");

        /* Reallocate link value buffer */
        udata->md_rw_cb_ud.sg_iov[0].iov_buf = DV_free(udata->md_rw_cb_ud.sg_iov[0].iov_buf);
        if(NULL == (udata->md_rw_cb_ud.sg_iov[0].iov_buf = DV_malloc(udata->md_rw_cb_ud.iod[0].iod_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate buffer for link value");

        /* Set up sgl */
        udata->md_rw_cb_ud.sg_iov[0].iov_buf_len = udata->md_rw_cb_ud.iod[0].iod_size;
        udata->md_rw_cb_ud.sg_iov[0].iov_len = udata->md_rw_cb_ud.iod[0].iod_size;
        udata->md_rw_cb_ud.sgl[0].sg_nr_out = 0;

        /* Create task for reissued link read */
        if(0 != (ret = daos_task_create(DAOS_OPC_OBJ_FETCH, &udata->md_rw_cb_ud.obj->item.file->sched, 0, NULL, &read_task)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task to read link: %s", H5_daos_err_to_string(ret));

        /* Set callback functions for link read */
        if(0 != (ret = tse_task_register_cbs(read_task, H5_daos_md_rw_prep_cb, NULL, 0, H5_daos_link_read_comp_cb, NULL, 0)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't register callbacks for task to read link: %s", H5_daos_err_to_string(ret));

        /* Set private data for link read */
        (void)tse_task_set_priv(read_task, udata);

        /* Schedule link read task and transfer ownership of udata */
        if(0 != (ret = tse_task_schedule(read_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task to read link: %s", H5_daos_err_to_string(ret));
        udata = NULL;
    } /* end if */
    else {
        /* Handle errors in read task.  Only record error in udata->req_status
         * if it does not already contain an error (it could contain an error if
         * another task this task is not dependent on also failed). */
        if(task->dt_result < -H5_DAOS_PRE_ERROR
                && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = task->dt_result;
            udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
            if(udata->link_read)
                *udata->link_read = FALSE;

        } /* end if */
        else if(task->dt_result == 0) {
            if(udata->md_rw_cb_ud.iod[0].iod_size == (uint64_t)0) {
                /* No link found */
                if(udata->link_read)
                    *udata->link_read = FALSE;
                else
                    D_GOTO_ERROR(H5E_LINK, H5E_TRAVERSE, -H5_DAOS_NONEXIST_LINK, "link not found");
            } /* end if */
            else {
                uint8_t *p = (uint8_t *)udata->md_rw_cb_ud.sg_iov[0].iov_buf;

                /* Decode link type */
                udata->link_val->type = (H5L_type_t)*p++;

                /* Decode remainder of link value */
                switch(udata->link_val->type) {
                    case H5L_TYPE_HARD:
                        /* Decode oid */
                        UINT64DECODE(p, udata->link_val->target.hard.lo)
                        UINT64DECODE(p, udata->link_val->target.hard.hi)

                        break;

                    case H5L_TYPE_SOFT:
                        /* The buffer allocated is guaranteed to be big enough to
                         * hold the soft link path, since the path needs an extra
                         * byte for the null terminator but loses the byte
                         * specifying the type.  Take ownership of the buffer and
                         * shift the value down one byte. */
                        udata->link_val->target.soft = (char *)udata->md_rw_cb_ud.sg_iov[0].iov_buf;
                        udata->md_rw_cb_ud.sg_iov[0].iov_buf = NULL;
                        memmove(udata->link_val->target.soft, udata->link_val->target.soft + 1, udata->md_rw_cb_ud.iod[0].iod_size - 1);

                        /* Add null terminator */
                        udata->link_val->target.soft[udata->md_rw_cb_ud.iod[0].iod_size - 1] = '\0';

                        break;

                    case H5L_TYPE_ERROR:
                    case H5L_TYPE_EXTERNAL:
                    case H5L_TYPE_MAX:
                    default:
                        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "invalid or unsupported link type");
                } /* end switch */

                if(udata->link_read)
                    *udata->link_read = TRUE;
            } /* end else */
        } /* end if */
    } /* end else */

done:
    /* Clean up if this is the last fetch task */
    if(udata) {
        /* Close group */
        if(H5_daos_group_close((H5_daos_group_t *)udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");

        /* Free buffer if we still own it */
        DV_free(udata->md_rw_cb_ud.sg_iov[0].iov_buf);

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except
         * for H5_daos_req_free_int, which updates req->status if it sees an
         * error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = ret_value;
            udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Complete fetch metatask */
        tse_task_complete(udata->read_metatask, ret_value);

        assert(!udata->md_rw_cb_ud.free_dkey);
        assert(!udata->md_rw_cb_ud.free_akeys);

        /* Free udata */
        DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_read_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_read
 *
 * Purpose:     Reads the specified link from the given group.  Note that
 *              if the returned link is a soft link, val->target.soft must
 *              eventually be freed.  name and name_len can be filled in
 *              after this function is called, before dep_task executes.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_read(H5_daos_group_t *grp, const char *name, size_t name_len,
    H5_daos_req_t *req, H5_daos_link_val_t *val, hbool_t *link_read,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_read_ud_t *read_udata = NULL;
    uint8_t *val_buf = NULL;
    tse_task_t *read_task;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(grp);
    assert(name);
    assert(req);
    assert(val);
    assert(first_task);
    assert(dep_task);

    /* Allocate argument struct for read task */
    if(NULL == (read_udata = (H5_daos_link_read_ud_t *)DV_calloc(sizeof(H5_daos_link_read_ud_t))))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't allocate buffer for read callback arguments");

    /* Set up operation to read link value from group */
    /* Set up ud struct */
    read_udata->md_rw_cb_ud.req = req;
    read_udata->md_rw_cb_ud.obj = &grp->obj;
    read_udata->link_val = val;
    read_udata->link_read = link_read;

    /* Set up dkey */
    daos_iov_set(&read_udata->md_rw_cb_ud.dkey, (void *)name, (daos_size_t)name_len);

    /* Single iod and sgl */
    read_udata->md_rw_cb_ud.nr = 1u;

    /* Set up iod */
    daos_iov_set(&read_udata->md_rw_cb_ud.iod[0].iod_name, (void *)H5_daos_link_key_g, H5_daos_link_key_size_g);
    read_udata->md_rw_cb_ud.iod[0].iod_nr = 1u;
    read_udata->md_rw_cb_ud.iod[0].iod_size = DAOS_REC_ANY;
    read_udata->md_rw_cb_ud.iod[0].iod_type = DAOS_IOD_SINGLE;

    /* Allocate initial link vallue buffer */
    if(NULL == (val_buf = DV_malloc(H5_DAOS_LINK_VAL_BUF_SIZE)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't allocate buffer for link value");

    /* Set up sgl */
    daos_iov_set(&read_udata->md_rw_cb_ud.sg_iov[0], val_buf, (daos_size_t)H5_DAOS_LINK_VAL_BUF_SIZE);
    read_udata->md_rw_cb_ud.sgl[0].sg_nr = 1;
    read_udata->md_rw_cb_ud.sgl[0].sg_nr_out = 0;
    read_udata->md_rw_cb_ud.sgl[0].sg_iovs = &read_udata->md_rw_cb_ud.sg_iov[0];

    /* Set task name */
    read_udata->md_rw_cb_ud.task_name = "link read";

    /* Create meta task for link read.  This empty task will be completed when
     * the read is finished by H5_daos_link_read_comp_cb. We can't use
     * read_task since it may not be completed by the first fetch. */
    if(0 != (ret = tse_task_create(NULL, &grp->obj.item.file->sched, NULL, &read_udata->read_metatask)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create meta task for link read: %s", H5_daos_err_to_string(ret));

    /* Create task for link read */
    if(0 != (ret = daos_task_create(DAOS_OPC_OBJ_FETCH, &grp->obj.item.file->sched, *dep_task ? 1 : 0, *dep_task ? dep_task : NULL, &read_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to read link: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for link read */
    if(0 != (ret = tse_task_register_cbs(read_task, H5_daos_md_rw_prep_cb, NULL, 0, H5_daos_link_read_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't register callbacks for task to read link: %s", H5_daos_err_to_string(ret));

    /* Set private data for link read */
    (void)tse_task_set_priv(read_task, read_udata);

    /* Schedule meta task */
    if(0 != (ret = tse_task_schedule(read_udata->read_metatask, false)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule meta task for link read: %s", H5_daos_err_to_string(ret));

    /* Schedule link read task (or save it to be scheduled later) and give it a
     * reference to req and the group */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(read_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task to read link: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = read_task;
    *dep_task = read_udata->read_metatask;
    req->rc++;
    grp->obj.item.rc++;
    read_udata = NULL;
    val_buf = NULL;

done:
    /* Cleanup on failure */
    if(ret_value < 0) {
        read_udata = DV_free(read_udata);
        val_buf = DV_free(val_buf);
    } /* end if */

    /* Make sure we cleaned up */
    assert(!read_udata);
    assert(!val_buf);

    D_FUNC_LEAVE;
} /* end H5_daos_link_read() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_read_ln_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous metadata fetch for
 *              H5_daos_link_read_late_name().
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              June, 2019
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_read_ln_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_link_read_ud_t *udata;
    daos_obj_rw_t *fetch_args;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link read task");

    assert(udata->md_rw_cb_ud.obj);
    assert(udata->md_rw_cb_ud.req);
    assert(udata->md_rw_cb_ud.req->file);
    assert(!udata->md_rw_cb_ud.req->file->closed);

    /* Handle errors in previous tasks */
    if(udata->md_rw_cb_ud.req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->md_rw_cb_ud.req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Check type of target_obj */
    if(udata->md_rw_cb_ud.obj->item.type != H5I_GROUP)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, -H5_DAOS_BAD_VALUE, "target object is not a group");

    /* Set up dkey */
    daos_iov_set(&udata->md_rw_cb_ud.dkey, (void *)*udata->name, (daos_size_t)*udata->name_len);

    /* Set update task arguments */
    if(NULL == (fetch_args = daos_task_get_args(task))) {
        tse_task_complete(task, -H5_DAOS_DAOS_GET_ERROR);
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for metadata I/O task");
    } /* end if */
    fetch_args->oh = udata->md_rw_cb_ud.obj->obj_oh;
    fetch_args->th = udata->md_rw_cb_ud.req->th;
    fetch_args->flags = 0;
    fetch_args->dkey = &udata->md_rw_cb_ud.dkey;
    fetch_args->nr = udata->md_rw_cb_ud.nr;
    fetch_args->iods = udata->md_rw_cb_ud.iod;
    fetch_args->sgls = udata->md_rw_cb_ud.sgl;

done:
    D_FUNC_LEAVE;
} /* end H5_daos_link_read_ln_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_read_late_name
 *
 * Purpose:     Reads the specified link from the given group.  Note that
 *              if the returned link is a soft link, val->target.soft must
 *              eventually be freed.  name and name_len can be filled in
 *              after this function is called, before dep_task executes.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_read_late_name(H5_daos_group_t *grp, const char **name, size_t *name_len,
    H5_daos_req_t *req, H5_daos_link_val_t *val, hbool_t *link_read,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_read_ud_t *read_udata = NULL;
    uint8_t *val_buf = NULL;
    tse_task_t *read_task;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(grp);
    assert(name);
    assert(req);
    assert(val);
    assert(first_task);
    assert(dep_task);

    /* Allocate argument struct for read task */
    if(NULL == (read_udata = (H5_daos_link_read_ud_t *)DV_calloc(sizeof(H5_daos_link_read_ud_t))))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't allocate buffer for read callback arguments");

    /* Set up operation to read link value from group */
    /* Set up ud struct */
    read_udata->md_rw_cb_ud.req = req;
    read_udata->md_rw_cb_ud.obj = &grp->obj;
    read_udata->name = name;
    read_udata->name_len = name_len;
    read_udata->link_val = val;
    read_udata->link_read = link_read;

    /* Single iod and sgl */
    read_udata->md_rw_cb_ud.nr = 1u;

    /* Set up iod */
    daos_iov_set(&read_udata->md_rw_cb_ud.iod[0].iod_name, (void *)H5_daos_link_key_g, H5_daos_link_key_size_g);
    read_udata->md_rw_cb_ud.iod[0].iod_nr = 1u;
    read_udata->md_rw_cb_ud.iod[0].iod_size = DAOS_REC_ANY;
    read_udata->md_rw_cb_ud.iod[0].iod_type = DAOS_IOD_SINGLE;

    /* Allocate initial link vallue buffer */
    if(NULL == (val_buf = DV_malloc(H5_DAOS_LINK_VAL_BUF_SIZE)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't allocate buffer for link value");

    /* Set up sgl */
    daos_iov_set(&read_udata->md_rw_cb_ud.sg_iov[0], val_buf, (daos_size_t)H5_DAOS_LINK_VAL_BUF_SIZE);
    read_udata->md_rw_cb_ud.sgl[0].sg_nr = 1;
    read_udata->md_rw_cb_ud.sgl[0].sg_nr_out = 0;
    read_udata->md_rw_cb_ud.sgl[0].sg_iovs = &read_udata->md_rw_cb_ud.sg_iov[0];

    /* Set task name */
    read_udata->md_rw_cb_ud.task_name = "link read (late name)";

    /* Create meta task for link read.  This empty task will be completed when
     * the read is finished by H5_daos_link_read_comp_cb. We can't use
     * read_task since it may not be completed by the first fetch. */
    if(0 != (ret = tse_task_create(NULL, &grp->obj.item.file->sched, NULL, &read_udata->read_metatask)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create meta task for link read: %s", H5_daos_err_to_string(ret));

    /* Create task for link read */
    if(0 != (ret = daos_task_create(DAOS_OPC_OBJ_FETCH, &grp->obj.item.file->sched, *dep_task ? 1 : 0, *dep_task ? dep_task : NULL, &read_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to read link: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for link read */
    if(0 != (ret = tse_task_register_cbs(read_task, H5_daos_link_read_ln_prep_cb, NULL, 0, H5_daos_link_read_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't register callbacks for task to read link: %s", H5_daos_err_to_string(ret));

    /* Set private data for link read */
    (void)tse_task_set_priv(read_task, read_udata);

    /* Schedule meta task */
    if(0 != (ret = tse_task_schedule(read_udata->read_metatask, false)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule meta task for link read: %s", H5_daos_err_to_string(ret));

    /* Schedule link read task (or save it to be scheduled later) and give it a
     * reference to req and the group */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(read_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task to read link: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = read_task;
    *dep_task = read_udata->read_metatask;
    req->rc++;
    grp->obj.item.rc++;
    read_udata = NULL;
    val_buf = NULL;

done:
    /* Cleanup on failure */
    if(ret_value < 0) {
        read_udata = DV_free(read_udata);
        val_buf = DV_free(val_buf);
    } /* end if */

    /* Make sure we cleaned up */
    assert(!read_udata);
    assert(!val_buf);

    D_FUNC_LEAVE;
} /* end H5_daos_link_read_late_name() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_write_task
 *
 * Purpose:     Asynchronous task for H5_daos_link_write().  Determines if
 *              creation order info needs to be written, if so schedules
 *              more tasks to handle this, otherwise schedules the update
 *              (actual link write) task.
 *
 * Return:      Success:        0 
 *              Failure:        Negative
 *
 * Programmer:  Neil Fortner
 *              June, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_write_task(tse_task_t *task)
{
    H5_daos_link_write_ud_t *udata = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link write task");

    assert(udata->md_rw_cb_ud.obj->item.type == H5I_GROUP);

    /* Handle errors in previous tasks */
    if(udata->md_rw_cb_ud.req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->md_rw_cb_ud.req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Check for creation order tracking/indexing */
    if(((H5_daos_group_t *)udata->md_rw_cb_ud.obj)->gcpl_cache.track_corder) {
        tse_task_t *end_task = NULL;

        /* Read group's current maximum creation order value */
        if(H5_daos_group_get_max_crt_order((H5_daos_group_t *)udata->md_rw_cb_ud.obj,
                &udata->max_corder, udata->md_rw_cb_ud.req, &first_task, &dep_task) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CANTGET, -H5_DAOS_H5_GET_ERROR, "can't get group's maximum creation order value");

        /* Create task to finish this operation */
        if(0 !=  (ret = tse_task_create(H5_daos_link_write_end_task, &udata->md_rw_cb_ud.obj->item.file->sched, udata, &end_task)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create end task for link write: %s", H5_daos_err_to_string(ret));

        /* Register task dependency */
        if(dep_task && 0 != (ret = tse_task_register_deps(end_task, 1, &dep_task)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for link write end task: %s", H5_daos_err_to_string(ret));

        /* Schedule link write end task (or save it to be scheduled later) and
         * transfer ownership of udata */
        if(first_task) {
            if(0 != (ret = tse_task_schedule(end_task, false)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule end task for link write: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else
            first_task = end_task;
        udata = NULL;
    } /* end if */
    else {
        /* Save link write update task to be scheduled later and transfer
         * ownership of udata */
        assert(first_task);
        first_task = udata->update_task;
        dep_task = udata->update_task;
        udata = NULL;
    } /* end else */

done:
    /* Schedule first task */
    if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
        D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link write end task: %s", H5_daos_err_to_string(ret));

    /* Clean up */
    if(udata) {
        assert(ret_value < 0);

        /* Decrememnt ref count on udata and close it if it drops to 0 */
        if(--udata->rc == 0) {
            /* Close target_grp */
            if(H5_daos_group_close((H5_daos_group_t *)udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
            udata->md_rw_cb_ud.obj = NULL;
        } /* end if */

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value != -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = ret_value;
            udata->md_rw_cb_ud.req->failed_task = "link write task";
        } /* end if */

        assert(!udata->md_rw_cb_ud.free_dkey);
        assert(!udata->md_rw_cb_ud.free_akeys);

        /* Continue closing udata */
        if(udata->rc == 0) {
            /* Release our reference to req */
            if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

            /* Complete link write task */
            assert(task == udata->link_write_task);
            tse_task_complete(udata->link_write_task, ret_value);

            /* Free memory */
            DV_free(udata->link_name_buf);
            DV_free(udata->link_val_buf);
            DV_free(udata);
        } /* end if */
    } /* end if */

    D_FUNC_LEAVE;
} /* end  H5_daos_link_write_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_write_end_task
 *
 * Purpose:     Finishes the link write task.  Only called if writing
 *              creation order info.  Adds ths info to the update task,
 *              schedules it, and cleans up.
 *
 * Return:      Success:        0 
 *              Failure:        Negative
 *
 * Programmer:  Neil Fortner
 *              June, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_write_end_task(tse_task_t *task)
{
    H5_daos_link_write_ud_t *udata = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    uint8_t *p;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link write task");

    assert(udata->md_rw_cb_ud.obj->item.type == H5I_GROUP);

    /* Handle errors in previous tasks */
    if(udata->md_rw_cb_ud.req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->md_rw_cb_ud.req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Encode group's current max creation order value */
    p = udata->prev_max_corder_buf;
    UINT64ENCODE(p, udata->max_corder);

    /* Add new link to max. creation order value */
    udata->max_corder++;

    /* Add link name -> creation order mapping key-value pair
     * to main link write operation
     */
    udata->md_rw_cb_ud.nr++;

    /* Adjust IOD for creation order info */
    daos_iov_set(&udata->md_rw_cb_ud.iod[1].iod_name, (void *)H5_daos_link_corder_key_g, H5_daos_link_corder_key_size_g);
    udata->md_rw_cb_ud.iod[1].iod_nr = 1u;
    udata->md_rw_cb_ud.iod[1].iod_size = (uint64_t)8;
    udata->md_rw_cb_ud.iod[1].iod_type = DAOS_IOD_SINGLE;

    /* Adjust SGL for creation order info */
    daos_iov_set(&udata->md_rw_cb_ud.sg_iov[1], (void *)udata->prev_max_corder_buf, sizeof(udata->prev_max_corder_buf));
    udata->md_rw_cb_ud.sgl[1].sg_nr = 1;
    udata->md_rw_cb_ud.sgl[1].sg_nr_out = 0;
    udata->md_rw_cb_ud.sgl[1].sg_iovs = &udata->md_rw_cb_ud.sg_iov[1];

    /* Create a task for writing the link creation order info to the
     * target group */
    if(H5_daos_link_write_corder_info((H5_daos_group_t *)udata->md_rw_cb_ud.obj, udata->max_corder, udata,
            udata->md_rw_cb_ud.req, &first_task, &dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task for writing link creation order information to group");

    /* Schedule link write update task, and transfer ownership of udata.  This
     * can run in parallel with tasks scheduled by
     * H5_daos_link_write_corder_info(). */
    if(0 != (ret = tse_task_schedule(udata->update_task, false)))
        D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule link write task: %s", H5_daos_err_to_string(ret));
    udata = NULL;

done:
    /* Schedule first task */
    if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
        D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link write end task: %s", H5_daos_err_to_string(ret));

    /* Clean up */
    if(udata) {
        assert(ret_value < 0);

        /* Decrememnt ref count on udata and close it if it drops to 0 */
        if(--udata->rc == 0) {
            /* Close target_grp */
            if(H5_daos_group_close((H5_daos_group_t *)udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
            udata->md_rw_cb_ud.obj = NULL;
        } /* end if */

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value != -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = ret_value;
            udata->md_rw_cb_ud.req->failed_task = "link write end task";
        } /* end if */

        /* Continue closing udata */
        if(udata->rc == 0) {
            /* Release our reference to req */
            if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

            /* Complete link write task */
            tse_task_complete(udata->link_write_task, ret_value);

            /* Free memory */
            DV_free(udata->link_name_buf);
            DV_free(udata->link_val_buf);
            DV_free(udata);
        } /* end if */
    } /* end if */

    /* Complete this task */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end  H5_daos_link_write_end_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_write
 *
 * Purpose:     Creates an asynchronous task for writing the specified link
 *              into the given group `target_grp`. `link_val` specifies the
 *              type of link to create and the value to write for the link.
 *
 * Return:      Success:        0
 *              Failure:        Error code (negative)
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_link_write(H5_daos_group_t *target_grp, const char *name,
    size_t name_len, H5_daos_link_val_t *link_val, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_write_ud_t *link_write_ud = NULL;
    int ret;
    int ret_value = 0;

    assert(target_grp);
    assert(name);
    assert(link_val);

    /* Check for write access */
    if(!(target_grp->obj.item.file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "no write intent on file");

    /* Allocate argument struct */
    if(NULL == (link_write_ud = (H5_daos_link_write_ud_t *)DV_calloc(sizeof(H5_daos_link_write_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate buffer for link write callback arguments");
    link_write_ud->link_val = *link_val;

    /* Allocate buffer for link value and encode type-specific value information */
    switch(link_val->type) {
        case H5L_TYPE_HARD:
            assert(H5_DAOS_HARD_LINK_VAL_SIZE == sizeof(link_val->target.hard) + 1);

            link_write_ud->link_val_buf_size = H5_DAOS_HARD_LINK_VAL_SIZE;
            if(NULL == (link_write_ud->link_val_buf = (uint8_t *)DV_malloc(link_write_ud->link_val_buf_size)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate space for link target");

            /* For hard links, encoding of the OID into the link's value buffer
             * is delayed until the link write task's preparation callback. This
             * is to allow for the case where the link write might depend on an
             * OID that gets generated asynchronously.
             */

            break;

        case H5L_TYPE_SOFT:
            link_write_ud->link_val_buf_size = strlen(link_val->target.soft) + 1;
            if(NULL == (link_write_ud->link_val_buf = (uint8_t *)DV_malloc(link_write_ud->link_val_buf_size)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate space for link target");

            H5_DAOS_ENCODE_LINK_VALUE(link_write_ud->link_val_buf, link_write_ud->link_val_buf_size,
                    link_write_ud->link_val, H5L_TYPE_SOFT);

            break;

        case H5L_TYPE_ERROR:
        case H5L_TYPE_EXTERNAL:
        case H5L_TYPE_MAX:
        default:
            D_GOTO_ERROR(H5E_SYM, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "invalid or unsupported link type");
    } /* end switch */

    /* Copy name */
    if(NULL == (link_write_ud->link_name_buf = (char *)DV_malloc(name_len)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate space for name buffer");
    (void)memcpy(link_write_ud->link_name_buf, name, name_len);
    link_write_ud->link_name_buf_size = name_len;

    /* Set up known fields of link_write_ud */
    link_write_ud->md_rw_cb_ud.req = req;
    link_write_ud->md_rw_cb_ud.obj = &target_grp->obj;
    link_write_ud->md_rw_cb_ud.nr = 1;
    link_write_ud->rc = 1;

    /* Set task name */
    link_write_ud->md_rw_cb_ud.task_name = "link write";

    /* Set up dkey */
    daos_iov_set(&link_write_ud->md_rw_cb_ud.dkey, (void *)link_write_ud->link_name_buf, (daos_size_t)name_len);
    link_write_ud->md_rw_cb_ud.free_dkey = FALSE;

    /* Set up iod */
    daos_iov_set(&link_write_ud->md_rw_cb_ud.iod[0].iod_name, (void *)H5_daos_link_key_g, H5_daos_link_key_size_g);
    link_write_ud->md_rw_cb_ud.iod[0].iod_nr = 1u;
    link_write_ud->md_rw_cb_ud.iod[0].iod_size = link_write_ud->link_val_buf_size;
    link_write_ud->md_rw_cb_ud.iod[0].iod_type = DAOS_IOD_SINGLE;
    link_write_ud->md_rw_cb_ud.free_akeys = FALSE;

    /* SGL is setup by link write task prep callback */

    /* Create task for link write update */
    /* No dependencies - it will be scheduled by an async callback later once it
     * is ready to run */
    if(0 != (ret = daos_task_create(DAOS_OPC_OBJ_UPDATE, &target_grp->obj.item.file->sched, 0, NULL, &link_write_ud->update_task)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, ret, "can't create task to write link: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for link write */
    if(0 != (ret = tse_task_register_cbs(link_write_ud->update_task, H5_daos_link_write_prep_cb, NULL, 0, H5_daos_link_write_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, ret, "can't register callbacks for task to write link: %s", H5_daos_err_to_string(ret));

    /* Set private data for link write */
    (void)tse_task_set_priv(link_write_ud->update_task, link_write_ud);

    /* Create task to finish this operation */
    if(0 !=  (ret = tse_task_create(H5_daos_link_write_task, &target_grp->obj.item.file->sched, link_write_ud, &link_write_ud->link_write_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for link write: %s", H5_daos_err_to_string(ret));

    /* Register task dependency */
    if(*dep_task && 0 != (ret = tse_task_register_deps(link_write_ud->link_write_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link write task: %s", H5_daos_err_to_string(ret));

    /* Schedule task (or save it to be scheduled later) and give it a
     * reference to the group, req and udata */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(link_write_ud->link_write_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for link write: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = link_write_ud->link_write_task;
    *dep_task = link_write_ud->link_write_task;
    target_grp->obj.item.rc++;
    req->rc++;
    link_write_ud = NULL;

done:
    /* Cleanup on failure */
    if(link_write_ud) {
        assert(ret_value < 0);
        link_write_ud->link_name_buf = DV_free(link_write_ud->link_name_buf);
        link_write_ud->link_val_buf = DV_free(link_write_ud->link_val_buf);
        link_write_ud = DV_free(link_write_ud);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_write() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_write_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous link write. Currently
 *              checks for errors from previous tasks then sets arguments
 *              for the DAOS operation. If creation order is tracked for
 *              the target group, also creates a task to write that info.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_write_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_link_write_ud_t *udata;
    daos_obj_rw_t *update_args;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link write task");

    assert(udata->md_rw_cb_ud.obj);
    assert(udata->md_rw_cb_ud.req);
    assert(udata->md_rw_cb_ud.req->file);
    assert(!udata->md_rw_cb_ud.req->file->closed);
    assert(udata->link_val_buf);

    /* Handle errors */
    if(udata->md_rw_cb_ud.req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->md_rw_cb_ud.req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* If this is a hard link, encoding of the OID into
     * the link's value buffer was delayed until this point.
     * Go ahead and do the encoding now.
     */
    if(udata->link_val.type == H5L_TYPE_HARD) {
        if(udata->link_val.target_oid_async) {
            /* If the OID for this link was generated asynchronously,
             * go ahead and set up the target OID field in the
             * H5_daos_link_val_t to be used for the encoding process.
             */
            udata->link_val.target.hard.lo = udata->link_val.target_oid_async->lo;
            udata->link_val.target.hard.hi = udata->link_val.target_oid_async->hi;
        } /* end if */

        H5_DAOS_ENCODE_LINK_VALUE(udata->link_val_buf, udata->link_val_buf_size,
                udata->link_val, H5L_TYPE_HARD);
    } /* end if */

    /* Set up SGL */
    daos_iov_set(&udata->md_rw_cb_ud.sg_iov[0], udata->link_val_buf, udata->md_rw_cb_ud.iod[0].iod_size);
    udata->md_rw_cb_ud.sgl[0].sg_nr = 1;
    udata->md_rw_cb_ud.sgl[0].sg_nr_out = 0;
    udata->md_rw_cb_ud.sgl[0].sg_iovs = &udata->md_rw_cb_ud.sg_iov[0];

    /* Set update task arguments */
    if(NULL == (update_args = daos_task_get_args(task))) {
        tse_task_complete(task, -H5_DAOS_DAOS_GET_ERROR);
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for link write task");
    } /* end if */
    update_args->oh = udata->md_rw_cb_ud.obj->obj_oh;
    update_args->th = udata->md_rw_cb_ud.req->th;
    update_args->flags = 0;
    update_args->dkey = &udata->md_rw_cb_ud.dkey;
    update_args->nr = udata->md_rw_cb_ud.nr;
    update_args->iods = udata->md_rw_cb_ud.iod;
    update_args->sgls = udata->md_rw_cb_ud.sgl;

done:
    D_FUNC_LEAVE;
} /* end H5_daos_link_write_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_write_comp_cb
 *
 * Purpose:     Completion callback for asynchronous link write. Currently
 *              checks for a failed task then frees private data.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_write_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_link_write_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link write task");

    assert(udata->md_rw_cb_ud.req);
    assert(udata->md_rw_cb_ud.req->file);
    assert(!udata->md_rw_cb_ud.req->file->closed);
    assert(udata->md_rw_cb_ud.obj->item.type == H5I_GROUP);

    /* Handle errors in update task.  Only record error in udata->req_status if
     * it does not already contain an error (it could contain an error if
     * another task this task is not dependent on also failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->md_rw_cb_ud.req->status = task->dt_result;
        udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
    } /* end if */

done:
    if(udata) {
        /* Decrememnt ref count on udata and close it if it drops to 0 */
        if(--udata->rc == 0) {
            /* Close group */
            if(H5_daos_group_close((H5_daos_group_t *)udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
                D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
        } /* end if */

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = ret_value;
            udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
        } /* end if */

        assert(!udata->md_rw_cb_ud.free_dkey);
        assert(!udata->md_rw_cb_ud.free_akeys);

        /* Continue closing udata */
        if(udata->rc == 0) {
            /* Release our reference to req */
            if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
                D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

            /* Complete link write task */
            tse_task_complete(udata->link_write_task, ret_value);

            /* Free memory */
            DV_free(udata->link_name_buf);
            DV_free(udata->link_val_buf);
            DV_free(udata);
        } /* end if */
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_write_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_write_corder_info
 *
 * Purpose:     Creates an asynchronous task for writing link creation
 *              order information to the given target group. This task
 *              doesn't necessarily depend on the main link write task
 *              having completed, but it does share some common buffers
 *              with the main link write task. Therefore, this task cannot
 *              be scheduled until the main link write task has been
 *              prepped.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_wr_corder_info_task(tse_task_t *task)
{
    H5_daos_link_write_corder_ud_t *udata = NULL;
    uint64_t uint_nlinks;
    tse_task_t *update_task = NULL;
    uint8_t *p;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link write creation order info task");

    assert(udata->md_rw_cb_ud.obj->item.type == H5I_GROUP);

    /* Handle errors in previous tasks */
    if(udata->md_rw_cb_ud.req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->md_rw_cb_ud.req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Encode old nlinks */
    uint_nlinks = (uint64_t)udata->nlinks;
    p = udata->nlinks_old_buf;
    UINT64ENCODE(p, uint_nlinks);

    /* Add new link to count */
    uint_nlinks++;

    /* Encode remaining buffers */
    p = udata->nlinks_new_buf;
    UINT64ENCODE(p, uint_nlinks);
    memcpy(udata->corder_target_buf, udata->nlinks_old_buf, 8);
    udata->corder_target_buf[8] = 0;

    /* Set up IOD */

    /* Max Link Creation Order Key */
    daos_iov_set(&udata->md_rw_cb_ud.iod[0].iod_name, (void *)H5_daos_max_link_corder_key_g, H5_daos_max_link_corder_key_size_g);
    udata->md_rw_cb_ud.iod[0].iod_nr = 1u;
    udata->md_rw_cb_ud.iod[0].iod_size = (daos_size_t)H5_DAOS_ENCODED_CRT_ORDER_SIZE;
    udata->md_rw_cb_ud.iod[0].iod_type = DAOS_IOD_SINGLE;

    /* Key for new number of links in group */
    daos_iov_set(&udata->md_rw_cb_ud.iod[1].iod_name, (void *)H5_daos_nlinks_key_g, H5_daos_nlinks_key_size_g);
    udata->md_rw_cb_ud.iod[1].iod_nr = 1u;
    udata->md_rw_cb_ud.iod[1].iod_size = (daos_size_t)H5_DAOS_ENCODED_NUM_LINKS_SIZE;
    udata->md_rw_cb_ud.iod[1].iod_type = DAOS_IOD_SINGLE;

    /* Key for mapping from link creation order value -> link name */
    daos_iov_set(&udata->md_rw_cb_ud.iod[2].iod_name, (void *)udata->nlinks_old_buf, H5_DAOS_ENCODED_NUM_LINKS_SIZE);
    udata->md_rw_cb_ud.iod[2].iod_nr = 1u;
    udata->md_rw_cb_ud.iod[2].iod_size = (uint64_t)udata->link_write_ud->link_name_buf_size;
    udata->md_rw_cb_ud.iod[2].iod_type = DAOS_IOD_SINGLE;

    /* Key for mapping from link creation order value -> link target */
    daos_iov_set(&udata->md_rw_cb_ud.iod[3].iod_name, (void *)udata->corder_target_buf, H5_DAOS_CRT_ORDER_TO_LINK_TRGT_BUF_SIZE);
    udata->md_rw_cb_ud.iod[3].iod_nr = 1u;
    udata->md_rw_cb_ud.iod[3].iod_size = udata->link_write_ud->link_val_buf_size;
    udata->md_rw_cb_ud.iod[3].iod_type = DAOS_IOD_SINGLE;

    udata->md_rw_cb_ud.free_akeys = FALSE;

    /* Set up SGL */

    /* Max Link Creation Order Value */
    daos_iov_set(&udata->md_rw_cb_ud.sg_iov[0], udata->max_corder_new_buf, (daos_size_t)H5_DAOS_ENCODED_CRT_ORDER_SIZE);
    udata->md_rw_cb_ud.sgl[0].sg_nr = 1;
    udata->md_rw_cb_ud.sgl[0].sg_nr_out = 0;
    udata->md_rw_cb_ud.sgl[0].sg_iovs = &udata->md_rw_cb_ud.sg_iov[0];

    /* Value for new number of links in group */
    daos_iov_set(&udata->md_rw_cb_ud.sg_iov[1], udata->nlinks_new_buf, (daos_size_t)H5_DAOS_ENCODED_NUM_LINKS_SIZE);
    udata->md_rw_cb_ud.sgl[1].sg_nr = 1;
    udata->md_rw_cb_ud.sgl[1].sg_nr_out = 0;
    udata->md_rw_cb_ud.sgl[1].sg_iovs = &udata->md_rw_cb_ud.sg_iov[1];

    /* Link name value for mapping from link creation order value -> link name */
    daos_iov_set(&udata->md_rw_cb_ud.sg_iov[2], (void *)udata->link_write_ud->link_name_buf,
            (daos_size_t)udata->link_write_ud->link_name_buf_size);
    udata->md_rw_cb_ud.sgl[2].sg_nr = 1;
    udata->md_rw_cb_ud.sgl[2].sg_nr_out = 0;
    udata->md_rw_cb_ud.sgl[2].sg_iovs = &udata->md_rw_cb_ud.sg_iov[2];

    /* Link target value for mapping from link creation order value -> link target */
    daos_iov_set(&udata->md_rw_cb_ud.sg_iov[3], udata->link_write_ud->link_val_buf,
            udata->link_write_ud->link_val_buf_size);
    udata->md_rw_cb_ud.sgl[3].sg_nr = 1;
    udata->md_rw_cb_ud.sgl[3].sg_nr_out = 0;
    udata->md_rw_cb_ud.sgl[3].sg_iovs = &udata->md_rw_cb_ud.sg_iov[3];

    /* Create task for writing link creation order information
     * to the target group.
     */
    if(0 != (ret = daos_task_create(DAOS_OPC_OBJ_UPDATE, &udata->md_rw_cb_ud.obj->item.file->sched, 0, NULL, &update_task)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, ret, "can't create task to write link creation order info: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for link creation order info write */
    if(0 != (ret = tse_task_register_cbs(update_task, H5_daos_md_rw_prep_cb, NULL, 0, H5_daos_link_write_corder_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, ret, "can't register callbacks for task to write link creation order info: %s", H5_daos_err_to_string(ret));

    /* Set private data for link creation order info write */
    (void)tse_task_set_priv(update_task, udata);

    /* Schedule creation order write task and transfer ownership of udata */
    if(0 != (ret = tse_task_schedule(update_task, false)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task to write link: %s", H5_daos_err_to_string(ret));
    udata = NULL;

done:
    /* Clean up */
    if(udata) {
        assert(ret_value < 0);

        /* Decrememnt ref count on udata and close it if it drops to 0 */
        if(--udata->link_write_ud->rc == 0) {
            assert(udata->md_rw_cb_ud.obj == udata->link_write_ud->md_rw_cb_ud.obj);
            /* Close object */
            if(H5_daos_object_close(udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
                D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");
        } /* end if */

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = ret_value;
            udata->md_rw_cb_ud.req->failed_task = "link write creation order info task";
        } /* end if */

        assert(!udata->md_rw_cb_ud.free_dkey);
        assert(!udata->md_rw_cb_ud.free_akeys);

        /* Continue closing udata */
        if(udata->link_write_ud->rc == 0) {
            /* Release our reference to req */
            assert(udata->md_rw_cb_ud.req == udata->link_write_ud->md_rw_cb_ud.req);
            if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
                D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

            /* Complete link write task */
            tse_task_complete(udata->link_write_ud->link_write_task, ret_value);

            /* Free memory */
            DV_free(udata->link_write_ud->link_name_buf);
            DV_free(udata->link_write_ud->link_val_buf);
            udata->link_write_ud = DV_free(udata->link_write_ud);
        } /* end if */

        /* We do not explicitly free the sg_iovs as some of them are
         * not dynamically allocated and the rest are freed from the
         * shared structure above.
         */

        /* Complete this task */
        assert(task == udata->write_corder_task);
        tse_task_complete(task, ret_value);

        DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_wr_corder_info_task() */

static herr_t
H5_daos_link_write_corder_info(H5_daos_group_t *target_grp, uint64_t new_max_corder,
    H5_daos_link_write_ud_t *link_write_ud, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_write_corder_ud_t *write_corder_ud = NULL;
    uint8_t *p;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(target_grp);
    assert(link_write_ud);
    H5daos_compile_assert(H5_DAOS_ENCODED_NUM_LINKS_SIZE == 8);
    H5daos_compile_assert(H5_DAOS_ENCODED_CRT_ORDER_SIZE == 8);
    assert(H5_daos_nlinks_key_size_g != 8);

    /* Allocate argument struct */
    if(NULL == (write_corder_ud = (H5_daos_link_write_corder_ud_t *)DV_calloc(sizeof(H5_daos_link_write_corder_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for link creation order info write callback arguments");

    /* Set up known fields of write_corder_ud */
    write_corder_ud->md_rw_cb_ud.req = req;
    write_corder_ud->md_rw_cb_ud.obj = &target_grp->obj;
    write_corder_ud->md_rw_cb_ud.nr = 4;
    write_corder_ud->link_write_ud = link_write_ud;

    /* Set task name */
    write_corder_ud->md_rw_cb_ud.task_name = "link creation order info write";

    /* Set up dkey */
    daos_iov_set(&write_corder_ud->md_rw_cb_ud.dkey, (void *)H5_daos_link_corder_key_g, H5_daos_link_corder_key_size_g);
    write_corder_ud->md_rw_cb_ud.free_dkey = FALSE;

    /* Read number of links in target group */
    if(H5_daos_group_get_num_links(target_grp, &write_corder_ud->nlinks, req, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTGET, FAIL, "can't get number of links in group");

    /* Encode new max corder buf */
    p = write_corder_ud->max_corder_new_buf;
    UINT64ENCODE(p, new_max_corder);

    /* Create task to finish this operation */
    if(0 !=  (ret = tse_task_create(H5_daos_link_wr_corder_info_task, &target_grp->obj.item.file->sched, write_corder_ud, &write_corder_ud->write_corder_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to write creation order info: %s", H5_daos_err_to_string(ret));

    /* Register task dependency */
    if(*dep_task && 0 != (ret = tse_task_register_deps(write_corder_ud->write_corder_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for link write creation order info: %s", H5_daos_err_to_string(ret));

    /* Schedule creation order write task, give write_corder_ud a reference to
     * link_write_ud, and transfer ownership of write_corder_ud.
     * write_corder_ud and link_write_ud share references to req and target_grp.
     */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(write_corder_ud->write_corder_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task for link write creation order info: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = write_corder_ud->write_corder_task;
    *dep_task = write_corder_ud->write_corder_task;
    link_write_ud->rc++;
    write_corder_ud = NULL;

done:
    /* Clean up */
    if(write_corder_ud) {
        assert(ret_value < 0);
        write_corder_ud = DV_free(write_corder_ud);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_write_corder_info() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_write_corder_comp_cb
 *
 * Purpose:     Completion callback for asynchronous task to write link
 *              creation order information to a target group.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_write_corder_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_link_write_corder_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link creation order info writing task");

    assert(!udata->md_rw_cb_ud.req->file->closed);
    assert(udata->link_write_ud);
    assert(udata->link_write_ud->shared);

    /* Handle errors in update task.  Only record error in udata->req_status if
     * it does not already contain an error (it could contain an error if
     * another task this task is not dependent on also failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->md_rw_cb_ud.req->status = task->dt_result;
        udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
    } /* end if */

done:
    if(udata) {
        /* Decrememnt ref count on udata and close it if it drops to 0 */
        if(--udata->link_write_ud->rc == 0) {
            assert(udata->md_rw_cb_ud.obj == udata->link_write_ud->md_rw_cb_ud.obj);
            /* Close object */
            if(H5_daos_object_close(udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
                D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");
        } /* end if */

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = ret_value;
            udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
        } /* end if */

        assert(!udata->md_rw_cb_ud.free_dkey);
        assert(!udata->md_rw_cb_ud.free_akeys);

        /* Continue closing udata */
        if(udata->link_write_ud->rc == 0) {
            /* Release our reference to req */
            assert(udata->md_rw_cb_ud.req == udata->link_write_ud->md_rw_cb_ud.req);
            if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
                D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

            /* Complete link write task */
            tse_task_complete(udata->link_write_ud->link_write_task, ret_value);

            /* Free memory */
            DV_free(udata->link_write_ud->link_val_buf);
            DV_free(udata->link_write_ud->link_name_buf);
            udata->link_write_ud = DV_free(udata->link_write_ud);
        } /* end if */

        /* We do not explicitly free the sg_iovs as some of them are
         * not dynamically allocated and the rest are freed from the
         * shared structure above.
         */

        /* Complete task */
        tse_task_complete(udata->write_corder_task, ret_value);

        DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_write_corder_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_create
 *
 * Purpose:     Creates a hard/soft/UD/external links.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_create(H5VL_link_create_type_t create_type, void *_item,
    const H5VL_loc_params_t *loc_params, hid_t lcpl_id,
    hid_t H5VL_DAOS_UNUSED lapl_id, hid_t dxpl_id, void **req, va_list arguments)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_obj_t *link_obj = NULL;
    H5_daos_obj_t *target_obj = NULL;
    H5_daos_obj_t *target_loc_obj_hard = NULL;
    char *path_buf = NULL;
    const char *link_name = NULL;
    size_t link_name_len = 0;
    H5_daos_link_val_t link_val;
    H5_daos_req_t *int_req = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    if(!loc_params)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "location parameters object is NULL");
    if(loc_params->type != H5VL_OBJECT_BY_NAME)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "location parameters type is not H5VL_OBJECT_BY_NAME");

    /* Handle usage of the H5L_SAME_LOC macro for H5Lcreate_hard.
     * Usage of this macro can cause `item` to be NULL; however,
     * we need a valid object pointer to create an async request.
     */
    if(H5VL_LINK_CREATE_HARD == create_type) {
        target_loc_obj_hard = va_arg(arguments, void *);

        /* Determine the target location object in which to place
         * the new link. If item is NULL here, H5L_SAME_LOC was
         * used as the third parameter to H5Lcreate_hard, so the
         * target location object is actually the object passed
         * in from the va_arg list.
         */
        if(!item)
            item = (H5_daos_item_t *) target_loc_obj_hard;

        /* Determine the target location object for the object
         * that the hard link is to point to. If target_loc_obj_hard
         * is NULL here, H5L_SAME_LOC was used as the first
         * parameter to H5Lcreate_hard, so the target location
         * object is actually the VOL object that was passed
         * into this callback as a function parameter.
         */
        if(target_loc_obj_hard == NULL)
            target_loc_obj_hard = (H5_daos_obj_t *) item;
    } /* end if */

    /* Now that `item` has been settled for H5Lcreate_hard, we can
     * safely reject a NULL `item` pointer for H5Lcreate(_soft/_external/_ud)
     */
    if(!item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "link loc. object is NULL");

    /* Start H5 operation */
    if(NULL == (int_req = H5_daos_req_create(item->file, H5I_INVALID_HID)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't create DAOS request");

#ifdef H5_DAOS_USE_TRANSACTIONS
    /* Start transaction */
    if(0 != (ret = daos_tx_open(item->file->coh, &int_req->th, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't start transaction");
    int_req->th_open = TRUE;
#endif /* H5_DAOS_USE_TRANSACTIONS */

    switch(create_type) {
        case H5VL_LINK_CREATE_HARD:
        {
            H5VL_loc_params_t *target_loc_params_hard = va_arg(arguments, H5VL_loc_params_t *);

            assert(target_loc_obj_hard);

            link_val.type = H5L_TYPE_HARD;

            if(H5VL_OBJECT_BY_NAME == target_loc_params_hard->type) {
                /* Attempt to open the hard link's target object */
                /* TODO: no logic for 'collective' yet */
                if(H5_daos_object_open_helper((H5_daos_item_t *)target_loc_obj_hard, target_loc_params_hard,
                        NULL, TRUE, &target_obj, int_req, &first_task, &dep_task) < 0)
                    D_GOTO_ERROR(H5E_LINK, H5E_CANTOPENOBJ, FAIL, "couldn't open hard link's target object");

                /* Wait until everything is complete then check for errors
                 * (temporary code until the rest of this function is async) */
                /* Need this because link_write doesn't handle async link_val */
                if(first_task && (0 != (ret = tse_task_schedule(first_task, false))))
                    D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
                if(H5_daos_progress(&item->file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
                    D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
                first_task = NULL;
                dep_task = NULL;
                if(int_req->status < -H5_DAOS_INCOMPLETE)
                    D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(int_req->status));

                link_val.target.hard = target_obj->oid;
            }
            else {
                /* H5Olink */
                if(H5VL_OBJECT_BY_SELF != target_loc_params_hard->type)
                    D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "invalid loc_params type");

                link_val.target.hard = target_loc_obj_hard->oid;
            }

            /*
             * TODO: if the link write succeeds, the link ref. count for
             * the target object should be incremented.
             */

            break;
        } /* H5VL_LINK_CREATE_HARD */

        case H5VL_LINK_CREATE_SOFT:
            /* Retrieve target name */
            link_val.type = H5L_TYPE_SOFT;
            link_val.target.soft = (char *)va_arg(arguments, const char *);

            break;

        case H5VL_LINK_CREATE_UD:
            D_GOTO_ERROR(H5E_LINK, H5E_UNSUPPORTED, FAIL, "UD link creation not supported");
            break;

        default:
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "invalid link creation call");
    } /* end switch */

    /* Find target group */
    /* This needs to be made collective DSINC */
    if(NULL == (link_obj = H5_daos_group_traverse(item, loc_params->loc_data.loc_by_name.name,
            lcpl_id, int_req, FALSE, &path_buf, &link_name, &link_name_len, &first_task, &dep_task)))
        D_GOTO_ERROR(H5E_SYM, H5E_BADITER, FAIL, "can't traverse path");

    /* Check type of link_obj */
    if(link_obj->item.type != H5I_GROUP)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "link object is not a group");

    /* Reject invalid link names during link creation */
    if(link_name_len == 0)
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "path given does not resolve to a final link name");

    /* Create link */
    link_val.target_oid_async = NULL;
    if(0 != (ret = H5_daos_link_write((H5_daos_group_t *)link_obj, link_name, strlen(link_name),
            &link_val, int_req, &first_task, &dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_WRITEERROR, FAIL, "can't create link: %s", H5_daos_err_to_string(ret));

done:
    /* Close link object and target object */
    if(link_obj && H5_daos_object_close(link_obj, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group");
    if(target_obj && H5_daos_object_close(target_obj, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_OBJECT, H5E_CLOSEERROR, FAIL, "can't close object");

    if(int_req) {
        /* Free path_buf if necessary */
        if(path_buf && H5_daos_free_async(item->file, path_buf, &first_task, &dep_task) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, FAIL, "can't free path buffer");

        /* Create task to finalize H5 operation */
        if(0 != (ret = tse_task_create(H5_daos_h5op_finalize, &item->file->sched, int_req, &int_req->finalize_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Register dependency (if any) */
        else if(dep_task && 0 != (ret = tse_task_register_deps(int_req->finalize_task, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Schedule finalize task */
        else if(0 != (ret = tse_task_schedule(int_req->finalize_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        else
            /* finalize_task now owns a reference to req */
            int_req->rc++;

        /* If there was an error during setup, pass it to the request */
        if(ret_value < 0)
            int_req->status = -H5_DAOS_SETUP_ERROR;

        /* Schedule first task */
        if(first_task &&(0 != (ret = tse_task_schedule(first_task, false))))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule first task: %s", H5_daos_err_to_string(ret));

        /* Block until operation completes */
        if(H5_daos_progress(&item->file->sched, int_req, H5_DAOS_PROGRESS_WAIT) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");

        /* Check for failure */
        if(int_req->status < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTOPERATE, FAIL, "link creation failed in task \"%s\": %s", int_req->failed_task, H5_daos_err_to_string(int_req->status));

        /* Close internal request */
        if(H5_daos_req_free_int(int_req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, FAIL, "can't free request");
    } /* end if */

    D_FUNC_LEAVE_API;
} /* end H5_daos_link_create() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_copy_task
 *
 * Purpose:     Asynchronous task for link copy operation.  Most of the
 *              work is done in H5_daos_link_copy_int(), this just writes
 *              the target link once the source link has been read and the
 *              target group opened.  Also schedules a task to finish link
 *              copy after the link write is complete.
 *
 * Return:      0 on success/Negative error code on failure
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_copy_task(tse_task_t *task)
{
    H5_daos_link_copy_ud_t *udata = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link copy task");

    /* Handle errors in previous tasks */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Write link to destination group */
    udata->link_val.target_oid_async = NULL;
    if(0 != (ret = H5_daos_link_write((H5_daos_group_t *)udata->target_obj, udata->new_link_name,
            udata->new_link_name_len, &udata->link_val, udata->req, &first_task, &dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTCOPY, FAIL, "failed to write destination link: %s", H5_daos_err_to_string(ret));

done:
    if(udata) {
        tse_task_t *end_task = NULL;

        /* Create task to finalize gnbn task */
        if(0 !=  (ret = tse_task_create(H5_daos_link_copy_end_task, &udata->req->file->sched, udata, &end_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for link copy end: %s", H5_daos_err_to_string(ret));
        else {
            /* Register task dependency */
            if(dep_task && 0 != (ret = tse_task_register_deps(end_task, 1, &dep_task)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link copy end task: %s", H5_daos_err_to_string(ret));

            /* Schedule copy end task (or save it to be scheduled later) */
            if(first_task) {
                if(0 != (ret = tse_task_schedule(end_task, false)))
                    D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for link copy order end: %s", H5_daos_err_to_string(ret));
            } /* end if */
            else
                first_task = end_task;
            dep_task = end_task;
        } /* end else */

        /* Schedule first task */
        if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link copy: %s", H5_daos_err_to_string(ret));

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->req->status = ret_value;
            udata->req->failed_task = "link copy task";
        } /* end if */
    } /* end if */
    else {
        assert(ret_value == -H5_DAOS_DAOS_GET_ERROR);
        tse_task_complete(task, ret_value);
    } /* end else */

    D_FUNC_LEAVE;
} /* end H5_daos_link_copy_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_copy_end_task
 *
 * Purpose:     Finishes a link copy operation, freeing data and marking
 *              the main link copy task complete.
 *
 * Return:      0 on success/Negative error code on failure
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_copy_end_task(tse_task_t *task)
{
    H5_daos_link_copy_ud_t *udata = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get name by name task");

    /* Handle errors in previous tasks */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT)
        ret_value = -H5_DAOS_PRE_ERROR;
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT)
        ret_value = -H5_DAOS_SHORT_CIRCUIT;

    /* Complete main task */
    tse_task_complete(udata->copy_task, ret_value);

    /* Close destination object */
    if(udata->target_obj && H5_daos_object_close(udata->target_obj, udata->req->dxpl_id, NULL) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close destination object");

    /* Free path_bufs and link value if necessary */
    udata->src_path_buf = DV_free(udata->src_path_buf);
    udata->dst_path_buf = DV_free(udata->dst_path_buf);
    if(udata->link_val.type == H5L_TYPE_SOFT)
        udata->link_val.target.soft = DV_free(udata->link_val.target.soft);

    /* Handle errors in this function */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->req->status = ret_value;
        udata->req->failed_task = "get link name by name order end task";
    } /* end if */

    /* Release our reference to req */
    if(H5_daos_req_free_int(udata->req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Free udata */
    udata = DV_free(udata);

done:
    /* Complete this task */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_link_copy_end_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_copy_int
 *
 * Purpose:     Internal version of H5_daos_link_copy().
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_copy_int(H5_daos_item_t *src_item, const H5VL_loc_params_t *loc_params1,
    H5_daos_item_t *dst_item, const H5VL_loc_params_t *loc_params2, hid_t lcpl,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_copy_ud_t *copy_udata = NULL;
    H5_daos_obj_t *src_obj = NULL;
    const char *src_link_name = NULL;
    size_t src_link_name_len = 0;
    tse_task_t *dep_tasks[2];
    int ret;
    herr_t ret_value = SUCCEED;

    assert(src_item || dst_item);
    assert(loc_params1);
    assert(loc_params1->type == H5VL_OBJECT_BY_NAME);
    assert(loc_params2);
    assert(loc_params2->type == H5VL_OBJECT_BY_NAME);

    /* Allocate task udata struct */
     if(NULL == (copy_udata = (H5_daos_link_copy_ud_t *)DV_calloc(sizeof(H5_daos_link_copy_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate link copy user data");
    copy_udata->req = req;
    copy_udata->link_val.type = H5L_TYPE_ERROR;

    /* Create first task if necessary (so we don't have to keep track of
     * mulptiple first tasks */
    if(!*first_task) {
        if(0 != (ret = (tse_task_create(H5_daos_metatask_autocomplete, &req->file->sched, NULL, first_task))))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create meta task link copy start: %s", H5_daos_err_to_string(ret));
        assert(!*dep_task);
        *dep_task = *first_task;
    } /* end if */

    /* Determine the group containing the link to be copied + the source link's name */
    /* This needs to be made collective DSINC */
    /* Make this work for copying across multiple files DSINC */
    dep_tasks[0] = *dep_task;
    if(NULL == (src_obj = H5_daos_group_traverse(src_item ? src_item : dst_item, /* Accounting for H5L_SAME_LOC usage */
            loc_params1->loc_data.loc_by_name.name, H5P_LINK_CREATE_DEFAULT,
            req, FALSE, &copy_udata->src_path_buf, &src_link_name, &src_link_name_len,
            first_task, &dep_tasks[0])))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get source group and source link name");

    /* Check type of src_obj */
    if(src_obj->item.type != H5I_GROUP)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "source object is not a group");

    /* Determine the target group for the new link + the new link's name */
    dep_tasks[1] = *dep_task;
    if(NULL == (copy_udata->target_obj = H5_daos_group_traverse(dst_item ? dst_item : src_item, /* Accounting for H5L_SAME_LOC usage */
            loc_params2->loc_data.loc_by_name.name, lcpl, req, FALSE,
            &copy_udata->dst_path_buf, &copy_udata->new_link_name, &copy_udata->new_link_name_len,
            first_task, &dep_tasks[1])))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get destination group and destination link name");

    /* Check type of target_obj */
    if(copy_udata->target_obj->item.type != H5I_GROUP)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "target object is not a group");

    /* Reject invalid link names during link copy */
    if(src_link_name_len == 0)
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "source path given does not resolve to a final link name");
    if(copy_udata->new_link_name_len == 0)
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "destination path given does not resolve to a final link name");

    /* Retrieve the source link's value */
    if(H5_daos_link_read((H5_daos_group_t *)src_obj, src_link_name, src_link_name_len, req,
            &copy_udata->link_val, NULL, first_task, &dep_tasks[0]) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_READERROR, FAIL, "can't read source link");

    /* Create task to finish this operation (write the target link) */
    if(0 !=  (ret = tse_task_create(H5_daos_link_copy_task, &req->file->sched, copy_udata, &copy_udata->copy_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task for link copy: %s", H5_daos_err_to_string(ret));

    /* Register task dependencies - this task depends both on the source link
     * read and the target group open */
    if(0 != (ret = tse_task_register_deps(copy_udata->copy_task, 2, dep_tasks)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for link copy task: %s", H5_daos_err_to_string(ret));

    /* Schedule copy task and give it a reference to req and udata */
    assert(*first_task);
    if(0 != (ret = tse_task_schedule(copy_udata->copy_task, false)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task for link copy: %s", H5_daos_err_to_string(ret));
    *dep_task = copy_udata->copy_task;
    req->rc++;
    copy_udata = NULL;

done:
    /* Close source object */
    if(src_obj && H5_daos_object_close(src_obj, req->dxpl_id, NULL) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close source object");

    /* Clean up on failure */
    if(copy_udata) {
        assert(ret_value < 0);
        if(copy_udata->target_obj && H5_daos_object_close(copy_udata->target_obj, req->dxpl_id, NULL) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close target object");
        copy_udata->src_path_buf = DV_free(copy_udata->src_path_buf);
        copy_udata->dst_path_buf = DV_free(copy_udata->dst_path_buf);
        if(copy_udata->link_val.type == H5L_TYPE_SOFT)
            copy_udata->link_val.target.soft = DV_free(copy_udata->link_val.target.soft);
        copy_udata = DV_free(copy_udata);
    } /* end if */

    D_FUNC_LEAVE_API;
} /* end H5_daos_link_copy_int() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_copy
 *
 * Purpose:     Copies a link.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_copy(void *src_item, const H5VL_loc_params_t *loc_params1,
    void *dst_item, const H5VL_loc_params_t *loc_params2, hid_t lcpl,
    hid_t H5VL_DAOS_UNUSED lapl, hid_t dxpl_id, void H5VL_DAOS_UNUSED **req)
{
    H5_daos_file_t *sched_file = NULL;
    H5_daos_req_t *int_req = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    if(!src_item && !dst_item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "source object location and destination object location are both NULL");
    if(!loc_params1)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "source location parameters object is NULL");
    if(loc_params1->type != H5VL_OBJECT_BY_NAME)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "invalid source location parameters type");
    if(!loc_params2)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "destination location parameters object is NULL");
    if(loc_params2->type != H5VL_OBJECT_BY_NAME)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "invalid destination location parameters type");

    /* Set convenience pointer to file to use for scheduling */
    sched_file = src_item ? ((H5_daos_item_t *)src_item)->file : ((H5_daos_item_t *)dst_item)->file;

    /* Start H5 operation */
    /* If we ever remove the dxpl_id from H5_daos_object_close, we should be
     * able to remove the dxpl_id from here */
    if(NULL == (int_req = H5_daos_req_create(sched_file, dxpl_id)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't create DAOS request");

#ifdef H5_DAOS_USE_TRANSACTIONS
    /* Start transaction */
    if(0 != (ret = daos_tx_open(sched_file->coh, &int_req->th, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't start transaction");
    int_req->th_open = TRUE;
#endif /* H5_DAOS_USE_TRANSACTIONS */

    /* Call internal routine */
    if(H5_daos_link_copy_int((H5_daos_item_t *)src_item, loc_params1,
            (H5_daos_item_t *)dst_item, loc_params2, lcpl, int_req,
            &first_task, &dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTCOPY, FAIL, "can't copy link");

done:
    if(int_req) {
        assert(sched_file);

        /* Create task to finalize H5 operation */
        if(0 != (ret = tse_task_create(H5_daos_h5op_finalize, &sched_file->sched, int_req, &int_req->finalize_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Register dependency (if any) */
        else if(dep_task && 0 != (ret = tse_task_register_deps(int_req->finalize_task, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Schedule finalize task */
        else if(0 != (ret = tse_task_schedule(int_req->finalize_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        else
            /* finalize_task now owns a reference to req */
            int_req->rc++;

        /* If there was an error during setup, pass it to the request */
        if(ret_value < 0)
            int_req->status = -H5_DAOS_SETUP_ERROR;

        /* Schedule first_task */
        if(first_task && (0 != (ret = tse_task_schedule(first_task, false))))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule first task: %s", H5_daos_err_to_string(ret));

        /* Block until operation completes */
        if(H5_daos_progress(&sched_file->sched, int_req, H5_DAOS_PROGRESS_WAIT) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");

        /* Check for failure */
        if(int_req->status < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTOPERATE, FAIL, "link write failed in task \"%s\": %s", int_req->failed_task, H5_daos_err_to_string(int_req->status));

        /* Close internal request */
        if(H5_daos_req_free_int(int_req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, FAIL, "can't free request");
    } /* end if */

    D_FUNC_LEAVE_API;
} /* end H5_daos_link_copy() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_move
 *
 * Purpose:     Moves a link.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_move(void *src_item, const H5VL_loc_params_t *loc_params1,
    void *dst_item, const H5VL_loc_params_t *loc_params2, hid_t lcpl,
    hid_t H5VL_DAOS_UNUSED lapl, hid_t H5VL_DAOS_UNUSED dxpl_id, void H5VL_DAOS_UNUSED **req)
{
    H5_daos_link_val_t *link_val = NULL;
    H5_daos_obj_t *src_obj = NULL;
    H5_daos_obj_t *target_obj = NULL;
    char *src_path_buf = NULL;
    char *dst_path_buf = NULL;
    const char *src_link_name = NULL;
    const char *new_link_name = NULL;
    size_t src_link_name_len = 0;
    size_t new_link_name_len = 0;
    H5_daos_file_t *sched_file = NULL;
    //int finalize_ndeps = 0;
    //tse_task_t *finalize_deps[2];
    H5_daos_req_t *int_req = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    if(!src_item && !dst_item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "source object location and destination object location are both NULL");
    if(!loc_params1)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "source location parameters object is NULL");
    if(loc_params1->type != H5VL_OBJECT_BY_NAME)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "invalid source location parameters type");
    if(!loc_params2)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "destination location parameters object is NULL");
    if(loc_params2->type != H5VL_OBJECT_BY_NAME)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "invalid destination location parameters type");

    /* Set convenience pointer to file to use for scheduling */
    sched_file = src_item ? ((H5_daos_item_t *)src_item)->file : ((H5_daos_item_t *)dst_item)->file;

    /* Start H5 operation */
    if(NULL == (int_req = H5_daos_req_create(sched_file, H5I_INVALID_HID)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't create DAOS request");

#ifdef H5_DAOS_USE_TRANSACTIONS
    /* Start transaction */
    if(0 != (ret = daos_tx_open(sched_file->coh, &int_req->th, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't start transaction");
    int_req->th_open = TRUE;
#endif /* H5_DAOS_USE_TRANSACTIONS */

    /* Determine the group containing the link to be copied + the source link's name */
    /* This needs to be made collective DSINC */
    /* Could do these traverses in parallel DSINC */
    /* Make this work for copying across multiple files DSINC */
    if(NULL == (src_obj = H5_daos_group_traverse(src_item ? src_item : dst_item, /* Accounting for H5L_SAME_LOC usage */
            loc_params1->loc_data.loc_by_name.name, H5P_LINK_CREATE_DEFAULT,
            int_req, FALSE, &src_path_buf, &src_link_name, &src_link_name_len,
            &first_task, &dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get source group and source link name");

    /* Check type of src_obj */
    if(src_obj->item.type != H5I_GROUP)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "source object is not a group");

    /* Determine the target group for the new link + the new link's name */
    if(NULL == (target_obj = H5_daos_group_traverse(dst_item ? dst_item : src_item, /* Accounting for H5L_SAME_LOC usage */
            loc_params2->loc_data.loc_by_name.name, lcpl, int_req, FALSE,
            &dst_path_buf, &new_link_name, &new_link_name_len, &first_task,
            &dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get destination group and destination link name");

    /* Check type of target_obj */
    if(target_obj->item.type != H5I_GROUP)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "target object is not a group");

    /* Reject invalid link names during link creation */
    if(src_link_name_len == 0)
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "source path given does not resolve to a final link name");
    if(new_link_name_len == 0)
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "destination path given does not resolve to a final link name");

    /* Allocate link value */
    if(NULL == (link_val = (H5_daos_link_val_t *)DV_malloc(sizeof(H5_daos_link_val_t))))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't allocate space for link value");
    link_val->type = H5L_TYPE_ERROR;

    /* Retrieve the source link's value */
    if(H5_daos_link_read((H5_daos_group_t *)src_obj, src_link_name, src_link_name_len, int_req,
            link_val, NULL, &first_task, &dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_READERROR, FAIL, "can't read source link");

    /* Wait until everything is complete then check for errors
     * (temporary code until the rest of this function is async) */
    /* Need this because link_write doesn't handle async link_val */
    if(first_task && (0 != (ret = tse_task_schedule(first_task, false))))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
    if(H5_daos_progress(&sched_file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
    first_task = NULL;
    dep_task = NULL;
    if(int_req->status < -H5_DAOS_INCOMPLETE)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(int_req->status));

    link_val->target_oid_async = NULL;
    if(0 != (ret = H5_daos_link_write((H5_daos_group_t *)target_obj, new_link_name, new_link_name_len,
            link_val, int_req, &first_task, &dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTCOPY, FAIL, "failed to copy link: %s", H5_daos_err_to_string(ret));

    /* Remove the original link */
    if(H5_daos_link_delete((src_item ? (H5_daos_item_t *)src_item : (H5_daos_item_t *)dst_item), loc_params1, int_req, &first_task, &dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTREMOVE, FAIL, "can't delete original link");

done:
    /* Close source and destination objects */
    if(target_obj && H5_daos_object_close(target_obj, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close destination object");
    if(src_obj && H5_daos_object_close(src_obj, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close source object");

    if(int_req) {
        assert(sched_file);

        /* Free path_bufs if necessary */
        if(src_path_buf && H5_daos_free_async(sched_file, src_path_buf, &first_task, &dep_task) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, FAIL, "can't free path buffer");
        if(dst_path_buf && H5_daos_free_async(sched_file, dst_path_buf, &first_task, &dep_task) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, FAIL, "can't free path buffer");
        if(link_val) {
            if((link_val->type == H5L_TYPE_SOFT) && H5_daos_free_async(
                    sched_file, link_val->target.soft, &first_task, &dep_task)
                    < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, FAIL, "can't free soft link value");
            if(H5_daos_free_async(sched_file, link_val, &first_task, &dep_task) < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, FAIL, "can't free link value");
        } /* end if */

        /* Create task to finalize H5 operation */
        if(0 != (ret = tse_task_create(H5_daos_h5op_finalize, &sched_file->sched, int_req, &int_req->finalize_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Register dependency (if any) */
        else if(dep_task && 0 != (ret = tse_task_register_deps(int_req->finalize_task, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Schedule finalize task */
        else if(0 != (ret = tse_task_schedule(int_req->finalize_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        else
            /* finalize_task now owns a reference to req */
            int_req->rc++;

        /* If there was an error during setup, pass it to the request */
        if(ret_value < 0)
            int_req->status = -H5_DAOS_SETUP_ERROR;

        /* Schedule first_task */
        if(first_task && (0 != (ret = tse_task_schedule(first_task, false))))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule first task: %s", H5_daos_err_to_string(ret));

        /* Block until operation completes */
        if(H5_daos_progress(&sched_file->sched, int_req, H5_DAOS_PROGRESS_WAIT) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");

        /* Check for failure */
        if(int_req->status < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTOPERATE, FAIL, "link write failed in task \"%s\": %s", int_req->failed_task, H5_daos_err_to_string(int_req->status));

        /* Close internal request */
        if(H5_daos_req_free_int(int_req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, FAIL, "can't free request");
    } /* end if */

    D_FUNC_LEAVE_API;
} /* end H5_daos_link_move() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get
 *
 * Purpose:     Gets information about a link.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_get(void *_item, const H5VL_loc_params_t *loc_params,
    H5VL_link_get_t get_type, hid_t dxpl_id, void **req, va_list arguments)
{
    H5_daos_group_t    *target_grp = NULL;
    H5_daos_item_t     *item = (H5_daos_item_t *)_item;
    H5_daos_req_t      *int_req = NULL;
    tse_task_t         *first_task = NULL;
    tse_task_t         *dep_task = NULL;
    int                 ret;
    herr_t              ret_value = SUCCEED;

    if(!_item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "VOL object is NULL");
    if(!loc_params)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "location parameters object is NULL");

    /* Start H5 operation */
    if(NULL == (int_req = H5_daos_req_create(item->file, H5I_INVALID_HID)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't create DAOS request");

#ifdef H5_DAOS_USE_TRANSACTIONS
    /* Start transaction */
    if(0 != (ret = daos_tx_open(item->file->coh, &int_req->th, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't start transaction");
    int_req->th_open = TRUE;
#endif /* H5_DAOS_USE_TRANSACTIONS */

    /* Determine group containing link in question */
    switch (loc_params->type) {
        case H5VL_OBJECT_BY_SELF:
            /* Use item as link's parent group, or the root group if item is a file */
            if(item->type == H5I_FILE)
                target_grp = ((H5_daos_file_t *)item)->root_grp;
            else if(item->type == H5I_GROUP)
                target_grp = (H5_daos_group_t *)item;
            else
                D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "item not a file or group");

            target_grp->obj.item.rc++;
            break;

        case H5VL_OBJECT_BY_NAME:
            /* Open target group */
            if(NULL == (target_grp = (H5_daos_group_t *)H5_daos_object_open(item, loc_params, NULL, dxpl_id, req)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTOPENOBJ, FAIL, "can't open target group for link");
            break;

        case H5VL_OBJECT_BY_IDX:
        {
            H5VL_loc_params_t by_name_params;

            /* Setup loc_params for opening target group */
            by_name_params.type = H5VL_OBJECT_BY_NAME;
            by_name_params.obj_type = H5I_GROUP;
            by_name_params.loc_data.loc_by_name.name = loc_params->loc_data.loc_by_idx.name;
            by_name_params.loc_data.loc_by_name.lapl_id = loc_params->loc_data.loc_by_idx.lapl_id;

            /* Open target group */
            if(NULL == (target_grp = (H5_daos_group_t *)H5_daos_object_open(item, &by_name_params, NULL, dxpl_id, req)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTOPENOBJ, FAIL, "can't open target group for link");
            break;
        }

        case H5VL_OBJECT_BY_TOKEN:
        default:
            D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "invalid loc_params type");
    } /* end switch */

    switch (get_type) {
        case H5VL_LINK_GET_INFO:
        {
            H5L_info2_t *link_info = va_arg(arguments, H5L_info2_t *);

            if(0 != (ret = H5_daos_link_get_info(item, loc_params, link_info, NULL, int_req, &first_task, &dep_task)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't retrieve link's info: %s", H5_daos_err_to_string(ret));

            break;
        } /* H5VL_LINK_GET_INFO */

        case H5VL_LINK_GET_NAME:
        {
            char *name_out = va_arg(arguments, char *);
            size_t name_out_size = va_arg(arguments, size_t);
            ssize_t *ret_size = va_arg(arguments, ssize_t *);

            /* Pass ret_size as size_t * - this should be fine since if the call
             * fails the HDF5 library will assign -1 to the return value anyways
             */
            if(H5_daos_link_get_name_by_idx(target_grp, loc_params->loc_data.loc_by_idx.idx_type,
                    loc_params->loc_data.loc_by_idx.order, (uint64_t)loc_params->loc_data.loc_by_idx.n,
                    (size_t *)ret_size, name_out, name_out_size, int_req, &first_task, &dep_task) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't retrieve link's name");

            break;
        } /* H5VL_LINK_GET_NAME */

        case H5VL_LINK_GET_VAL:
        {
            void *out_buf = va_arg(arguments, void *);
            size_t out_buf_size = va_arg(arguments, size_t);

            if(H5_daos_link_get_val(item, loc_params, out_buf, out_buf_size, int_req, &first_task, &dep_task) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get link's value");

            break;
        } /* H5VL_LINK_GET_VAL */

        default:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "invalid or unsupported link get operation");
    } /* end switch */

done:
    if(int_req) {
        /* Create task to finalize H5 operation */
        if(0 != (ret = tse_task_create(H5_daos_h5op_finalize, &item->file->sched, int_req, &int_req->finalize_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Register dependencies (if any) */
        else if(dep_task && 0 != (ret = tse_task_register_deps(int_req->finalize_task, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Schedule finalize task */
        else if(0 != (ret = tse_task_schedule(int_req->finalize_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        else
            /* finalize_task now owns a reference to req */
            int_req->rc++;

        /* If there was an error during setup, pass it to the request */
        if(ret_value < 0)
            int_req->status = -H5_DAOS_SETUP_ERROR;

        /* Schedule first_task */
        if(first_task && (0 != (ret = tse_task_schedule(first_task, false))))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule first task: %s", H5_daos_err_to_string(ret));

        /* Block until operation completes */
        if(H5_daos_progress(&item->file->sched, int_req, H5_DAOS_PROGRESS_WAIT) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");

        /* Check for failure */
        if(int_req->status < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTOPERATE, FAIL, "link creation failed in task \"%s\": %s", int_req->failed_task, H5_daos_err_to_string(int_req->status));

        /* Close internal request */
        if(H5_daos_req_free_int(int_req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, FAIL, "can't free request");
    } /* end if */

    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group");

    D_FUNC_LEAVE_API;
} /* end H5_daos_link_get() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_specific
 *
 * Purpose:     Specific operations with links
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_specific(void *_item, const H5VL_loc_params_t *loc_params,
    H5VL_link_specific_t specific_type, hid_t dxpl_id, void **req,
    va_list arguments)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_group_t *target_grp = NULL;
    hid_t target_grp_id = -1;
    H5_daos_req_t      *int_req = NULL;
    herr_t              iter_ret = 0;
    tse_task_t         *first_task = NULL;
    tse_task_t         *dep_task = NULL;
    int                 ret;
    herr_t ret_value = SUCCEED;    /* Return value */

    if(!_item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "VOL object is NULL");
    if(!loc_params)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "location parameters object is NULL");

    /* Start H5 operation */
    if(NULL == (int_req = H5_daos_req_create(item->file, dxpl_id)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTALLOC, FAIL, "can't create DAOS request");

#ifdef H5_DAOS_USE_TRANSACTIONS
    /* Start transaction */
    if(0 != (ret = daos_tx_open(item->file->coh, &int_req->th, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't start transaction");
    int_req->th_open = TRUE;
#endif /* H5_DAOS_USE_TRANSACTIONS */

    switch (specific_type) {
        /* H5Lexists */
        case H5VL_LINK_EXISTS:
            {
                htri_t *lexists_ret = va_arg(arguments, htri_t *);

                assert(H5VL_OBJECT_BY_NAME == loc_params->type);

                if((*lexists_ret = H5_daos_link_exists(item, loc_params->loc_data.loc_by_name.name, int_req, &first_task, &dep_task)) < 0)
                    D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't determine if link exists");

                break;
            } /* end block */

        /* H5Literate(_by_name)/visit(_by_name) */
        case H5VL_LINK_ITER:
            {
                H5_daos_iter_data_t iter_data;
                int is_recursive = va_arg(arguments, int);
                H5_index_t idx_type = (H5_index_t) va_arg(arguments, int);
                H5_iter_order_t iter_order = (H5_iter_order_t) va_arg(arguments, int);
                hsize_t *idx_p = va_arg(arguments, hsize_t *);
                H5L_iterate2_t iter_op = va_arg(arguments, H5L_iterate2_t);
                void *op_data = va_arg(arguments, void *);

                /* Determine group containing link in question */
                switch (loc_params->type) {
                    /* H5Literate/H5Lvisit */
                    case H5VL_OBJECT_BY_SELF:
                    {
                        /* Use item as link's parent group, or the root group if item is a file */
                        if(item->type == H5I_FILE)
                            target_grp = ((H5_daos_file_t *)item)->root_grp;
                        else if(item->type == H5I_GROUP)
                            target_grp = (H5_daos_group_t *)item;
                        else
                            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "item not a file or group");

                        target_grp->obj.item.rc++;
                        break;
                    } /* H5VL_OBJECT_BY_SELF */

                    /* H5Literate_by_name/H5Lvisit_by_name */
                    case H5VL_OBJECT_BY_NAME:
                    {
                        H5VL_loc_params_t sub_loc_params;

                        /* Open target_grp */
                        sub_loc_params.obj_type = item->type;
                        sub_loc_params.type = H5VL_OBJECT_BY_SELF;
                        if(NULL == (target_grp = (H5_daos_group_t *)H5_daos_group_open_int(item, &sub_loc_params,
                                loc_params->loc_data.loc_by_name.name, H5P_GROUP_ACCESS_DEFAULT, int_req, FALSE, &first_task, &dep_task)))
                            D_GOTO_ERROR(H5E_SYM, H5E_CANTOPENOBJ, FAIL, "can't open group for link operation");

                        break;
                    } /* H5VL_OBJECT_BY_NAME */

                    case H5VL_OBJECT_BY_IDX:
                    case H5VL_OBJECT_BY_TOKEN:
                    default:
                        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "invalid loc_params type");
                } /* end switch */

                /* Register id for target_grp */
                if((target_grp_id = H5VLwrap_register(target_grp, H5I_GROUP)) < 0)
                    D_GOTO_ERROR(H5E_ATOM, H5E_CANTREGISTER, FAIL, "unable to atomize object handle");

                /* Initialize iteration data */
                H5_DAOS_ITER_DATA_INIT(iter_data, H5_DAOS_ITER_TYPE_LINK, idx_type, iter_order,
                        is_recursive, idx_p, target_grp_id, op_data, dxpl_id, int_req, &first_task, &dep_task);
                iter_data.u.link_iter_data.u.link_iter_op = iter_op;

                /* Handle iteration return value (TODO: how to handle if called
                 * async? */
                if(!req)
                    iter_data.op_ret_p = &iter_ret;

                if(H5_daos_link_iterate(target_grp, &iter_data, &first_task, &dep_task) < 0)
                    D_GOTO_ERROR(H5E_LINK, H5E_BADITER, FAIL, "link iteration failed");

                break;
            } /* end block */

        /* H5Ldelete(_by_idx) */
        case H5VL_LINK_DELETE:
            if(H5_daos_link_delete(item, loc_params, int_req, &first_task, &dep_task) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CANTREMOVE, FAIL, "failed to delete link");
            break;

        default:
            D_GOTO_ERROR(H5E_VOL, H5E_BADVALUE, FAIL, "invalid specific operation");
    } /* end switch */

done:
    if(int_req) {
        /* Create task to finalize H5 operation */
        if(0 != (ret = tse_task_create(H5_daos_h5op_finalize, &item->file->sched, int_req, &int_req->finalize_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Register dependencies (if any) */
        else if(dep_task && 0 != (ret = tse_task_register_deps(int_req->finalize_task, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        /* Schedule finalize task */
        else if(0 != (ret = tse_task_schedule(int_req->finalize_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task to finalize H5 operation: %s", H5_daos_err_to_string(ret));
        else
            /* finalize_task now owns a reference to req */
            int_req->rc++;

        /* If there was an error during setup, pass it to the request */
        if(ret_value < 0)
            int_req->status = -H5_DAOS_SETUP_ERROR;

        /* Schedule first_task */
        if(first_task && (0 != (ret = tse_task_schedule(first_task, false))))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule first task: %s", H5_daos_err_to_string(ret));

        /* Block until operation completes */
        if(H5_daos_progress(&item->file->sched, int_req, H5_DAOS_PROGRESS_WAIT) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");

        /* Check for failure */
        if(int_req->status < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTOPERATE, FAIL, "link specific operation failed in task \"%s\": %s", int_req->failed_task, H5_daos_err_to_string(int_req->status));

        /* Close internal request */
        if(H5_daos_req_free_int(int_req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, FAIL, "can't free request");

        /* Set return value for link iteration, unless this function failed but
         * the iteration did not */
        if(specific_type == H5VL_LINK_ITER && !(ret_value < 0 && iter_ret >= 0))
            ret_value = iter_ret;
    } /* end if */

    if(target_grp_id >= 0) {
        if(H5Idec_ref(target_grp_id) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group ID");
        target_grp_id = -1;
        target_grp = NULL;
    } /* end if */
    else if(target_grp) {
        if(H5_daos_group_close(target_grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group");
        target_grp = NULL;
    } /* end else */

    D_FUNC_LEAVE_API;
} /* end H5_daos_link_specific() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_follow_end
 *
 * Purpose:     Asynchronous task for finalizing H5_daos_link_follow.
 *              Executes after soft link traversal or intermediate group
 *              creation from H5_daos_link_follow_task.  Cleans up udata
 *              and completes the follow task.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              April, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_follow_end(tse_task_t *task)
{
    H5_daos_link_follow_ud_t *udata = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for MPI broadcast task");

    assert(udata->req);
    assert(udata->req->file);
    assert(udata->grp);
    assert(udata->grp->obj.item.file == udata->req->file);

    /* Complete follow task - just set return value to 0 since it's not examined
     * anyway (errors are handled through req->status) */
    tse_task_complete(udata->follow_task, 0);

    /* Close group */
    if(H5_daos_object_close(udata->grp, H5I_INVALID_HID, NULL) < 0)
        D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");

    /* Release our reference to req */
    if(H5_daos_req_free_int(udata->req) < 0)
        D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Free soft link path if present */
    if(udata->link_val.type == H5L_TYPE_SOFT)
        DV_free(udata->link_val.target.soft);

    /* Free path buffer and private data struct */
    DV_free(udata->path_buf);
    DV_free(udata);

done:
    /* Complete this task */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_link_follow_end() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_follow_task
 *
 * Purpose:     Asynchronous task for H5_daos_link_follow.  Executes after
 *              H5_daos_link_read.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              April, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_follow_task(tse_task_t *task)
{
    H5_daos_link_follow_ud_t *udata = NULL;
    H5_daos_group_t *target_grp = NULL;
    H5_daos_req_t *req = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for MPI broadcast task");

    assert(udata->req);
    assert(udata->req->file);
    assert(udata->grp);
    assert(udata->grp->obj.item.file == udata->req->file);
    assert(udata->oid);
    assert(!udata->path_buf);
    assert(udata->follow_task == task);

    /* Assign req convenience pointer.  We do this so we can still handle errors
     * after transfering ownership of udata.  This should be safe since we
     * increase the ref count on req when we transfer ownership. */
    req = udata->req;

    /* Handle errors in previous tasks */
    if(req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Check if the link was read */
    if(udata->link_read) {
        switch(udata->link_val.type) {
           case H5L_TYPE_HARD:
                /* Simply return the read oid and */
                *udata->oid = udata->link_val.target.hard;

                if(udata->link_exists)
                    *udata->link_exists = TRUE;

                break;

            case H5L_TYPE_SOFT:
                {
                    const char *target_name = NULL;
                    size_t target_name_len;
                    daos_obj_id_t **oid_ptr = NULL;

                    /* Traverse the path */
                    if(NULL == (target_grp = (H5_daos_group_t *)H5_daos_group_traverse(&udata->grp->obj.item,
                            udata->link_val.target.soft, H5P_LINK_CREATE_DEFAULT, req, FALSE,
                            &udata->path_buf, &target_name, &target_name_len, &first_task,
                            &dep_task)))
                        D_GOTO_ERROR(H5E_LINK, H5E_TRAVERSE, -H5_DAOS_TRAVERSE_ERROR, "can't traverse path");
                    assert(target_grp->obj.item.type == H5I_GROUP);

                    /* Check for no target_name, in this case just return target_grp */
                    if(target_name_len == 0) {
                        /* Output oid of target_grp */
                        *udata->oid = target_grp->obj.oid;
                    } /* end if */
                    else {
                        /* Follow link to group */
                        if(H5_daos_link_follow(target_grp, target_name, target_name_len,
                                FALSE, req, &oid_ptr, udata->link_exists, &first_task,
                                &dep_task) < 0)
                            D_GOTO_ERROR(H5E_LINK, H5E_TRAVERSE, -H5_DAOS_FOLLOW_ERROR, "can't follow link to group");

                        /* Retarget oid_ptr to grp->obj.oid so
                         * H5_daos_link_follow fills in udata->oid */
                        *oid_ptr = udata->oid;
                    } /* end else */

                    /* Close target_grp */
                    if(H5_daos_group_close(target_grp, req->dxpl_id, NULL) < 0)
                        D_GOTO_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
                    target_grp = NULL;

                    break;
                } /* end block */

            case H5L_TYPE_EXTERNAL:
                D_GOTO_ERROR(H5E_LINK, H5E_UNSUPPORTED, -H5_DAOS_BAD_VALUE, "following of external links is unsupported");
                break;

            case H5L_TYPE_ERROR:
            case H5L_TYPE_MAX:
            default:
               D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "invalid link type");
        } /* end switch */
    } /* end if */
    else {
        if(udata->crt_missing_grp) {
            /* Create missing group and link to group */
            if(NULL == (target_grp = (H5_daos_group_t *)H5_daos_group_create_helper(udata->grp->obj.item.file, FALSE,
                    H5P_GROUP_CREATE_DEFAULT, H5P_GROUP_ACCESS_DEFAULT, req, udata->grp, udata->name,
                    udata->name_len, FALSE, &first_task, &dep_task)))
                D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, -H5_DAOS_SETUP_ERROR, "can't create missing group");

            /* Output oid of target_grp */
            *udata->oid = target_grp->obj.oid;

            /* Close target_grp */
            if(H5_daos_group_close(target_grp, req->dxpl_id, NULL) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
            target_grp = NULL;

            assert(!udata->link_exists);
        } /* end if */
        else {
            if(udata->link_exists)
                *udata->link_exists = FALSE;
            else
                D_GOTO_ERROR(H5E_LINK, H5E_TRAVERSE, -H5_DAOS_NONEXIST_LINK, "link not found");
        } /* end else */
    } /* end else */

done:
    /* Check for tasks scheduled, in this case we need to schedule a task
     * to mark this task complete and possibly free path_buf */
    if(dep_task) {
        tse_task_t *follow_end_task;

        /* Schedule task to complete this task and free path buf */
        if(0 != (ret = tse_task_create(H5_daos_link_follow_end, &udata->grp->obj.item.file->sched, udata, &follow_end_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task to finish following link: %s", H5_daos_err_to_string(ret));
        else {
            /* Register dependency for task */
            if(0 != (ret = tse_task_register_deps(follow_end_task, 1, &dep_task)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link follow: %s", H5_daos_err_to_string(ret));

            /* Schedule link follow task and give it ownership of udata, while
             * keeping a reference to req for ourselves */
            assert(first_task);
            req->rc++;
            if(0 != (ret = tse_task_schedule(follow_end_task, false)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task to finish following link: %s", H5_daos_err_to_string(ret));
            udata = NULL;
            dep_task = follow_end_task;
        } /* end else */

        /* Schedule first task */
        assert(first_task);
        if(0 != (ret = tse_task_schedule(first_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link follow: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        assert(!first_task);

    /* Close group and free soft link path if we still own udata */
    if(udata)
        if(H5_daos_group_close(udata->grp, H5I_INVALID_HID, NULL) < 0)
            D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");

    /* Handle errors */
    if(ret_value < 0) {
        /* Clean up */
        if(udata)
            udata->path_buf = DV_free(udata->path_buf);

        if(target_grp) {
            assert(req);
            if(H5_daos_group_close(target_grp, req->dxpl_id, NULL) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CLOSEERROR, FAIL, "can't close group");
            target_grp = NULL;
        } /* end if */

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            req->status = ret_value;
            req->failed_task = "link follow";
        } /* end if */
    } /* end if */

    /* Release our reference to req */
    if(req && H5_daos_req_free_int(req) < 0)
        D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Complete task and free udata if we still own udata */
    if(udata) {
        /* Complete this task */
        tse_task_complete(task, ret_value);

        /* Free soft link path */
        if(udata->link_val.type == H5L_TYPE_SOFT)
            DV_free(udata->link_val.target.soft);

        /* Free private data */
        assert(!udata->path_buf);
        udata = DV_free(udata);
    } /* end if */
    else
        assert(ret_value >= 0);

    /* Make sure we cleaned up */
    assert(!udata);
    assert(!target_grp);

    D_FUNC_LEAVE;
} /* end H5_daos_link_follow_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_follow
 *
 * Purpose:     Follows the link in grp identified with name, and returns
 *              in oid the oid of the target object.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Neil Fortner
 *              January, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_follow(H5_daos_group_t *grp, const char *name, size_t name_len,
    hbool_t crt_missing_grp, H5_daos_req_t *req, daos_obj_id_t ***oid_ptr,
    hbool_t *link_exists, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_follow_ud_t *follow_udata = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(grp);
    assert(name);
    assert(oid_ptr);

    /* Allocate private data for follow task */
    if(NULL == (follow_udata = (H5_daos_link_follow_ud_t *)DV_malloc(sizeof(H5_daos_link_follow_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate user data struct for object open task");

    /* Read link to group */
    if(H5_daos_link_read(grp, name, name_len, req, &follow_udata->link_val, &follow_udata->link_read, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't read link");

    /* Create task for link follow */
    if(0 != (ret = tse_task_create(H5_daos_link_follow_task, &grp->obj.item.file->sched, follow_udata, &follow_udata->follow_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task to follow link: %s", H5_daos_err_to_string(ret));

    /* Register dependency for task */
    assert(*dep_task);
    if(0 != (ret = tse_task_register_deps(follow_udata->follow_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for link follow: %s", H5_daos_err_to_string(ret));

    /* Set private data for link follow */
    follow_udata->req = req;
    follow_udata->grp = grp;
    follow_udata->name = name;
    follow_udata->name_len = name_len;
    follow_udata->crt_missing_grp = crt_missing_grp;
    follow_udata->link_exists = link_exists;
    follow_udata->path_buf = NULL;
    follow_udata->link_val.type = H5L_TYPE_ERROR;

    /* Set *oid_ptr so calling function can direct output of link follow task */
    *oid_ptr = &follow_udata->oid;
#ifndef NDEBUG
    /* Set oid to NULL so we'll get a helpful assertion failure if it doesn't
     * get set */
    follow_udata->oid = NULL;
#endif /* NDEBUG */

    /* Schedule link follow task and give it a reference to req and grp, and
     * ownership of follow_udata */
    assert(*first_task);
    if(0 != (ret = tse_task_schedule(follow_udata->follow_task, false)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to follow link: %s", H5_daos_err_to_string(ret));
    req->rc++;
    grp->obj.item.rc++;
    *dep_task = follow_udata->follow_task;
    follow_udata = NULL;

done:
    /* Cleanup */
    if(follow_udata) {
        assert(ret_value < 0);
        follow_udata = DV_free(follow_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_follow() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_info_end_task
 *
 * Purpose:     Finishes the link get info operation, setting output and
 *              freeing data.
 *
 * Return:      Success:        0
 *              Failure:        FAIL
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_get_info_end_task(tse_task_t *task)
{
    H5_daos_link_get_info_ud_t *udata = NULL;
    tse_task_t *metatask = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get name by creation order task");

    /* Handle errors in previous tasks */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /*
     * Fill in link type and link object address (hard link) or
     * link value size (soft link) fields, then free the link
     * value if this is a soft link and the link's value is not
     * being returned through link_val_out.
     */
    H5_DAOS_LINK_VAL_TO_INFO(udata->local_link_val, *udata->link_info_out, -H5_DAOS_H5_GET_ERROR);
    if(!udata->link_val_out && (H5L_TYPE_SOFT == udata->local_link_val.type))
        udata->local_link_val.target.soft = (char *)DV_free(udata->local_link_val.target.soft);

    assert(udata->target_obj->item.type == H5I_GROUP);
    if(((H5_daos_group_t *)udata->target_obj)->gcpl_cache.track_corder) {
        /* Note that this function increments ref counts on target_obj and req,
         * and target_name points into path_buf which wont be freed until after
         * this function's task(s), so we can free udata in this function
         * without waiting */
        if(H5_daos_link_get_crt_order_by_name((H5_daos_group_t *)udata->target_obj,
                udata->target_name, (uint64_t *)&udata->link_info_out->corder /* DSINC - no check for overflow */,
                udata->req, &first_task, &dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, -H5_DAOS_H5_GET_ERROR, "can't get link's creation order value");
        udata->link_info_out->corder_valid = TRUE;
    } /* end if */
    else {
        udata->link_info_out->corder = -1;
        udata->link_info_out->corder_valid = FALSE;
    } /* end else */

    /* Only ASCII character set is supported currently */
    udata->link_info_out->cset = H5T_CSET_ASCII;

    if(udata->link_val_out)
        memcpy(udata->link_val_out, &udata->local_link_val, sizeof(*udata->link_val_out));

done:
    if(udata) {
        /* Free path_buf if necessary */
        if(udata->path_buf && H5_daos_free_async(udata->target_obj->item.file, udata->path_buf, &first_task, &dep_task) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, -H5_DAOS_FREE_ERROR, "can't free path buffer");

        /* Create metatask to complete this task after dep_task if necessary */
        if(dep_task) {
            /* Create metatask */
            if(0 != (ret = tse_task_create(H5_daos_metatask_autocomp_other, &udata->target_obj->item.file->sched, task, &metatask))) {
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create metatask for link get info end: %s", H5_daos_err_to_string(ret));
                tse_task_complete(task, ret_value);
            } /* end if */
            else {
                /* Register task dependency */
                if(0 != (ret = tse_task_register_deps(metatask, 1, &dep_task)))
                    D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link get info end metatask: %s", H5_daos_err_to_string(ret));

                /* Schedule metatask */
                assert(first_task);
                if(0 != (ret = tse_task_schedule(metatask, false)))
                    D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule metatask for link get info end: %s", H5_daos_err_to_string(ret));
            } /* end else */
        } /* end if */

        /* Schedule first task */
        if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link get info end: %s", H5_daos_err_to_string(ret));

        /* Close target_obj */
        if(H5_daos_group_close((H5_daos_group_t *)udata->target_obj, H5I_INVALID_HID, NULL) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
        udata->target_obj = NULL;

        /* Handle errors in this function */
        if(ret_value < 0) {
            /* Free soft link value since we failed */
            udata->local_link_val.target.soft = (char *)DV_free(udata->local_link_val.target.soft);

            /* Do not place any code that can issue errors after this block, except for
             * H5_daos_req_free_int, which updates req->status if it sees an error */
            if(ret_value != -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
                udata->req->status = ret_value;
                udata->req->failed_task = "link get info end task";
            } /* end if */
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Complete task if necessary */
        if(!metatask)
            tse_task_complete(task, ret_value);

        /* Free udata */
        udata = DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_get_info_end_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_info
 *
 * Purpose:     Helper routine to retrieve a link's info and populate a
 *              H5L_info2_t struct.
 *
 *              If the link_val_out parameter is non-NULL, the link's value
 *              is returned through it. If the link in question is a soft
 *              link, this means the caller is responsible for freeing the
 *              allocated link value.
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_get_info(H5_daos_item_t *item, const H5VL_loc_params_t *loc_params,
    H5L_info2_t *link_info_out, H5_daos_link_val_t *link_val_out,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_get_info_ud_t *task_udata = NULL;
    tse_task_t *end_task = NULL;
    int ret;
    int ret_value = 0;

    assert(item);
    assert(loc_params);
    assert(link_info_out);

    /* Check type of item */
    if(item->type != H5I_GROUP && item->type != H5I_FILE)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, -H5_DAOS_BAD_VALUE, "target object is not a group or file");

    /* Allocate task udata struct */
    if(NULL == (task_udata = (H5_daos_link_get_info_ud_t *)DV_calloc(sizeof(H5_daos_link_get_info_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate link get info data");
    task_udata->req = req;
    task_udata->link_info_out = link_info_out;
    task_udata->link_val_out = link_val_out;

    /* Determine the target group */
    switch (loc_params->type) {
        /* H5Lget_info */
        case H5VL_OBJECT_BY_NAME:
        {
            /* Traverse the path */
            if(NULL == (task_udata->target_obj = H5_daos_group_traverse(item, loc_params->loc_data.loc_by_name.name,
                    H5P_LINK_CREATE_DEFAULT, req, FALSE, &task_udata->path_buf, &task_udata->target_name,
                    &task_udata->target_name_len, first_task, dep_task)))
                D_GOTO_ERROR(H5E_SYM, H5E_TRAVERSE, -H5_DAOS_TRAVERSE_ERROR, "failed to traverse path");

            assert(task_udata->target_obj->item.type == H5I_GROUP);

            break;
        } /* H5VL_OBJECT_BY_NAME */

        case H5VL_OBJECT_BY_IDX:
        {
            H5VL_loc_params_t sub_loc_params;

            /* Open the group containing the target link */
            sub_loc_params.type = H5VL_OBJECT_BY_SELF;
            sub_loc_params.obj_type = item->type;
            if(NULL == (task_udata->target_obj = (H5_daos_obj_t *)H5_daos_group_open(item, &sub_loc_params,
                    loc_params->loc_data.loc_by_idx.name, loc_params->loc_data.loc_by_idx.lapl_id, req->dxpl_id, NULL)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTOPENOBJ, -H5_DAOS_H5_OPEN_ERROR, "can't open group containing target link");

            /* Retrieve the link name */
            assert(task_udata->target_obj->item.type == H5I_GROUP);
            if(H5_daos_link_get_name_by_idx_alloc((H5_daos_group_t *)task_udata->target_obj, loc_params->loc_data.loc_by_idx.idx_type,
                    loc_params->loc_data.loc_by_idx.order, (uint64_t)loc_params->loc_data.loc_by_idx.n,
                    &task_udata->target_name, &task_udata->target_name_len, &task_udata->path_buf,
                    NULL, req, first_task, dep_task) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, -H5_DAOS_H5_GET_ERROR, "can't get link name");

            break;
        } /* H5VL_OBJECT_BY_IDX */

        case H5VL_OBJECT_BY_SELF:
        case H5VL_OBJECT_BY_TOKEN:
        default:
            D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "invalid loc_params type");
    } /* end switch */

    /* Read link value */
    memset(&task_udata->local_link_val, 0, sizeof(task_udata->local_link_val));
    if(H5_daos_link_read_late_name((H5_daos_group_t *)task_udata->target_obj, &task_udata->target_name,
            &task_udata->target_name_len, req, &task_udata->local_link_val, NULL, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_READERROR, -H5_DAOS_H5_GET_ERROR, "failed to read link");

done:
    /* Clean up */
    if(task_udata) {
        /* Create task to finish this operation */
        if(0 !=  (ret = tse_task_create(H5_daos_link_get_info_end_task, &item->file->sched, task_udata, &end_task))) {
            if(task_udata->target_obj && H5_daos_object_close(task_udata->target_obj, H5I_INVALID_HID, NULL) < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");
            task_udata = DV_free(task_udata);
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for link get info end: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else {
            /* Register task dependency */
            if(*dep_task && 0 != (ret = tse_task_register_deps(end_task, 1, dep_task)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link get info end task: %s", H5_daos_err_to_string(ret));

            /* Schedule end task (or save it to be scheduled later) and give it a
             * reference to req and udata (transfer ownership of the group) */
            if(*first_task) {
                if(0 != (ret = tse_task_schedule(end_task, false)))
                    D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for link get info: %s", H5_daos_err_to_string(ret));
            } /* end if */
            else
                *first_task = end_task;
            *dep_task = end_task;
            req->rc++;
            task_udata = NULL;
        } /* end else */
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_get_info() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_val_end_task
 *
 * Purpose:     Finishes the link get val operation, setting output and
 *              freeing data.
 *
 * Return:      Success:        0
 *              Failure:        FAIL
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_get_val_end_task(tse_task_t *task)
{
    H5_daos_link_get_val_ud_t *udata = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get name by creation order task");

    /* Handle errors in previous tasks */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    if(H5L_TYPE_HARD == udata->local_link_val.type)
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "link value cannot be retrieved from a hard link");

    /* Copy value to output buffer */
    /*
     * H5Lget_val(_by_idx) specifically says that if the size of
     * the buffer given is smaller than the size of the link's
     * value, then the link's value will be truncated to 'size'
     * bytes and will not be null-terminated.
     */
    if(udata->out_buf) {
        size_t link_val_size = strlen(udata->local_link_val.target.soft) + 1;
        memcpy(udata->out_buf, udata->local_link_val.target.soft, MIN(link_val_size, udata->out_buf_size));
    } /* end if */

done:
    if(udata) {
        /* Free path_buf if necessary */
        if(udata->path_buf && H5_daos_free_async(udata->target_obj->item.file, udata->path_buf, &first_task, &dep_task) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, -H5_DAOS_FREE_ERROR, "can't free path buffer");

        /* Schedule first task */
        if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link get info end: %s", H5_daos_err_to_string(ret));

        /* Close target_obj */
        if(H5_daos_group_close((H5_daos_group_t *)udata->target_obj, H5I_INVALID_HID, NULL) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
        udata->target_obj = NULL;

        /* Free soft link value */
        if(H5L_TYPE_SOFT == udata->local_link_val.type)
            udata->local_link_val.target.soft = (char *)DV_free(udata->local_link_val.target.soft);

        /* Handle errors in this function */
        if(ret_value < 0) {
            /* Do not place any code that can issue errors after this block, except for
             * H5_daos_req_free_int, which updates req->status if it sees an error */
            if(ret_value != -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
                udata->req->status = ret_value;
                udata->req->failed_task = "link get val end task";
            } /* end if */
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Complete this task */
        tse_task_complete(task, ret_value);

        /* Free udata */
        udata = DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_get_val_end_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_val
 *
 * Purpose:     Helper routine to retrieve a symbolic or user-defined
 *              link's value. The caller must remember to free the link's
 *              value after using it.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_get_val(H5_daos_item_t *item, const H5VL_loc_params_t *loc_params,
    void *out_buf, size_t out_buf_size, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_get_val_ud_t *task_udata = NULL;
    tse_task_t *end_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(item);
    assert(loc_params);
    assert(link_val_out);
    assert(H5VL_OBJECT_BY_NAME == loc_params->type || H5VL_OBJECT_BY_IDX == loc_params->type);

    /* Allocate task udata struct */
    if(NULL == (task_udata = (H5_daos_link_get_val_ud_t *)DV_calloc(sizeof(H5_daos_link_get_val_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate link get info data");
    task_udata->req = req;
    task_udata->out_buf = out_buf;
    task_udata->out_buf_size = out_buf_size;

    /* Determine the target group */
    switch (loc_params->type) {
        /* H5Lget_val */
        case H5VL_OBJECT_BY_NAME:
        {
            /* Traverse the path */
            if(NULL == (task_udata->target_obj = H5_daos_group_traverse(item, loc_params->loc_data.loc_by_name.name,
                    H5P_LINK_CREATE_DEFAULT, req, FALSE, &task_udata->path_buf, &task_udata->target_name,
                    &task_udata->target_name_len, first_task, dep_task)))
                D_GOTO_ERROR(H5E_SYM, H5E_TRAVERSE, FAIL, "failed to traverse path");

            /* Check type of target_obj */
            if(task_udata->target_obj->item.type != H5I_GROUP)
                D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "target object is not a group");

            break;
        } /* H5VL_OBJECT_BY_NAME */

        case H5VL_OBJECT_BY_IDX:
        {
            H5VL_loc_params_t sub_loc_params;

            /* Open the group containing the target link */
            sub_loc_params.type = H5VL_OBJECT_BY_SELF;
            sub_loc_params.obj_type = item->type;
            if(NULL == (task_udata->target_obj = (H5_daos_obj_t *)H5_daos_group_open(item, &sub_loc_params,
                    loc_params->loc_data.loc_by_idx.name, loc_params->loc_data.loc_by_idx.lapl_id, req->dxpl_id, NULL)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTOPENOBJ, -H5_DAOS_H5_OPEN_ERROR, "can't open group containing target link");

            /* Retrieve the link name */
            assert(task_udata->target_obj->item.type == H5I_GROUP);
            if(H5_daos_link_get_name_by_idx_alloc((H5_daos_group_t *)task_udata->target_obj, loc_params->loc_data.loc_by_idx.idx_type,
                    loc_params->loc_data.loc_by_idx.order, (uint64_t)loc_params->loc_data.loc_by_idx.n,
                    &task_udata->target_name, &task_udata->target_name_len, &task_udata->path_buf,
                    NULL, req, first_task, dep_task) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, -H5_DAOS_H5_GET_ERROR, "can't get link name");

            break;
        } /* H5VL_OBJECT_BY_IDX */

        case H5VL_OBJECT_BY_SELF:
        case H5VL_OBJECT_BY_TOKEN:
        default:
            D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "invalid loc_params type");
    } /* end switch */

    /* Read link value */
    memset(&task_udata->local_link_val, 0, sizeof(task_udata->local_link_val));
    if(H5_daos_link_read_late_name((H5_daos_group_t *)task_udata->target_obj, &task_udata->target_name,
            &task_udata->target_name_len, req, &task_udata->local_link_val, NULL, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_READERROR, -H5_DAOS_H5_GET_ERROR, "failed to read link");

done:
    /* Clean up */
    if(task_udata) {
        /* Create task to finish this operation */
        if(0 !=  (ret = tse_task_create(H5_daos_link_get_val_end_task, &item->file->sched, task_udata, &end_task))) {
            if(task_udata->target_obj && H5_daos_object_close(task_udata->target_obj, H5I_INVALID_HID, NULL) < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");
            task_udata = DV_free(task_udata);
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for link get val end: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else {
            /* Register task dependency */
            if(*dep_task && 0 != (ret = tse_task_register_deps(end_task, 1, dep_task)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link get val end task: %s", H5_daos_err_to_string(ret));

            /* Schedule end task (or save it to be scheduled later) and give it a
             * reference to req and udata (transfer ownership of the group) */
            if(*first_task) {
                if(0 != (ret = tse_task_schedule(end_task, false)))
                    D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for link get val: %s", H5_daos_err_to_string(ret));
            } /* end if */
            else
                *first_task = end_task;
            *dep_task = end_task;
            req->rc++;
            task_udata = NULL;
        } /* end else */
    } /* end if */

    D_FUNC_LEAVE;

    D_FUNC_LEAVE;
} /* end H5_daos_link_get_val() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_exists
 *
 * Purpose:     Helper routine to determine if a link exists by the given
 *              pathname from the specified object.
 *
 * Return:      Success:        TRUE if the link exists or FALSE if it does
 *                              not exist.
 *              Failure:        FAIL
 *
 *-------------------------------------------------------------------------
 */
htri_t
H5_daos_link_exists(H5_daos_item_t *item, const char *link_path,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_obj_t *target_obj = NULL;
    char *path_buf = NULL;
    const char *target_name = NULL;
    size_t target_name_len = 0;
    daos_key_t dkey;
    daos_iod_t iod;
    int ret;
    htri_t ret_value = FALSE;

    assert(item);
    assert(link_path);

    /* Traverse the path */
    if(NULL == (target_obj = H5_daos_group_traverse(item, link_path, H5P_LINK_CREATE_DEFAULT,
            req, FALSE, &path_buf, &target_name, &target_name_len, first_task, dep_task)))
        D_GOTO_ERROR(H5E_SYM, H5E_TRAVERSE, FAIL, "can't traverse path");

    /* Check type of target_obj */
    if(target_obj->item.type != H5I_GROUP)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "target object is not a group");

    /* Wait until everything is complete then check for errors
     * (temporary code until the rest of this function is async) */
    if(*first_task && (0 != (ret = tse_task_schedule(*first_task, false))))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
    if(H5_daos_progress(&item->file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
    *first_task = NULL;
    *dep_task = NULL;
    if(req->status < -H5_DAOS_INCOMPLETE)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(req->status));

    /* Set up dkey */
    daos_iov_set(&dkey, (void *)target_name, strlen(target_name));

    /* Set up iod */
    memset(&iod, 0, sizeof(iod));
    daos_iov_set(&iod.iod_name, (void *)H5_daos_link_key_g, H5_daos_link_key_size_g);
    iod.iod_nr = 1u;
    iod.iod_size = DAOS_REC_ANY;
    iod.iod_type = DAOS_IOD_SINGLE;

    /* Read link */
    if(0 != (ret = daos_obj_fetch(target_obj->obj_oh, DAOS_TX_NONE, 0 /*flags*/, &dkey, 1, &iod, NULL /*sgl*/, NULL /*maps*/, NULL /*event*/)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't read link: %s", H5_daos_err_to_string(ret));

    /* Set return value */
    ret_value = iod.iod_size != (uint64_t)0;

done:
    /* Free path_buf if necessary */
    if(path_buf && H5_daos_free_async(item->file, path_buf, first_task, dep_task) < 0)
        D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, FAIL, "can't free path buffer");

    if(target_obj) {
        if(H5_daos_object_close(target_obj, req->dxpl_id, NULL) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close object");
        target_obj = NULL;
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_exists() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_iterate
 *
 * Purpose:     Iterates over the links in the specified group, using the
 *              supplied iter_data struct for the iteration parameters.
 *
 * Return:      Success:        SUCCEED or positive
 *              Failure:        FAIL
 *
 * Programmer:  Neil Fortner
 *              January, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_iterate(H5_daos_group_t *target_grp, H5_daos_iter_data_t *iter_data,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    herr_t ret_value = SUCCEED;

    assert(target_grp);
    assert(iter_data);
    assert(H5_DAOS_ITER_TYPE_LINK == iter_data->iter_type);

    /* Iteration restart not supported */
    if(iter_data->idx_p && (*iter_data->idx_p != 0))
        D_GOTO_ERROR(H5E_SYM, H5E_UNSUPPORTED, FAIL, "iteration restart not supported (must start from 0)");

    switch (iter_data->index_type) {
        case H5_INDEX_NAME:
            if((ret_value = H5_daos_link_iterate_by_name_order(target_grp, iter_data, first_task, dep_task)) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_BADITER, FAIL, "link iteration by name order failed");
            break;

        case H5_INDEX_CRT_ORDER:
            if((ret_value = H5_daos_link_iterate_by_crt_order(target_grp, iter_data, first_task, dep_task)) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_BADITER, FAIL, "link iteration by creation order failed");
            break;

        case H5_INDEX_UNKNOWN:
        case H5_INDEX_N:
        default:
            D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "invalid or unsupported index type");
    } /* end switch */

done:
    D_FUNC_LEAVE;
} /* end H5_daos_link_iterate() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_iterate_list_comp_cb
 *
 * Purpose:     Completion callback for dkey list for link iteration by
 *              name.  Initiates operation on each link and reissues list
 *              operation if appropriate.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              May, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_iterate_list_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_iter_ud_t *udata = NULL;
    H5_daos_link_iter_op_ud_t *iter_op_udata = NULL;
    H5VL_loc_params_t sub_loc_params;
    H5_daos_req_t *req = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for operation finalize task");

    /* Assign req convenience pointer.  We do this so we can still handle errors
     * after transfering ownership of udata.  This should be safe since the
     * iteration metatask holds a reference to req until all iteration is
     * complete at this level. */
    req = udata->iter_data->req;

    /* Check for buffer not large enough */
    if(task->dt_result == -DER_REC2BIG) {
        char *tmp_realloc = NULL;
        size_t key_buf_len = 2 * (udata->sg_iov.iov_buf_len + 1);

        /* Reallocate larger buffer */
        if(NULL == (tmp_realloc = (char *)DV_realloc(udata->sg_iov.iov_buf, key_buf_len)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't reallocate key buffer");

        /* Update sg_iov */
        daos_iov_set(&udata->sg_iov, tmp_realloc, (daos_size_t)(key_buf_len - 1));

        /* Reissue list operation */
        if(0 != (ret = H5_daos_list_key_start(udata, DAOS_OPC_OBJ_LIST_DKEY, H5_daos_link_iterate_list_comp_cb, &first_task, &dep_task)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't start iteration");
        udata = NULL;
    } /* end if */
    else {
        /* Handle errors in list task.  Only record error in req->status
         * if it does not already contain an error (it could contain an error if
         * another task this task is not dependent on also failed). */
        if(task->dt_result < -H5_DAOS_PRE_ERROR
                && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            req->status = task->dt_result;
            req->failed_task = "link iterate key list completion callback";
        } /* end if */
        else if(task->dt_result == 0) {
            uint32_t i;
            char *p = udata->sg_iov.iov_buf;
    
            /* Loop over returned dkeys */
            for(i = 0; i < udata->nr; i++) {
                /* Check if this key represents a link */
                if(p[0] != '/') {
                    char tmp_char;

                    /* Allocate iter op udata */
                    if(NULL == (iter_op_udata = (H5_daos_link_iter_op_ud_t *)DV_calloc(sizeof(H5_daos_link_iter_op_ud_t))))
                        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate iteration op user data");
                    iter_op_udata->iter_ud = udata;
                    iter_op_udata->link_path = p;
                    iter_op_udata->link_path_len = udata->kds[i].kd_key_len;

                    /* Add null terminator temporarily */
                    tmp_char = p[udata->kds[i].kd_key_len];
                    p[udata->kds[i].kd_key_len] = '\0';

                    /* Retrieve link's info and value */
                    sub_loc_params.obj_type = udata->target_obj->item.type;
                    sub_loc_params.type = H5VL_OBJECT_BY_NAME;
                    sub_loc_params.loc_data.loc_by_name.lapl_id = H5P_LINK_ACCESS_DEFAULT;
                    sub_loc_params.loc_data.loc_by_name.name = p;
                    if(0 != (ret = H5_daos_link_get_info(&udata->target_obj->item, &sub_loc_params,
                            &iter_op_udata->linfo, &iter_op_udata->link_val, req,
                            &first_task, &dep_task)))
                        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, ret, "can't get link info: %s", H5_daos_err_to_string(ret));

                    /* Create task for iter op */
                    if(0 !=  (ret = tse_task_create(H5_daos_link_iterate_op_task, &udata->target_obj->item.file->sched, iter_op_udata, &iter_op_udata->op_task)))
                        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for iteration op: %s", H5_daos_err_to_string(ret));

                    /* Register task dependency */
                    if(dep_task && 0 != (ret = tse_task_register_deps(iter_op_udata->op_task, 1, &dep_task)))
                        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for iteration op task: %s", H5_daos_err_to_string(ret));

                    /* Schedule iter op (or save it to be scheduled later) and
                     * transfer ownership of iter_op_udata */
                    if(first_task) {
                        if(0 != (ret = tse_task_schedule(iter_op_udata->op_task, false)))
                            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for iteration op: %s", H5_daos_err_to_string(ret));
                    } /* end if */
                    else
                        first_task = iter_op_udata->op_task;
                    dep_task = iter_op_udata->op_task;
                    iter_op_udata = NULL;

                    /* Replace null terminator */
                    p[udata->kds[i].kd_key_len] = tmp_char;
                } /* end if */

                /* Advance to next akey */
                p += udata->kds[i].kd_key_len + udata->kds[i].kd_csum_len;
            } /* end for */

            /* Continue iteration if we're not done */
            if(!daos_anchor_is_eof(&udata->anchor) && (req->status == -H5_DAOS_INCOMPLETE)) {
                if(0 != (ret = H5_daos_list_key_start(udata, DAOS_OPC_OBJ_LIST_DKEY, H5_daos_link_iterate_list_comp_cb, &first_task, &dep_task)))
                    D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't start iteration");
                udata = NULL;
            } /* end if */
        } /* end if */
    } /* end else */

done:
    /* If we still own udata then iteration is complete.  Register dependency
     * for metatask and schedule it. */
    if(udata) {
        if(dep_task && 0 != (ret = tse_task_register_deps(udata->iter_metatask, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for iteration metatask: %s", H5_daos_err_to_string(ret));

        if(first_task) {
            if(0 != (ret = tse_task_schedule(udata->iter_metatask, false)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule iteration metatask: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else
            first_task = udata->iter_metatask;
        udata = NULL;
    } /* end if */

    /* Schedule first task */
    if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
        D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link iteration dkey list comp cb: %s", H5_daos_err_to_string(ret));

    /* Clean up on error */
    if(ret_value < 0) {
        /* Free iter_op_udata */
        iter_op_udata = DV_free(iter_op_udata);

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value != -H5_DAOS_SHORT_CIRCUIT && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            req->status = ret_value;
            req->failed_task = "link iteration dkey list completion callback";
        } /* end if */
    } /* end if */

    /* Make sure we cleaned up */
    assert(!udata);
    assert(!iter_op_udata);

    D_FUNC_LEAVE;
} /* end H5_daos_link_iterate_list_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_iterate_op_task
 *
 * Purpose:     Perform operation on a link during iteration.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              May, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_iterate_op_task(tse_task_t *task)
{
    H5_daos_link_iter_op_ud_t *udata = NULL;
    H5VL_loc_params_t sub_loc_params;
    H5_daos_group_t *subgroup = NULL;
    H5_daos_req_t *req = NULL;
    char *link_path;
    char link_path_replace_char = '\0';
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for iteration operation task");

    /* Assign req convenience pointer.  We do this so we can still handle errors
     * after transfering ownership of udata.  This should be safe since the
     * iteration metatask holds a reference to req until all iteration is
     * complete at this level. */
    req = udata->iter_ud->iter_data->req;

    /* Handle errors in previous tasks */
    if(req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Free soft link value if necessary */
    if(H5L_TYPE_SOFT == udata->link_val.type)
        udata->link_val.target.soft = (char *)DV_free(udata->link_val.target.soft);

    /* If doing recursive iteration, add the current link name to the end of the recursive link path */
    if(udata->iter_ud->iter_data->is_recursive) {
        size_t cur_link_path_len = strlen(udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path);

        /*
         * Save a pointer to the current end of the recursive link path string. This will
         * be used later to strip the added link name back off of the path string once
         * processing is done.
         */
        udata->char_replace_loc = &udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len];
        udata->char_replace_char = '\0';

        /*
         * Similarly, add a tmeporary null terminator to udata->link_path
         */
        link_path_replace_char = udata->link_path[udata->link_path_len];
        udata->link_path[udata->link_path_len] = '\0';

        /*
         * Reallocate the link path buffer if the current link path + the current
         * link name and null terminator is larger than what's currently allocated.
         */
        while(cur_link_path_len + udata->link_path_len + 1 > udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path_nalloc) {
            char *tmp_realloc;

            udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path_nalloc *= 2;
            if(NULL == (tmp_realloc = DV_realloc(udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path,
                    udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path_nalloc)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't reallocate link path buffer");

            udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path = tmp_realloc;
        } /* end if */

        /* Append the current link name to the current link path */
        memcpy(&udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len],
                udata->link_path, udata->link_path_len + 1);
        udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len + udata->link_path_len] = '\0';

        link_path = udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path;
    } /* end if */
    else {
        /* Non-recursive, just use udata->link_path after adding null terminator
         * (and saving info to replace null terminator with original char) */
        link_path = udata->link_path;
        udata->char_replace_loc = &link_path[udata->link_path_len];
        udata->char_replace_char = *udata->char_replace_loc;
        *udata->char_replace_loc = '\0';
    } /* end else */

    /* Call the link iteration callback operator function on the current link */
    if(udata->iter_ud->iter_data->async_op) {
        if(udata->iter_ud->iter_data->u.link_iter_data.u.link_iter_op_async(
                udata->iter_ud->iter_data->iter_root_obj, link_path,
                &udata->linfo, udata->iter_ud->iter_data->op_data,
                &udata->iter_ud->iter_data->op_ret, &first_task, &dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_BADITER, -H5_DAOS_CALLBACK_ERROR, "operator function returned failure");
    } /* end if */
    else
        udata->iter_ud->iter_data->op_ret = udata->iter_ud->iter_data->u.link_iter_data.u.link_iter_op(
                udata->iter_ud->iter_data->iter_root_obj, link_path,
                &udata->linfo, udata->iter_ud->iter_data->op_data);

    /* Check for failure from operator return */
    if(udata->iter_ud->iter_data->op_ret < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_BADITER, -H5_DAOS_CALLBACK_ERROR, "operator function returned failure");

    /* Check for short-circuit success */
    if(udata->iter_ud->iter_data->op_ret) {
        udata->iter_ud->iter_data->req->status = -H5_DAOS_SHORT_CIRCUIT;
        udata->iter_ud->iter_data->short_circuit_init = TRUE;

        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    if(udata->iter_ud->iter_data->is_recursive) {
        assert(udata->iter_ud->iter_data->u.link_iter_data.visited_link_table);

        /* If the current link points to a group that hasn't been visited yet, iterate over its links as well. */
        if((H5L_TYPE_HARD == udata->link_val.type) && (H5I_GROUP == H5_daos_oid_to_type(udata->link_val.target.hard))
                && (DV_HASH_TABLE_NULL == dv_hash_table_lookup(udata->iter_ud->iter_data->u.link_iter_data.visited_link_table, &udata->link_val.target.hard.lo))) {
            uint64_t *oid_lo_copy;
            size_t cur_link_path_len;

            if(NULL == (oid_lo_copy = DV_malloc(sizeof(*oid_lo_copy))))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "failed to allocate entry for visited link table");
            *oid_lo_copy = udata->link_val.target.hard.lo;

            /*
             * The value chosen for the hash table entry doesn't really matter, as long
             * as it doesn't match DV_HASH_TABLE_NULL. Later, it only needs to be known
             * if we inserted the key into the table or not, so the value will not be checked.
             */
            ret = dv_hash_table_insert(udata->iter_ud->iter_data->u.link_iter_data.visited_link_table, oid_lo_copy, oid_lo_copy);
            if(!ret) {
                DV_free(oid_lo_copy);
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINSERT, ret, "failed to insert link into visited link table");
            } /* end if */

            sub_loc_params.type = H5VL_OBJECT_BY_SELF;
            sub_loc_params.obj_type = H5I_GROUP;
            if(NULL == (subgroup = H5_daos_group_open_int(&udata->iter_ud->target_obj->item, &sub_loc_params,
                    udata->link_path, H5P_GROUP_ACCESS_DEFAULT, req, FALSE, &first_task, &dep_task)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTOPENOBJ, -H5_DAOS_H5_OPEN_ERROR, "failed to open group");

            /* Add a trailing slash to the link path buffer to denote that this link points to a group */
            cur_link_path_len = strlen(udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path);
            while(cur_link_path_len + 2 > udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path_nalloc) {
                char *tmp_realloc;

                udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path_nalloc *= 2;
                if(NULL == (tmp_realloc = DV_realloc(udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path,
                        udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path_nalloc)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "failed to reallocate link path buffer");

                udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path = tmp_realloc;
            } /* end while */

            udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len] = '/';
            udata->iter_ud->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len + 1] = '\0';

            /* Recurse on this group */
            if(0 != (ret = H5_daos_list_key_init(udata->iter_ud->iter_data, &subgroup->obj,
                    NULL, DAOS_OPC_OBJ_LIST_DKEY, H5_daos_link_iterate_list_comp_cb, FALSE,
                    &first_task, &dep_task)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't start link iteration: %s", H5_daos_err_to_string(ret));

            if(H5_daos_group_close(subgroup, req->dxpl_id, NULL) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
            subgroup = NULL;
        } /* end if */

        /* Reset udata->link_path to state before adding null terminator */
        udata->link_path[udata->link_path_len] = link_path_replace_char;
    } /* end if */

done:
    /* Check for tasks scheduled, in this case we need to schedule a task to
     * mark this task complete and free udata */
    if(dep_task) {
        tse_task_t *op_end_task;

        assert(udata);

        /* Schedule task to complete this task and free udata */
        if(0 != (ret = tse_task_create(H5_daos_link_iter_op_end, &req->file->sched, udata, &op_end_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task to finish iteration op: %s", H5_daos_err_to_string(ret));
        else {
            /* Register dependency for task */
            if(0 != (ret = tse_task_register_deps(op_end_task, 1, &dep_task)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for iteration op end task: %s", H5_daos_err_to_string(ret));

            /* Schedule link iter op end task and give it ownership of udata */
            assert(first_task);
            if(0 != (ret = tse_task_schedule(op_end_task, false)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task to finish link iteration: %s", H5_daos_err_to_string(ret));
            udata = NULL;
            dep_task = op_end_task;
        } /* end else */

        /* Schedule first task */
        assert(first_task);
        if(0 != (ret = tse_task_schedule(first_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link iteration op: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        assert(!first_task);

    /* Handle errors */
    if(ret_value < 0 && req) {
        if(subgroup) {
            assert(req);
            if(H5_daos_group_close(subgroup, req->dxpl_id, NULL) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
            subgroup = NULL;
        } /* end if */

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value != -H5_DAOS_SHORT_CIRCUIT && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            req->status = ret_value;
            req->failed_task = "link iteration op";
        } /* end if */
    } /* end if */

    /* Complete task and free udata if we still own udata */
    if(udata) {
        /* Replace char */
        if(udata->char_replace_loc)
            *udata->char_replace_loc = udata->char_replace_char;

        /* Complete this task */
        tse_task_complete(task, ret_value);

        /* Free private data */
        udata = DV_free(udata);
    } /* end if */
    else
        assert(ret_value >= 0 || ret_value == -H5_DAOS_DAOS_GET_ERROR);

    /* Make sure we cleaned up */
    assert(!udata);
    assert(!subgroup);

    D_FUNC_LEAVE;
} /* end H5_daos_link_iterate_op_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_iter_op_end
 *
 * Purpose:     Completes the link iteration op and frees its udata.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              June, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_iter_op_end(tse_task_t *task)
{
    H5_daos_link_iter_op_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for task");

    /* Replace char */
    if(udata->char_replace_loc)
        *udata->char_replace_loc = udata->char_replace_char;

    /* Check if we need to update the request status due to an operator return
     */
    if(udata->iter_ud->iter_data->async_op
            && udata->iter_ud->iter_data->req->status >= -H5_DAOS_INCOMPLETE) {
        /* Check for failure from operator return */
        if(udata->iter_ud->iter_data->op_ret < 0) {
            udata->iter_ud->iter_data->req->status = -H5_DAOS_CALLBACK_ERROR;
            udata->iter_ud->iter_data->req->failed_task = "link iteration callback operator function";
            D_DONE_ERROR(H5E_LINK, H5E_BADITER, -H5_DAOS_CALLBACK_ERROR, "operator function returned failure");
        } /* end if */
        else if(udata->iter_ud->iter_data->op_ret) {
            /* Short-circuit success */
            udata->iter_ud->iter_data->req->status = -H5_DAOS_SHORT_CIRCUIT;
            udata->iter_ud->iter_data->short_circuit_init = TRUE;
        } /* end if */
    } /* end if */

    /* Complete op task */
    tse_task_complete(udata->op_task, 0);

    /* Free udata */
    DV_free(udata);

done:
    /* Complete this task */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_link_iter_op_end() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_iterate_by_name_order
 *
 * Purpose:     Iterates over the links in the specified group according to
 *              their alphabetical order. The supplied iter_data struct
 *              contains the iteration parameters.
 *
 * Return:      Success:        SUCCEED or positive
 *              Failure:        FAIL
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_iterate_by_name_order(H5_daos_group_t *target_grp, H5_daos_iter_data_t *iter_data,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    int ret;
    herr_t ret_value = SUCCEED;

    assert(target_grp);
    assert(iter_data);
    assert(H5_INDEX_NAME == iter_data->index_type);
    assert(H5_ITER_NATIVE == iter_data->iter_order || H5_ITER_INC == iter_data->iter_order
            || H5_ITER_DEC == iter_data->iter_order);

    /* Native iteration order is currently associated with increasing order; decreasing order iteration is not currently supported */
    if(iter_data->iter_order == H5_ITER_DEC)
        D_GOTO_ERROR(H5E_LINK, H5E_UNSUPPORTED, FAIL, "decreasing iteration order not supported (order must be H5_ITER_NATIVE or H5_ITER_INC)");

    /* Increment reference count on root obj id */
    if(H5Iinc_ref(iter_data->iter_root_obj) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINC, FAIL, "can't increment reference count on iteration base object");

    /*
     * If iteration is recursive, setup a hash table to keep track of visited
     * group links so that cyclic links don't result in infinite looping.
     *
     * Also setup the recursive link path buffer, which keeps track of the full
     * path to the current link and is passed to the operator callback function.
     */
    if(iter_data->is_recursive && (iter_data->u.link_iter_data.recurse_depth == 0)) {
        if(NULL == (iter_data->u.link_iter_data.visited_link_table = dv_hash_table_new(H5_daos_hash_obj_id, H5_daos_cmp_obj_id)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate visited links table");

        /*
         * Since the table key values are DV_malloc()ed, register the callback function to
         * call DV_free() on the keys when necessary.
         */
        dv_hash_table_register_free_functions(iter_data->u.link_iter_data.visited_link_table, H5_daos_free_visited_link_hash_table_key, NULL);

        /* Allocate the link path buffer for recursive iteration */
        if(NULL == (iter_data->u.link_iter_data.recursive_link_path = DV_malloc(H5_DAOS_RECURSE_LINK_PATH_BUF_INIT)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate link path buffer");
        iter_data->u.link_iter_data.recursive_link_path_nalloc = H5_DAOS_RECURSE_LINK_PATH_BUF_INIT;
        *iter_data->u.link_iter_data.recursive_link_path = '\0';
    } /* end if */

    /* Start iteration */
    if(0 != (ret = H5_daos_list_key_init(iter_data, &target_grp->obj,
            NULL, DAOS_OPC_OBJ_LIST_DKEY, H5_daos_link_iterate_list_comp_cb, TRUE,
            first_task, dep_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't start link iteration: %s", H5_daos_err_to_string(ret));

done:
    /* Cleanup on error */
    if(ret_value < 0) {
        iter_data->u.link_iter_data.recursive_link_path = DV_free(iter_data->u.link_iter_data.recursive_link_path);

        if(iter_data->u.link_iter_data.visited_link_table) {
            dv_hash_table_free(iter_data->u.link_iter_data.visited_link_table);
            iter_data->u.link_iter_data.visited_link_table = NULL;
        } /* end if */
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_iterate_by_name_order() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_ibco_end_task
 *
 * Purpose:     Finishes iteration at a single level of recursion for
 *              H5_daos_link_iterate_by_crt_order().  Performs some
 *              bookkeeping and frees data.
 *
 * Return:      Success:        0
 *              Failure:        Negative error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_ibco_end_task(tse_task_t *task)
{
    H5_daos_link_ibco_ud_t *udata = NULL;
    H5_daos_req_t *req = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for iteration operation task");

    assert(task == udata->ibco_metatask);

    /* Assign req convenience pointer.  We do this so we can still handle errors
     * after freeing.  This should be safe since we don't decrease the ref count
     * on req until we're done with it. */
    req = udata->iter_data->req;

    assert(req);
    assert(req->file);

    /* Replace char */
    if(udata->null_replace_loc)
        *udata->null_replace_loc = '\0';

    /* Check if we need to update the request status due to an operator return
     */
    if(udata->iter_data->async_op
            && udata->iter_data->req->status >= -H5_DAOS_INCOMPLETE) {
        /* Check for failure from operator return */
        if(udata->iter_data->op_ret < 0) {
            udata->iter_data->req->status = -H5_DAOS_CALLBACK_ERROR;
            udata->iter_data->req->failed_task = "link iteration callback operator function";
            D_DONE_ERROR(H5E_LINK, H5E_BADITER, -H5_DAOS_CALLBACK_ERROR, "operator function returned failure");
        } /* end if */
        else if(udata->iter_data->op_ret) {
            /* Short-circuit success */
            udata->iter_data->req->status = -H5_DAOS_SHORT_CIRCUIT;
            udata->iter_data->short_circuit_init = TRUE;
        } /* end if */
    } /* end if */

    /* Finalize iter_data if this is the base of iteration */
    if(udata->base_iter) {
        /* Iteration is complete, we are no longer short-circuiting (if this
         * iteration caused the short circuit) */
        if(udata->iter_data->short_circuit_init) {
            if(udata->iter_data->req->status == -H5_DAOS_SHORT_CIRCUIT)
                udata->iter_data->req->status = -H5_DAOS_INCOMPLETE;
            udata->iter_data->short_circuit_init = FALSE;
        } /* end if */

        /* Decrement reference count on root obj id */
        if(H5Idec_ref(udata->iter_data->iter_root_obj) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTDEC, -H5_DAOS_H5_CLOSE_ERROR, "can't decrement reference count on iteration base object");
        udata->iter_data->iter_root_obj = H5I_INVALID_HID;

        /* Set *op_ret_p if present */
        if(udata->iter_data->op_ret_p)
            *udata->iter_data->op_ret_p = udata->iter_data->op_ret;

        /* Free hash table */
        udata->iter_data->u.link_iter_data.recursive_link_path = DV_free(udata->iter_data->u.link_iter_data.recursive_link_path);

        if(udata->iter_data->u.link_iter_data.visited_link_table) {
            dv_hash_table_free(udata->iter_data->u.link_iter_data.visited_link_table);
            udata->iter_data->u.link_iter_data.visited_link_table = NULL;
        } /* end if */

        /* Free iter data */
        udata->iter_data = DV_free(udata->iter_data);
    } /* end if */
    else
        assert(udata->iter_data->is_recursive);

    /* Close target_grp */
    if(H5_daos_group_close(udata->target_grp, H5I_INVALID_HID, NULL) < 0)
        D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");

    /* Free name buffer */
    udata->name_buf = DV_free(udata->name_buf);

    /* Free udata */
    udata = DV_free(udata);

    /* Handle errors */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        req->status = ret_value;
        req->failed_task = "link iterate by creation order finish";
    } /* end if */

    /* Release req */
    if(H5_daos_req_free_int(req) < 0)
        D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

done:
    /* Mark task as complete */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_link_ibco_end_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_ibco_op_task
 *
 * Purpose:     Perform operation on a link during iteration by creation
 *              order.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              June, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_ibco_op_task(tse_task_t *task)
{
    H5_daos_link_ibco_ud_t *udata = NULL;
    H5VL_loc_params_t sub_loc_params;
    H5_daos_group_t *subgroup = NULL;
    H5_daos_req_t *req = NULL;
    const char *link_path;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for iteration operation task");

    /* Assign req convenience pointer and take a refernce to it */
    req = udata->iter_data->req;
    req->rc++;

    /* Free soft link value if necessary */
    if(H5L_TYPE_SOFT == udata->link_val.type)
        udata->link_val.target.soft = (char *)DV_free(udata->link_val.target.soft);

    /* Handle errors in previous tasks */
    if(req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* If doing recursive iteration, add the current link name to the end of the recursive link path */
    if(udata->iter_data->is_recursive) {
        size_t cur_link_path_len = strlen(udata->iter_data->u.link_iter_data.recursive_link_path);

        /*
         * Save a pointer to the current end of the recursive link path string. This will
         * be used later to strip the added link name back off of the path string once
         * processing is done.
         */
        udata->null_replace_loc = &udata->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len];

        /*
         * Reallocate the link path buffer if the current link path + the current
         * link name and null terminator is larger than what's currently allocated.
         */
        while(cur_link_path_len + udata->link_name_len + 1 > udata->iter_data->u.link_iter_data.recursive_link_path_nalloc) {
            char *tmp_realloc;

            udata->iter_data->u.link_iter_data.recursive_link_path_nalloc *= 2;
            if(NULL == (tmp_realloc = DV_realloc(udata->iter_data->u.link_iter_data.recursive_link_path,
                    udata->iter_data->u.link_iter_data.recursive_link_path_nalloc)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't reallocate link path buffer");

            udata->iter_data->u.link_iter_data.recursive_link_path = tmp_realloc;
        } /* end if */

        /* Append the current link name to the current link path */
        memcpy(&udata->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len],
                udata->link_name, udata->link_name_len + 1);
        udata->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len + udata->link_name_len] = '\0';

        link_path = udata->iter_data->u.link_iter_data.recursive_link_path;
    } /* end if */
    else
        /* Non-recursive, just use udata->link_name */
        link_path = udata->link_name;

    /* Call the link iteration callback operator function on the current link */
    if(udata->iter_data->async_op) {
        if(udata->iter_data->u.link_iter_data.u.link_iter_op_async(
                udata->iter_data->iter_root_obj, link_path,
                &udata->linfo, udata->iter_data->op_data,
                &udata->iter_data->op_ret, &first_task, &dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_BADITER, -H5_DAOS_CALLBACK_ERROR, "operator function returned failure");
    } /* end if */
    else
        udata->iter_data->op_ret = udata->iter_data->u.link_iter_data.u.link_iter_op(
                udata->iter_data->iter_root_obj, link_path,
                &udata->linfo, udata->iter_data->op_data);

    /* Check for failure from operator return */
    if(udata->iter_data->op_ret < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_BADITER, -H5_DAOS_CALLBACK_ERROR, "operator function returned failure");

    /* Check for short-circuit success */
    if(udata->iter_data->op_ret) {
        udata->iter_data->req->status = -H5_DAOS_SHORT_CIRCUIT;
        udata->iter_data->short_circuit_init = TRUE;

        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    if(udata->iter_data->is_recursive) {
        assert(udata->iter_data->u.link_iter_data.visited_link_table);

        /* If the current link points to a group that hasn't been visited yet, iterate over its links as well. */
        if((H5L_TYPE_HARD == udata->link_val.type) && (H5I_GROUP == H5_daos_oid_to_type(udata->link_val.target.hard))
                && (DV_HASH_TABLE_NULL == dv_hash_table_lookup(udata->iter_data->u.link_iter_data.visited_link_table, &udata->link_val.target.hard.lo))) {
            uint64_t *oid_lo_copy;
            size_t cur_link_path_len;

            if(NULL == (oid_lo_copy = DV_malloc(sizeof(*oid_lo_copy))))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "failed to allocate entry for visited link table");
            *oid_lo_copy = udata->link_val.target.hard.lo;

            /*
             * The value chosen for the hash table entry doesn't really matter, as long
             * as it doesn't match DV_HASH_TABLE_NULL. Later, it only needs to be known
             * if we inserted the key into the table or not, so the value will not be checked.
             */
            ret = dv_hash_table_insert(udata->iter_data->u.link_iter_data.visited_link_table, oid_lo_copy, oid_lo_copy);
            if(!ret) {
                DV_free(oid_lo_copy);
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINSERT, ret, "failed to insert link into visited link table");
            } /* end if */

            sub_loc_params.type = H5VL_OBJECT_BY_SELF;
            sub_loc_params.obj_type = H5I_GROUP;
            if(NULL == (subgroup = H5_daos_group_open_int(&udata->target_grp->obj.item, &sub_loc_params,
                    udata->link_name, H5P_GROUP_ACCESS_DEFAULT, req, FALSE, &first_task, &dep_task)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTOPENOBJ, -H5_DAOS_H5_OPEN_ERROR, "failed to open group");

            /* Add a trailing slash to the link path buffer to denote that this link points to a group */
            cur_link_path_len = strlen(udata->iter_data->u.link_iter_data.recursive_link_path);
            while(cur_link_path_len + 2 > udata->iter_data->u.link_iter_data.recursive_link_path_nalloc) {
                char *tmp_realloc;

                udata->iter_data->u.link_iter_data.recursive_link_path_nalloc *= 2;
                if(NULL == (tmp_realloc = DV_realloc(udata->iter_data->u.link_iter_data.recursive_link_path,
                        udata->iter_data->u.link_iter_data.recursive_link_path_nalloc)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "failed to reallocate link path buffer");

                udata->iter_data->u.link_iter_data.recursive_link_path = tmp_realloc;
            } /* end while */

            udata->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len] = '/';
            udata->iter_data->u.link_iter_data.recursive_link_path[cur_link_path_len + 1] = '\0';

            /* Recurse on this group */
            if(0 != (ret = H5_daos_link_ibco_helper(subgroup, udata->iter_data,
                    FALSE, &first_task, &dep_task)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't start link iteration: %s", H5_daos_err_to_string(ret));

            if(H5_daos_group_close(subgroup, req->dxpl_id, NULL) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
            subgroup = NULL;
        } /* end if */
    } /* end if */

    /* Check if we should schedule another iteration */
    if(++udata->crt_idx < udata->grp_nlinks) {
        tse_task_t *next_task = NULL;

        /* Create task to continue this operation */
        if(0 !=  (ret = tse_task_create(H5_daos_link_ibco_task, &req->file->sched, udata, &next_task)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for link iterate by creation order: %s", H5_daos_err_to_string(ret));

        /* Register task dependency */
        if(dep_task && 0 != (ret = tse_task_register_deps(next_task, 1, &dep_task)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link iterate by creation order task: %s", H5_daos_err_to_string(ret));

        /* Schedule ibco task (or save it to be scheduled later) and transfer
         * ownershiop of udata */
        if(first_task) {
            if(0 != (ret = tse_task_schedule(next_task, false)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for link iterate by creation order: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else
            first_task = next_task;
        dep_task = next_task;
        udata = NULL;
    } /* end if */

done:
    if(udata) {
        /* If we still own udata then the iteration is complete, schedule
         * metatask */
        /* Register task dependency */
        if(dep_task && 0 != (ret = tse_task_register_deps(udata->ibco_metatask, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link iterate by creation order metatask: %s", H5_daos_err_to_string(ret));

        /* Schedule metatsk (or save it to be scheduled later) and transfer
         * ownershiop of udata */
        if(first_task) {
            if(0 != (ret = tse_task_schedule(udata->ibco_metatask, false)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule metatask for link iterate by creation order: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else
            first_task = udata->ibco_metatask;
        dep_task = udata->ibco_metatask;
        udata = NULL;
    } /* end if */
    else
        assert(ret_value >= 0 || ret_value == -H5_DAOS_DAOS_GET_ERROR);

    /* Schedule first task */
    if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
        D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link iteration op: %s", H5_daos_err_to_string(ret));

    /* Handle errors */
    if(ret_value < 0 && req) {
        if(subgroup) {
            if(H5_daos_group_close(subgroup, req->dxpl_id, NULL) < 0)
                D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
            subgroup = NULL;
        } /* end if */

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value != -H5_DAOS_SHORT_CIRCUIT && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            req->status = ret_value;
            req->failed_task = "link iteration op";
        } /* end if */
    } /* end if */

    /* Release req */
    if(req && H5_daos_req_free_int(req) < 0)
        D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Complete this task */
    tse_task_complete(task, ret_value);

    /* Make sure we cleaned up */
    assert(!udata);
    assert(!subgroup);

    D_FUNC_LEAVE;
} /* end H5_daos_link_ibco_op_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_ibco_task2
 *
 * Purpose:     Second asynchronous task routine for
 *              H5_daos_link_iterate_by_crt_order().  Starts a task to get
 *              the link value and info then queues up the operator task).
 *
 * Return:      Success:        0
 *              Failure:        Negative error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_ibco_task2(tse_task_t *task)
{
    H5_daos_link_ibco_ud_t *udata = NULL;
    H5VL_loc_params_t sub_loc_params;
    H5_daos_req_t *req = NULL;
    tse_task_t *op_task = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link iterate by creation order task 2");

    /* Assign req convenience pointer and take a refernce to it */
    req = udata->iter_data->req;
    req->rc++;

    /* Handle errors in previous tasks */
    if(req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Retrieve link's info and value */
    sub_loc_params.obj_type = H5I_GROUP;
    sub_loc_params.type = H5VL_OBJECT_BY_NAME;
    sub_loc_params.loc_data.loc_by_name.lapl_id = H5P_LINK_ACCESS_DEFAULT;
    sub_loc_params.loc_data.loc_by_name.name = udata->link_name;
    if(0 != (ret = H5_daos_link_get_info(&udata->target_grp->obj.item, &sub_loc_params,
            &udata->linfo, &udata->link_val, req, &first_task, &dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, ret, "can't get link info: %s", H5_daos_err_to_string(ret));

    /* Create task for iter op */
    if(0 !=  (ret = tse_task_create(H5_daos_link_ibco_op_task, &udata->target_grp->obj.item.file->sched, udata, &op_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for iteration op: %s", H5_daos_err_to_string(ret));

    /* Register task dependency */
    if(dep_task && 0 != (ret = tse_task_register_deps(op_task, 1, &dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for iteration op task: %s", H5_daos_err_to_string(ret));

    /* Schedule iter op (or save it to be scheduled later) and
     * transfer ownership of udata */
    if(first_task) {
        if(0 != (ret = tse_task_schedule(op_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for iteration op: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        first_task = op_task;
    dep_task = op_task;
    udata = NULL;

done:
    if(udata) {
        /* If we still own udata then the iteration is complete, schedule
         * metatask */
        /* Register task dependency */
        if(dep_task && 0 != (ret = tse_task_register_deps(udata->ibco_metatask, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link iterate by creation order metatask: %s", H5_daos_err_to_string(ret));

        /* Schedule metatsk (or save it to be scheduled later) and transfer
         * ownershiop of udata */
        if(first_task) {
            if(0 != (ret = tse_task_schedule(udata->ibco_metatask, false)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule metatask for link iterate by creation order: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else
            first_task = udata->ibco_metatask;
        dep_task = udata->ibco_metatask;
        udata = NULL;
    } /* end if */

    /* Schedule first task */
    if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
        D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link iteration op: %s", H5_daos_err_to_string(ret));

    /* Handle errors in this function */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && req && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        req->status = ret_value;
        req->failed_task = "link iteration by creation order task 2";
    } /* end if */

    /* Release req */
    if(req && H5_daos_req_free_int(req) < 0)
        D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Complete this task */
    tse_task_complete(task, ret_value);

    /* Make sure we cleaned up */
    assert(!udata);

    D_FUNC_LEAVE;
} /* end H5_daos_link_ibco_task2() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_ibco_task
 *
 * Purpose:     First asynchronous task routine for
 *              H5_daos_link_iterate_by_crt_order().  Performs some
 *              bookkeeping, starts a task to get the link name, then
 *              queues up the next task for this operation (task 2).
 *
 * Return:      Success:        0
 *              Failure:        Negative error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_ibco_task(tse_task_t *task)
{
    H5_daos_link_ibco_ud_t *udata = NULL;
    H5_daos_req_t *req = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link iterate by creation order task");

    /* Assign req convenience pointer and take a refernce to it */
    req = udata->iter_data->req;
    req->rc++;

    /* Handle errors in previous tasks */
    if(req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Check if we need to update the request status due to an operator return
     */
    if(udata->iter_data->async_op
            && req->status >= -H5_DAOS_INCOMPLETE) {
        /* Check for failure from operator return */
        if(udata->iter_data->op_ret < 0) {
            req->status = -H5_DAOS_CALLBACK_ERROR;
            req->failed_task = "link iteration callback operator function";
            D_DONE_ERROR(H5E_LINK, H5E_BADITER, -H5_DAOS_CALLBACK_ERROR, "operator function returned failure");
        } /* end if */
        else if(udata->iter_data->op_ret) {
            /* Short-circuit success */
            req->status = -H5_DAOS_SHORT_CIRCUIT;
            udata->iter_data->short_circuit_init = TRUE;
            D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
        } /* end if */
    } /* end if */

    /* Replace null character if requested */
    if(udata->null_replace_loc)
        *udata->null_replace_loc = '\0';

    /* Check that creation order is tracked for target group */
    if(!udata->target_grp->gcpl_cache.track_corder) {
        assert(udata->crt_idx == 0);

        if(udata->iter_data->is_recursive) {
            /*
             * For calls to H5Lvisit ONLY, the index type setting is a "best effort"
             * setting, meaning that we fall back to name order if link creation order
             * is not tracked for the target group.
             */
            /* Initiate iteration by name order.  No need to change the
             * index_type field in iter_data since the internal functions for
             * iteration by name order don't check this field */
            if(0 != (ret = H5_daos_list_key_init(udata->iter_data, &udata->target_grp->obj,
                    NULL, DAOS_OPC_OBJ_LIST_DKEY, H5_daos_link_iterate_list_comp_cb, TRUE,
                    &first_task, &dep_task)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't fall back to iteration by name order: %s", H5_daos_err_to_string(ret));

            /* Schedule metatask to clean up */
            /* Register task dependency */
            if(dep_task && 0 != (ret = tse_task_register_deps(udata->ibco_metatask, 1, &dep_task)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link iterate by creation order metatask: %s", H5_daos_err_to_string(ret));

            /* Schedule metatsk (or save it to be scheduled later) and transfer
             * ownershiop of udata */
            if(first_task) {
                if(0 != (ret = tse_task_schedule(udata->ibco_metatask, false)))
                    D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule metatask for link iterate by creation order: %s", H5_daos_err_to_string(ret));
            } /* end if */
            else
                first_task = udata->ibco_metatask;
            dep_task = udata->ibco_metatask;
            udata = NULL;

            D_GOTO_DONE(SUCCEED);
        } /* end if */
        else
            D_GOTO_ERROR(H5E_SYM, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "creation order is not tracked for group");
    } /* end if */

    /* Make sure this index is within the bounds */
    if(udata->crt_idx < udata->grp_nlinks) {
        tse_task_t *task2_task = NULL;

        /* Get link name */
        if(H5_daos_link_get_name_by_idx_alloc(udata->target_grp, H5_INDEX_CRT_ORDER,
                udata->iter_data->iter_order, (uint64_t)udata->crt_idx,
                &udata->link_name, &udata->link_name_len, &udata->name_buf,
                &udata->name_buf_size, udata->iter_data->req, &first_task, &dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, -H5_DAOS_H5_GET_ERROR, "can't get link name");

        /* Create task to continue this operation */
        if(0 !=  (ret = tse_task_create(H5_daos_link_ibco_task2, &req->file->sched, udata, &task2_task)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task 2 for link iterate by creation order: %s", H5_daos_err_to_string(ret));

        /* Register task dependency */
        if(dep_task && 0 != (ret = tse_task_register_deps(task2_task, 1, &dep_task)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link iterate by creation order task 2: %s", H5_daos_err_to_string(ret));

        /* Schedule ibco task 2 (or save it to be scheduled later) and give it a
         * reference to req and udata */
        if(first_task) {
            if(0 != (ret = tse_task_schedule(task2_task, false)))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task 2 for link get name by name order: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else
            first_task = task2_task;
        dep_task = task2_task;
        udata = NULL;
    } /* end if */
    else
        assert(udata->grp_nlinks == 0);

done:
    if(udata) {
        /* If we still own udata then the iteration is complete, schedule
         * metatask */
        /* Register task dependency */
        if(dep_task && 0 != (ret = tse_task_register_deps(udata->ibco_metatask, 1, &dep_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link iterate by creation order metatask: %s", H5_daos_err_to_string(ret));

        /* Schedule metatsk (or save it to be scheduled later) and transfer
         * ownershiop of udata */
        if(first_task) {
            if(0 != (ret = tse_task_schedule(udata->ibco_metatask, false)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule metatask for link iterate by creation order: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else
            first_task = udata->ibco_metatask;
        dep_task = udata->ibco_metatask;
        udata = NULL;
    } /* end if */

    /* Schedule first task */
    if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
        D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link iteration op: %s", H5_daos_err_to_string(ret));

    /* Handle errors in this function */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && req && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        req->status = ret_value;
        req->failed_task = "link iteration by creation order task";
    } /* end if */

    /* Release req */
    if(req && H5_daos_req_free_int(req) < 0)
        D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Complete this task */
    tse_task_complete(task, ret_value);

    /* Make sure we cleaned up */
    assert(!udata);

    D_FUNC_LEAVE;
} /* end H5_daos_link_ibco_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_ibco_helper
 *
 * Purpose:     Helper routine for H5_daos_link_iterate_by_crt_order().
 *              Starts iteration at a single level of recursion.
 *
 * Return:      Success:        0
 *              Failure:        Negative error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_ibco_helper(H5_daos_group_t *target_grp,
    H5_daos_iter_data_t *iter_data, hbool_t base_iter,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_ibco_ud_t *ibco_udata = NULL;
    tse_task_t *ibco_task;
    int ret;
    int ret_value = SUCCEED;

    /* Allocate user data for iteration by creation order */
    if(NULL == (ibco_udata = DV_calloc(sizeof(H5_daos_link_ibco_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate udata struct");
    ibco_udata->target_grp = target_grp;
    ibco_udata->base_iter = base_iter;

    /* Copy iter_data if this is the base of iteration, otherwise point to
     * existing iter_data */
    if(base_iter) {
        if(NULL == (ibco_udata->iter_data = (H5_daos_iter_data_t *)DV_malloc(sizeof(H5_daos_iter_data_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate iteration data");
        memcpy(ibco_udata->iter_data, iter_data, sizeof(*iter_data));
    } /* end if */
    else
        ibco_udata->iter_data = iter_data;

    /* Retrieve the number of links in the group */
    if(H5_daos_group_get_num_links(target_grp, &ibco_udata->grp_nlinks, iter_data->req,
            first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, -H5_DAOS_DAOS_GET_ERROR, "can't get number of links in group");

    /* Create metatask for this iteration - body function will free udata.  Will
     * be scheduled later when iteration is complete. */
    if(0 != (ret = tse_task_create(H5_daos_link_ibco_end_task, &target_grp->obj.item.file->sched, ibco_udata, &ibco_udata->ibco_metatask)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for link iterate by creation order: %s", H5_daos_err_to_string(ret));

    /* Create task to continue this operation */
    if(0 !=  (ret = tse_task_create(H5_daos_link_ibco_task, &target_grp->obj.item.file->sched, ibco_udata, &ibco_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for link iterate by creation order: %s", H5_daos_err_to_string(ret));

    /* Register task dependency */
    if(*dep_task && 0 != (ret = tse_task_register_deps(ibco_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link iterate by creation order task: %s", H5_daos_err_to_string(ret));

    /* Schedule ibco task (or save it to be scheduled later) and give it a
     * reference to req, udata, and target_grp */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(ibco_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for link get name by name order: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = ibco_task;
    *dep_task = ibco_udata->ibco_metatask;
    iter_data->req->rc++;
    target_grp->obj.item.rc++;
    ibco_udata = NULL;

done:
    /* Cleanup on error */
    if(ibco_udata) {
        assert(ret_value < 0);

        if(base_iter)
            DV_free(ibco_udata->iter_data);
        if(ibco_udata->ibco_metatask)
            tse_task_complete(ibco_udata->ibco_metatask, ret_value);
        ibco_udata = DV_free(ibco_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_ibco_helper() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_iterate_by_crt_order
 *
 * Purpose:     Iterates over the links in the specified group according to
 *              their link creation order values. The supplied iter_data
 *              struct contains the iteration parameters.
 *
 * Return:      Success:        SUCCEED or positive
 *              Failure:        FAIL
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_iterate_by_crt_order(H5_daos_group_t *target_grp,
    H5_daos_iter_data_t *iter_data, tse_task_t **first_task,
    tse_task_t **dep_task)
{
    int ret;
    herr_t ret_value = SUCCEED;

    assert(target_grp);
    assert(iter_data);
    assert(H5_INDEX_CRT_ORDER == iter_data->index_type);
    assert(H5_ITER_NATIVE == iter_data->iter_order || H5_ITER_INC == iter_data->iter_order
            || H5_ITER_DEC == iter_data->iter_order);

    /* Increment reference count on root obj id */
    if(H5Iinc_ref(iter_data->iter_root_obj) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINC, FAIL, "can't increment reference count on iteration base object");

    /*
     * If iteration is recursive, setup a hash table to keep track of visited
     * group links so that cyclic links don't result in infinite looping.
     *
     * Also setup the recursive link path buffer, which keeps track of the full
     * path to the current link and is passed to the operator callback function.
     */
    assert(iter_data->u.link_iter_data.recurse_depth == 0);
    if(iter_data->is_recursive) {
        if(NULL == (iter_data->u.link_iter_data.visited_link_table = dv_hash_table_new(H5_daos_hash_obj_id, H5_daos_cmp_obj_id)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate visited links table");

        /*
         * Since the table key values are DV_malloc()ed, register the callback function to
         * call DV_free() on the keys when necessary.
         */
        dv_hash_table_register_free_functions(iter_data->u.link_iter_data.visited_link_table, H5_daos_free_visited_link_hash_table_key, NULL);

        /* Allocate the link path buffer for recursive iteration */
        if(NULL == (iter_data->u.link_iter_data.recursive_link_path = DV_malloc(H5_DAOS_RECURSE_LINK_PATH_BUF_INIT)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate link path buffer");
        iter_data->u.link_iter_data.recursive_link_path_nalloc = H5_DAOS_RECURSE_LINK_PATH_BUF_INIT;
        *iter_data->u.link_iter_data.recursive_link_path = '\0';
    } /* end if */

    /* Call helper function */
    if(0 != (ret = H5_daos_link_ibco_helper(target_grp, iter_data, TRUE, first_task, dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "failed to iterate over links: %s", H5_daos_err_to_string(ret));

done:
    /* Cleanup on error */
    if(ret_value < 0) {
        iter_data->u.link_iter_data.recursive_link_path = DV_free(iter_data->u.link_iter_data.recursive_link_path);

        if(iter_data->u.link_iter_data.visited_link_table) {
            dv_hash_table_free(iter_data->u.link_iter_data.visited_link_table);
            iter_data->u.link_iter_data.visited_link_table = NULL;
        } /* end if */

        if(H5Idec_ref(iter_data->iter_root_obj) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTDEC, -H5_DAOS_H5_CLOSE_ERROR, "can't decrement reference count on iteration base object");
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_iterate_by_crt_order() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_iterate_count_links_callback
 *
 * Purpose:     A callback for H5_daos_link_iterate() that simply counts
 *              the number of links in the given group.
 *
 * Return:      0 (can't fail)
 *
 * Programmer:  Jordan Henderson
 *              February, 2019
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_iterate_count_links_callback(hid_t H5VL_DAOS_UNUSED group, const char H5VL_DAOS_UNUSED *name,
    const H5L_info2_t H5VL_DAOS_UNUSED *info, void *op_data)
{
    (*((hsize_t *) op_data))++;
    return 0;
} /* end H5_daos_link_iterate_count_links_callback() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_delete
 *
 * Purpose:     Deletes the link from the specified group according to
 *              either the given link pathname or the given index into the
 *              group's link creation order/link name index.
 *
 * Return:      Success:        SUCCEED or positive
 *              Failure:        FAIL
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_delete(H5_daos_item_t *item, const H5VL_loc_params_t *loc_params,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_obj_t *target_obj = NULL;
    daos_key_t dkey;
    const char *grp_path = NULL;
    char *path_buf = NULL;
    const char *target_link_name = NULL;
    size_t target_link_name_len = 0;
    char *link_name_buf_dyn = NULL;
    char link_name_buf_static[H5_DAOS_LINK_NAME_BUF_SIZE];
    hsize_t grp_nlinks;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(item);
    assert(loc_params);
    assert(H5VL_OBJECT_BY_NAME == loc_params->type || H5VL_OBJECT_BY_IDX == loc_params->type);

    grp_path = (H5VL_OBJECT_BY_NAME == loc_params->type)
            ? loc_params->loc_data.loc_by_name.name : loc_params->loc_data.loc_by_idx.name;

    /* Traverse the path */
    if(NULL == (target_obj = H5_daos_group_traverse(item, grp_path,
            H5P_LINK_CREATE_DEFAULT, req, FALSE, &path_buf, &target_link_name,
            &target_link_name_len, first_task, dep_task)))
        D_GOTO_ERROR(H5E_SYM, H5E_TRAVERSE, FAIL, "can't traverse path");

    /* Check type of target_obj */
    if(target_obj->item.type != H5I_GROUP)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "target object is not a group");

    if(H5VL_OBJECT_BY_IDX == loc_params->type) {
        /* Retrieve the name of the link at the given index */
        /* Note this could be changed to get_name_by_idx_alloc to simplify things */
        if(H5_daos_link_get_name_by_idx((H5_daos_group_t *)target_obj, loc_params->loc_data.loc_by_idx.idx_type,
                loc_params->loc_data.loc_by_idx.order, (uint64_t)loc_params->loc_data.loc_by_idx.n,
                &target_link_name_len, link_name_buf_static, H5_DAOS_LINK_NAME_BUF_SIZE, req, first_task, dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get link name");

        /* Wait until everything is complete then check for errors
         * (temporary code until the rest of this function is async) */
        if(*first_task && (0 != (ret = tse_task_schedule(*first_task, false))))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
        if(H5_daos_progress(&item->file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
        *first_task = NULL;
        *dep_task = NULL;
        if(req->status < -H5_DAOS_INCOMPLETE)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(req->status));

        /* Check that buffer was large enough to fit link name */
        if(target_link_name_len > H5_DAOS_LINK_NAME_BUF_SIZE - 1) {
            if(NULL == (link_name_buf_dyn = DV_malloc(target_link_name_len + 1)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate buffer for link name");

            /* Re-issue the call with a larger buffer */
            if(H5_daos_link_get_name_by_idx((H5_daos_group_t *)target_obj, loc_params->loc_data.loc_by_idx.idx_type,
                    loc_params->loc_data.loc_by_idx.order, (uint64_t)loc_params->loc_data.loc_by_idx.n,
                    &target_link_name_len, link_name_buf_dyn, target_link_name_len + 1, req, first_task, dep_task) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get link name");

            /* Wait until everything is complete then check for errors
             * (temporary code until the rest of this function is async) */
            if(*first_task && (0 != (ret = tse_task_schedule(*first_task, false))))
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
            if(H5_daos_progress(&item->file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
            *first_task = NULL;
            *dep_task = NULL;
            if(req->status < -H5_DAOS_INCOMPLETE)
                D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(req->status));

            target_link_name = link_name_buf_dyn;
        } /* end if */
        else
            target_link_name = link_name_buf_static;
    } /* end if */

    /* Setup dkey */
    daos_iov_set(&dkey, (void *)target_link_name, target_link_name_len);

    /* Wait until everything is complete then check for errors
     * (temporary code until the rest of this function is async) */
    if(*first_task && (0 != (ret = tse_task_schedule(*first_task, false))))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
    if(H5_daos_progress(&item->file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
    *first_task = NULL;
    *dep_task = NULL;
    if(req->status < -H5_DAOS_INCOMPLETE)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(req->status));

    /* If link creation order is tracked, perform some bookkeeping */
    if(((H5_daos_group_t *)target_obj)->gcpl_cache.track_corder) {
        /* Update the "number of links" key in the group */
        if(H5_daos_group_get_num_links((H5_daos_group_t *)target_obj, &grp_nlinks, req,
                first_task, dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get number of links in group");

        /* Wait until everything is complete then check for errors
         * (temporary code until the rest of this function is async) */
        if(*first_task && (0 != (ret = tse_task_schedule(*first_task, false))))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
        if(H5_daos_progress(&item->file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
        *first_task = NULL;
        *dep_task = NULL;
        if(req->status < -H5_DAOS_INCOMPLETE)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(req->status));

        if(grp_nlinks > 0)
            grp_nlinks--;

        if(H5_daos_group_update_num_links_key((H5_daos_group_t *)target_obj, (uint64_t)grp_nlinks) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CANTMODIFY, FAIL, "can't update number of links in group");

        /* Remove the link from the group's creation order index */
        if(H5_daos_link_remove_from_crt_idx((H5_daos_group_t *)target_obj, loc_params, req,
                first_task, dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTREMOVE, FAIL, "failed to remove link from creation order index");
    } /* end if */

    /* Punch the link's dkey, along with all of its akeys */
    if(0 != (ret = daos_obj_punch_dkeys(target_obj->obj_oh, DAOS_TX_NONE, 0 /*flags*/, 1, &dkey, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTREMOVE, FAIL, "failed to punch link dkey: %s", H5_daos_err_to_string(ret));

    /* TODO: If no more hard links point to the object in question, it should be
     * removed from the file, or at least marked to be removed.
     */

done:
    /* Free path_buf if necessary */
    if(path_buf && H5_daos_free_async(item->file, path_buf, first_task, dep_task) < 0)
        D_DONE_ERROR(H5E_LINK, H5E_CANTFREE, FAIL, "can't free path buffer");

    if(link_name_buf_dyn)
        link_name_buf_dyn = DV_free(link_name_buf_dyn);
    if(target_obj && H5_daos_object_close(target_obj, req->dxpl_id, NULL) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close object");

    D_FUNC_LEAVE;
} /* end H5_daos_link_delete() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_remove_from_crt_idx
 *
 * Purpose:     Removes the target link from the target group's link
 *              creation order index by locating the relevant akeys and
 *              then removing them.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_remove_from_crt_idx(H5_daos_group_t *target_grp, const H5VL_loc_params_t *loc_params,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    daos_key_t dkey;
    daos_key_t akeys[2];
    uint64_t delete_idx = 0;
    uint8_t crt_order_target_buf[H5_DAOS_CRT_ORDER_TO_LINK_TRGT_BUF_SIZE];
    uint8_t idx_buf[H5_DAOS_ENCODED_CRT_ORDER_SIZE];
    uint8_t *p;
    hsize_t grp_nlinks_remaining;
    hid_t target_grp_id = H5I_INVALID_HID;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(target_grp);
    assert(loc_params);
    assert(H5VL_OBJECT_BY_NAME == loc_params->type || H5VL_OBJECT_BY_IDX == loc_params->type);
    H5daos_compile_assert(H5_DAOS_ENCODED_CRT_ORDER_SIZE == 8);

    /* Retrieve the current number of links in the group */
    if(H5_daos_group_get_num_links(target_grp, &grp_nlinks_remaining, req,
            first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get the number of links in group");

    /* Wait until everything is complete then check for errors
     * (temporary code until the rest of this function is async) */
    if(*first_task && (0 != (ret = tse_task_schedule(*first_task, false))))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
    if(H5_daos_progress(&req->file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
    *first_task = NULL;
    *dep_task = NULL;
    if(req->status < -H5_DAOS_INCOMPLETE)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(req->status));

    if(H5VL_OBJECT_BY_IDX == loc_params->type) {
        /* DSINC - no check for safe cast here */
        /*
         * Note that this assumes this routine is always called after a link's
         * dkey is punched during deletion, so the number of links in the group
         * should reflect the number after the link has been removed.
         */
        delete_idx = (H5_ITER_DEC == loc_params->loc_data.loc_by_idx.order) ?
                (uint64_t)grp_nlinks_remaining - (uint64_t)loc_params->loc_data.loc_by_idx.n :
                (uint64_t)loc_params->loc_data.loc_by_idx.n;
    } /* end if */
    else {
        H5_daos_link_crt_idx_iter_ud_t iter_cb_ud;
        H5_daos_iter_data_t iter_data;

        /* Register ID for group for link iteration */
        if((target_grp_id = H5VLwrap_register(target_grp, H5I_GROUP)) < 0)
            D_GOTO_ERROR(H5E_ATOM, H5E_CANTREGISTER, FAIL, "unable to atomize object handle");
        target_grp->obj.item.rc++;

        /* Initialize iteration data */
        iter_cb_ud.target_link_name = loc_params->loc_data.loc_by_name.name;
        iter_cb_ud.link_idx_out = &delete_idx;
        H5_DAOS_ITER_DATA_INIT(iter_data, H5_DAOS_ITER_TYPE_LINK, H5_INDEX_CRT_ORDER, H5_ITER_INC,
                FALSE, NULL, target_grp_id, &iter_cb_ud, H5P_DATASET_XFER_DEFAULT, req, first_task,
                dep_task);
        iter_data.u.link_iter_data.u.link_iter_op = H5_daos_link_remove_from_crt_idx_name_cb;

        /*
         * TODO: Currently, deleting a link by name means that we need to iterate through
         *       the link creation order index until we find the value corresponding to
         *       the link being deleted. This is especially important because the deletion
         *       of links might cause the target link's index value to shift downwards.
         *
         *       Once iteration restart is supported for link iteration, performance can
         *       be improved here by first looking up the original, permanent creation order
         *       value of the link using the 'link name -> creation order' mapping and then
         *       using that value as the starting point for iteration. In this case, the
         *       iteration order MUST be switched to H5_ITER_DEC or the key will not be
         *       found by the iteration.
         */
        if(H5_daos_link_iterate(target_grp, &iter_data, first_task, dep_task) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_BADITER, FAIL, "link iteration failed");

        /* Wait until everything is complete then check for errors
         * (temporary code until the rest of this function is async) */
        if(*first_task && (0 != (ret = tse_task_schedule(*first_task, false))))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule initial task for H5 operation: %s", H5_daos_err_to_string(ret));
        if(H5_daos_progress(&req->file->sched, NULL, H5_DAOS_PROGRESS_WAIT) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't progress scheduler");
        *first_task = NULL;
        *dep_task = NULL;
        if(req->status < -H5_DAOS_INCOMPLETE)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "asynchronous task failed: %s", H5_daos_err_to_string(req->status));
    } /* end else */

    /* Set up dkey */
    daos_iov_set(&dkey, (void *)H5_daos_link_corder_key_g, H5_daos_link_corder_key_size_g);

    /* Set up akeys */

    /* Remove the akey which maps creation order -> link name */
    p = idx_buf;
    UINT64ENCODE(p, delete_idx);
    daos_iov_set(&akeys[0], (void *)idx_buf, H5_DAOS_ENCODED_CRT_ORDER_SIZE);

    /* Remove the akey which maps creation order -> link target */
    p = crt_order_target_buf;
    UINT64ENCODE(p, delete_idx);
    crt_order_target_buf[H5_DAOS_CRT_ORDER_TO_LINK_TRGT_BUF_SIZE - 1] = 0;
    daos_iov_set(&akeys[1], (void *)crt_order_target_buf, H5_DAOS_CRT_ORDER_TO_LINK_TRGT_BUF_SIZE);

    /* Remove the akeys */
    if(0 != (ret = daos_obj_punch_akeys(target_grp->obj.obj_oh, DAOS_TX_NONE, 0 /*flags*/, &dkey, 2, akeys, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTREMOVE, FAIL, "failed to punch link akeys: %s", H5_daos_err_to_string(ret));

    /*
     * If there are still links remaining in the group and we didn't delete the
     * link currently at the end of the creation order index, shift the indices
     * of all akeys past the removed link's akeys down by one. This maintains
     * the ability to directly index into the link creation order index.
     */
    if((grp_nlinks_remaining > 0) && (delete_idx < (uint64_t)grp_nlinks_remaining))
        if(H5_daos_link_shift_crt_idx_keys_down(target_grp, delete_idx + 1, (uint64_t)grp_nlinks_remaining) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTMODIFY, FAIL, "failed to update link creation order index");

done:
    if((target_grp_id >= 0) && (H5Idec_ref(target_grp_id) < 0))
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group ID");

    D_FUNC_LEAVE;
} /* end H5_daos_link_remove_from_crt_idx() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_remove_from_crt_idx_name_cb
 *
 * Purpose:     Link iteration callback for
 *              H5_daos_link_remove_from_crt_idx which iterates through
 *              links by creation order until the current link name matches
 *              the target link name, at which point the creation order
 *              index value for the target link has been found.
 *
 * Return:      Non-negative (can't fail)
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_remove_from_crt_idx_name_cb(hid_t H5VL_DAOS_UNUSED group, const char *name,
    const H5L_info2_t H5VL_DAOS_UNUSED *info, void *op_data)
{
    H5_daos_link_crt_idx_iter_ud_t *cb_ud = (H5_daos_link_crt_idx_iter_ud_t *) op_data;

    if(!strcmp(name, cb_ud->target_link_name))
        return 1;

    (*cb_ud->link_idx_out)++;
    return 0;
} /* end H5_daos_link_remove_from_crt_idx_name_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_shift_crt_idx_keys_down
 *
 * Purpose:     After a link has been deleted in a group, this routine
 *              is used to update a group's link creation order index. All
 *              of the index's akeys within the range specified by the
 *              begin and end index parameters are read and then re-written
 *              to the index under new akeys whose integer 'name' values
 *              are one less than the akeys' original values.
 *
 *              By shifting these indices downward, the creation order
 *              index will not contain any holes and will maintain its
 *              ability to be directly indexed into.
 *
 *              TODO: Currently, this routine attempts to avoid calls to
 *                    the server by allocating buffers for all of the keys
 *                    and then reading/writing them at once. However, this
 *                    leads to several tiny allocations and the potential
 *                    for a very large amount of memory usage, which could
 *                    be improved upon.
 *
 *                    One improvement would be to allocate a single large
 *                    buffer for the key data and then set indices into the
 *                    buffer appropriately in each of the SGLs. This would
 *                    help in avoiding the tiny allocations for the data
 *                    buffers for each key.
 *
 *                    Another improvement would be to pick a sensible upper
 *                    bound on the amount of keys handled at a single time
 *                    and then perform several rounds of reading/writing
 *                    until all of the keys have been processed. This
 *                    should help to minimize the total amount of memory
 *                    that is used at any point in time.
 *
 * Return:      Non-negative on success/negative on failure
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_shift_crt_idx_keys_down(H5_daos_group_t *target_grp,
    uint64_t idx_begin, uint64_t idx_end)
{
    daos_sg_list_t *sgls = NULL;
    daos_iod_t *iods = NULL;
    daos_iov_t *sg_iovs = NULL;
    daos_key_t dkey;
    daos_key_t tail_akeys[2];
    uint64_t tmp_uint;
    uint8_t *crt_order_link_name_buf = NULL;
    uint8_t *crt_order_link_trgt_buf = NULL;
    uint8_t *p;
    size_t nlinks_shift;
    size_t i;
    char *tmp_buf = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(target_grp);
    assert(idx_end >= idx_begin);
    H5daos_compile_assert(H5_DAOS_ENCODED_CRT_ORDER_SIZE == 8);

    nlinks_shift = idx_end - idx_begin + 1;

    /*
     * Allocate space for the 2 akeys per link, one akey that maps the link's
     * creation order value to the link's name and one akey that maps the link's
     * creation order value to the link's target.
     */
    if(NULL == (iods = DV_malloc(2 * nlinks_shift * sizeof(*iods))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate IOD buffer");
    if(NULL == (sgls = DV_malloc(2 * nlinks_shift * sizeof(*sgls))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate SGL buffer");
    if(NULL == (sg_iovs = DV_calloc(2 * nlinks_shift * sizeof(*sg_iovs))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate IOV buffer");
    if(NULL == (crt_order_link_name_buf = DV_malloc(nlinks_shift * H5_DAOS_ENCODED_CRT_ORDER_SIZE)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate akey data buffer");
    /*
     * The 'creation order -> link target' akey's integer 'name' value is a 9-byte buffer:
     * 8 bytes for the integer creation order value + 1 0-byte at the end of the buffer.
     */
    if(NULL == (crt_order_link_trgt_buf = DV_malloc(nlinks_shift * (H5_DAOS_ENCODED_CRT_ORDER_SIZE + 1))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate akey data buffer");

    /* Set up dkey */
    daos_iov_set(&dkey, (void *)H5_daos_link_corder_key_g, H5_daos_link_corder_key_size_g);

    /* Set up iods */
    for(i = 0; i < nlinks_shift; i++) {
        tmp_uint = idx_begin + i;

        /* Setup the integer 'name' value for the current 'creation order -> link name' akey */
        p = &crt_order_link_name_buf[i * H5_DAOS_ENCODED_CRT_ORDER_SIZE];
        UINT64ENCODE(p, tmp_uint);

        /* Set up iods for the current 'creation order -> link name' akey */
        memset(&iods[2 * i], 0, sizeof(*iods));
        daos_iov_set(&iods[2 * i].iod_name, &crt_order_link_name_buf[i * H5_DAOS_ENCODED_CRT_ORDER_SIZE], H5_DAOS_ENCODED_CRT_ORDER_SIZE);
        iods[2 * i].iod_nr = 1u;
        iods[2 * i].iod_size = DAOS_REC_ANY;
        iods[2 * i].iod_type = DAOS_IOD_SINGLE;

        /* Setup the integer 'name' value for the current 'creation order -> link target' akey */
        p = &crt_order_link_trgt_buf[i * (H5_DAOS_ENCODED_CRT_ORDER_SIZE + 1)];
        UINT64ENCODE(p, tmp_uint);
        *p++ = 0;

        /* Set up iods for the current 'creation order -> link target' akey */
        memset(&iods[(2 * i) + 1], 0, sizeof(*iods));
        daos_iov_set(&iods[(2 * i) + 1].iod_name, &crt_order_link_trgt_buf[i * (H5_DAOS_ENCODED_CRT_ORDER_SIZE + 1)], H5_DAOS_ENCODED_CRT_ORDER_SIZE + 1);
        iods[(2 * i) + 1].iod_nr = 1u;
        iods[(2 * i) + 1].iod_size = DAOS_REC_ANY;
        iods[(2 * i) + 1].iod_type = DAOS_IOD_SINGLE;
    } /* end for */

    /* Fetch the data size for each akey */
    if(0 != (ret = daos_obj_fetch(target_grp->obj.obj_oh, DAOS_TX_NONE, 0 /*flags*/, &dkey, (unsigned) 2 * nlinks_shift,
            iods, NULL, NULL /*maps*/, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_READERROR, FAIL, "can't read akey data sizes: %s", H5_daos_err_to_string(ret));

    /* Allocate buffers and setup sgls for each akey */
    for(i = 0; i < nlinks_shift; i++) {
        /* Allocate buffer for the current 'creation order -> link name' akey */
        if(iods[2 * i].iod_size == 0)
            D_GOTO_ERROR(H5E_SYM, H5E_BADSIZE, FAIL, "invalid iod size - missing metadata");
        if(NULL == (tmp_buf = DV_malloc(iods[2 * i].iod_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey data");

        /* Set up sgls for the current 'creation order -> link name' akey */
        daos_iov_set(&sg_iovs[2 * i], tmp_buf, iods[2 * i].iod_size);
        sgls[2 * i].sg_nr = 1;
        sgls[2 * i].sg_nr_out = 0;
        sgls[2 * i].sg_iovs = &sg_iovs[2 * i];

        /* Allocate buffer for the current 'creation order -> link target' akey */
        if(iods[(2 * i) + 1].iod_size == 0)
            D_GOTO_ERROR(H5E_SYM, H5E_BADSIZE, FAIL, "invalid iod size - missing metadata");
        if(NULL == (tmp_buf = DV_malloc(iods[(2 * i) + 1].iod_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey data");

        /* Set up sgls for the current 'creation order -> link target' akey */
        daos_iov_set(&sg_iovs[(2 * i) + 1], tmp_buf, iods[(2 * i) + 1].iod_size);
        sgls[(2 * i) + 1].sg_nr = 1;
        sgls[(2 * i) + 1].sg_nr_out = 0;
        sgls[(2 * i) + 1].sg_iovs = &sg_iovs[(2 * i) + 1];
    } /* end for */

    /* Read the akey's data */
    if(0 != (ret = daos_obj_fetch(target_grp->obj.obj_oh, DAOS_TX_NONE, 0 /*flags*/, &dkey, (unsigned) 2 * nlinks_shift,
            iods, sgls, NULL /*maps*/, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_READERROR, FAIL, "can't read akey data: %s", H5_daos_err_to_string(ret));

    /*
     * Adjust the akeys down by setting their integer 'name' values to
     * one less than their original values
     */
    for(i = 0; i < nlinks_shift; i++) {
        /* Setup the integer 'name' value for the current 'creation order -> link name' akey */
        p = &crt_order_link_name_buf[i * H5_DAOS_ENCODED_CRT_ORDER_SIZE];
        UINT64DECODE(p, tmp_uint);

        tmp_uint--;
        p = &crt_order_link_name_buf[i * H5_DAOS_ENCODED_CRT_ORDER_SIZE];
        UINT64ENCODE(p, tmp_uint);

        /* Setup the integer 'name' value for the current 'creation order -> link target' akey */
        p = &crt_order_link_trgt_buf[i * (H5_DAOS_ENCODED_CRT_ORDER_SIZE + 1)];
        UINT64ENCODE(p, tmp_uint);
        *p++ = 0;
    } /* end for */

    /* Write the akeys back */
    if(0 != (ret = daos_obj_update(target_grp->obj.obj_oh, DAOS_TX_NONE, 0 /*flags*/, &dkey, (unsigned) 2 * nlinks_shift,
            iods, sgls, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_WRITEERROR, FAIL, "can't write akey data: %s", H5_daos_err_to_string(ret));

    /* Delete the (now invalid) akeys at the end of the creation index */
    tmp_uint = idx_end;
    p = &crt_order_link_name_buf[0];
    UINT64ENCODE(p, tmp_uint);
    daos_iov_set(&tail_akeys[0], (void *)crt_order_link_name_buf, H5_DAOS_ENCODED_CRT_ORDER_SIZE);

    p = &crt_order_link_trgt_buf[0];
    UINT64ENCODE(p, tmp_uint);
    *p++ = 0;
    daos_iov_set(&tail_akeys[1], (void *)crt_order_link_trgt_buf, H5_DAOS_ENCODED_CRT_ORDER_SIZE + 1);

    if(0 != (ret = daos_obj_punch_akeys(target_grp->obj.obj_oh, DAOS_TX_NONE, 0 /*flags*/, &dkey,
            2, tail_akeys, NULL /*event*/)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTDELETE, FAIL, "can't trim tail akeys from link creation order index");

done:
    for(i = 0; i < nlinks_shift; i++) {
        if(sg_iovs[2 * i].iov_buf)
            sg_iovs[2 * i].iov_buf = DV_free(sg_iovs[2 * i].iov_buf);
        if(sg_iovs[(2 * i) + 1].iov_buf)
            sg_iovs[(2 * i) + 1].iov_buf = DV_free(sg_iovs[(2 * i) + 1].iov_buf);
    } /* end for */
    if(crt_order_link_trgt_buf)
        crt_order_link_trgt_buf = DV_free(crt_order_link_trgt_buf);
    if(crt_order_link_name_buf)
        crt_order_link_name_buf = DV_free(crt_order_link_name_buf);
    if(sg_iovs)
        sg_iovs = DV_free(sg_iovs);
    if(sgls)
        sgls = DV_free(sgls);
    if(iods)
        iods = DV_free(iods);

    D_FUNC_LEAVE;
} /* end H5_daos_link_shift_crt_idx_keys_down() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_gnbi_alloc_task
 *
 * Purpose:     Asynchronous task for
 *              H5_daos_link_get_name_by_idx_alloc().  Executes once all
 *              parameters are valid.
 *
 * Return:      Success:        0
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_gnbi_alloc_task(tse_task_t *task)
{
    H5_daos_link_gnbi_alloc_ud_t *udata = NULL;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get name by creation order task");

    /* Handle errors in previous tasks */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Check if we need to issue another get operation */
    if(*udata->link_name_size > udata->cur_link_name_size - 1) {
        /* Reallocate buffer */
        DV_free(*udata->link_name_buf);
        if(NULL == (*udata->link_name_buf = DV_malloc(*udata->link_name_size + 1)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate link name buffer");
        udata->cur_link_name_size = *udata->link_name_size + 1;

        /* Reissue call with larger buffer and transfer ownership of udata */
        if(H5_daos_link_get_name_by_idx(udata->target_grp, udata->index_type,
                udata->iter_order, udata->idx, udata->link_name_size,
                *udata->link_name_buf, udata->cur_link_name_size, udata->req,
                &first_task, &dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, -H5_DAOS_H5_GET_ERROR, "can't get link name by index");
        udata = NULL;
    } /* end if */

done:
    /* Finish task if we still own udata */
    if(udata) {
        /* Assign link_name */
        *udata->link_name = *udata->link_name_buf;

        /* Return link_name_buf_size */
        if(udata->link_name_buf_size)
            *udata->link_name_buf_size = udata->cur_link_name_size;

        /* Close target_grp */
        if(H5_daos_group_close(udata->target_grp, H5I_INVALID_HID, NULL) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
        udata->target_grp = NULL;

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->req->status = ret_value;
            udata->req->failed_task = "link get name by index end task";
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Complete main task if different from this task */
        if(udata->gnbi_task != task)
            tse_task_complete(udata->gnbi_task, ret_value);

        /* Complete this task */
        tse_task_complete(task, ret_value);

        /* Free udata */
        udata = DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_gnbi_alloc_task() */
    

/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_name_by_idx_alloc
 *
 * Purpose:     Like H5_daos_link_get_name_by_idx, but automatically
 *              allocates the link_name buffer.
 *
 * Return:      Success:        SUCCEED (0)
 *              Failure:        FAIL (Negative)
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_get_name_by_idx_alloc(H5_daos_group_t *target_grp,
    H5_index_t index_type, H5_iter_order_t iter_order, uint64_t idx,
    const char **link_name, size_t *link_name_size, char **link_name_buf,
    size_t *link_name_buf_size, H5_daos_req_t *req, tse_task_t **first_task,
    tse_task_t **dep_task)
{
    H5_daos_link_gnbi_alloc_ud_t *gnbi_udata = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    /* Allocate task udata struct */
     if(NULL == (gnbi_udata = (H5_daos_link_gnbi_alloc_ud_t *)DV_calloc(sizeof(H5_daos_link_gnbi_alloc_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate get name by index user data");
    gnbi_udata->req = req;
    gnbi_udata->target_grp = target_grp;
    gnbi_udata->index_type = index_type;
    gnbi_udata->iter_order = iter_order;
    gnbi_udata->idx = idx;
    gnbi_udata->link_name = link_name;
    gnbi_udata->link_name_size = link_name_size;
    gnbi_udata->link_name_buf = link_name_buf;
    gnbi_udata->link_name_buf_size = link_name_buf_size;

    /* Check for preexisting name buffer */
    if(*link_name_buf) {
        assert(link_name_buf_size);
        assert(*link_name_buf_size);
        gnbi_udata->cur_link_name_size = *link_name_buf_size;
    } /* end if */
    else {
        /* Allocate initial name buffer */
        if(NULL == (*gnbi_udata->link_name_buf = DV_malloc(H5_DAOS_LINK_NAME_BUF_SIZE)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate link name buffer");
        gnbi_udata->cur_link_name_size = H5_DAOS_LINK_NAME_BUF_SIZE;
    } /* end else */

    /* Call underlying function */
    if(H5_daos_link_get_name_by_idx(target_grp, index_type, iter_order, idx,
            gnbi_udata->link_name_size, *gnbi_udata->link_name_buf,
            gnbi_udata->cur_link_name_size, req, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get link name by index");

    /* Create task to finish this operation */
    if(0 !=  (ret = tse_task_create(H5_daos_link_gnbi_alloc_task, &target_grp->obj.item.file->sched, gnbi_udata, &gnbi_udata->gnbi_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task for link get name by index: %s", H5_daos_err_to_string(ret));

    /* Register task dependency */
    if(*dep_task && 0 != (ret = tse_task_register_deps(gnbi_udata->gnbi_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for link get name by index task: %s", H5_daos_err_to_string(ret));

    /* Schedule gnbc task (or save it to be scheduled later) and give it a
     * reference to the group, req and udata */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(gnbi_udata->gnbi_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task for link get name by index: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = gnbi_udata->gnbi_task;
    *dep_task = gnbi_udata->gnbi_task;
    target_grp->obj.item.rc++;
    req->rc++;
    gnbi_udata = NULL;

done:
    /* Clean up */
    if(gnbi_udata) {
        assert(ret_value < 0);
        gnbi_udata = DV_free(gnbi_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_get_name_by_idx_alloc() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_name_by_idx
 *
 * Purpose:     Given an index type, index iteration order and index value,
 *              retrieves the name of the nth link (as specified by the
 *              index value) within the given index (name index or creation
 *              order index) according to the given order (increasing,
 *              decreasing or native order).
 *
 *              The link_name_out parameter may be NULL, in which case the
 *              length of the link's name is simply returned. If non-NULL,
 *              the link's name is stored in link_name_out.
 *
 * Return:      Success:        SUCCEED (0)
 *              Failure:        FAIL (Negative)
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_get_name_by_idx(H5_daos_group_t *target_grp, H5_index_t index_type,
    H5_iter_order_t iter_order, uint64_t idx, size_t *link_name_size,
    char *link_name_out, size_t link_name_out_size, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    herr_t ret_value = SUCCEED;

    assert(target_grp);

    if(H5_INDEX_CRT_ORDER == index_type) {
        if(H5_daos_link_get_name_by_crt_order(target_grp, iter_order, idx,
                link_name_size, link_name_out, link_name_out_size, req,
                first_task, dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't retrieve link name from creation order index");
    } /* end if */
    else if(H5_INDEX_NAME == index_type) {
        if(H5_daos_link_get_name_by_name_order(target_grp, iter_order, idx,
                link_name_size, link_name_out, link_name_out_size, req,
                first_task, dep_task) < 0)
            D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't retrieve link name from name order index");
    } /* end else */
    else
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, FAIL, "invalid or unsupported index type");

done:
    D_FUNC_LEAVE;
} /* end H5_daos_link_get_name_by_idx() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_gnbc_task
 *
 * Purpose:     Asynchronous task for
 *              H5_daos_link_get_name_by_crt_order().  Executes once
 *              target_grp is valid.
 *
 * Return:      Success:        0
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_gnbc_task(tse_task_t *task)
{
    H5_daos_link_gnbc_ud_t *udata = NULL;
    uint64_t fetch_idx = 0;
    tse_task_t *fetch_task = NULL;
    tse_task_t *first_task = NULL;
    uint8_t *p;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get name by creation order task");

    assert(udata->md_rw_cb_ud.obj->item.type == H5I_GROUP);

    /* Handle errors in previous tasks */
    if(udata->md_rw_cb_ud.req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->md_rw_cb_ud.req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Check that creation order is tracked for target group */
    if(!((H5_daos_group_t *)(udata->md_rw_cb_ud.obj))->gcpl_cache.track_corder)
        D_GOTO_ERROR(H5E_SYM, H5E_BADVALUE, (-1), "creation order is not tracked for group");

    /* Ensure the index is within range */
    if(udata->index >= (uint64_t)udata->grp_nlinks)
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "index value out of range");

    /* Calculate the correct index of the link, based upon the iteration order */
    if(H5_ITER_DEC == udata->iter_order)
        fetch_idx = (uint64_t)udata->grp_nlinks - udata->index - 1;
    else
        fetch_idx = udata->index;

    p = udata->idx_buf;
    UINT64ENCODE(p, fetch_idx);

    /* Set up dkey */
    daos_iov_set(&udata->md_rw_cb_ud.dkey, (void *)H5_daos_link_corder_key_g, H5_daos_link_corder_key_size_g);

    /* Set nr */
    udata->md_rw_cb_ud.nr = 1;

    /* Set up iod */
    daos_iov_set(&udata->md_rw_cb_ud.iod[0].iod_name, (void *)udata->idx_buf, H5_DAOS_ENCODED_CRT_ORDER_SIZE);
    udata->md_rw_cb_ud.iod[0].iod_nr = 1u;
    udata->md_rw_cb_ud.iod[0].iod_size = DAOS_REC_ANY;
    udata->md_rw_cb_ud.iod[0].iod_type = DAOS_IOD_SINGLE;

    /* Set up sgl if link_name_out buffer is supplied */
    if(udata->link_name_out && udata->link_name_out_size > 0) {
        daos_iov_set(&udata->md_rw_cb_ud.sg_iov[0], udata->link_name_out, udata->link_name_out_size - 1);
        udata->md_rw_cb_ud.sgl[0].sg_nr = 1;
        udata->md_rw_cb_ud.sgl[0].sg_nr_out = 0;
        udata->md_rw_cb_ud.sgl[0].sg_iovs = &udata->md_rw_cb_ud.sg_iov[0];
    } /* end if */

    /* Do not free buffers */
    udata->md_rw_cb_ud.free_akeys = FALSE;
    udata->md_rw_cb_ud.free_dkey = FALSE;

    /* Set task name */
    udata->md_rw_cb_ud.task_name = "link get name by crt order fetch";

    /* Create task for name fetch */
    if(0 != (ret = daos_task_create(DAOS_OPC_OBJ_FETCH, &udata->md_rw_cb_ud.obj->item.file->sched, 0, NULL, &fetch_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task to read link name by creation order: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for name fetch */
    if(0 != (ret = tse_task_register_cbs(fetch_task, H5_daos_md_rw_prep_cb, NULL, 0, H5_daos_link_gnbc_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't register callbacks for task to get link name by creation order: %s", H5_daos_err_to_string(ret));

    /* Set private data for name fetch */
    (void)tse_task_set_priv(fetch_task, udata);

    /* Save fetch task to be scheduled later and transfer ownership of udata */
    assert(!first_task);
    first_task = fetch_task;
    udata = NULL;

done:
    /* Schedule first task */
    if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
        D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link get name by name: %s", H5_daos_err_to_string(ret));

    /* Cleanup on failure */
    if(udata) {
        assert(ret_value < 0);

        /* Close target_grp */
        if(H5_daos_group_close((H5_daos_group_t *)udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
        udata->md_rw_cb_ud.obj = NULL;

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value != -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = ret_value;
            udata->md_rw_cb_ud.req->failed_task = "link get name by creation order task";
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Complete task */
        tse_task_complete(task, ret_value);

        /* Free udata */
        udata = DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_gnbc_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_gnbc_comp_cb
 *
 * Purpose:     Completion callback for name fetch by creation order from
 *              H5_daos_link_gnbc_task().
 *
 * Return:      Success:        The length of the link's name
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_gnbc_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_link_gnbc_ud_t *udata = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get name by creation order task");

    /* Handle errors in fetch task.  Only record error in udata->req_status if
     * it does not already contain an error (it could contain an error if
     * another task this task is not dependent on also failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR && task->dt_result != -DER_REC2BIG
            && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->md_rw_cb_ud.req->status = task->dt_result;
        udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
    } /* end if */
    else if(task->dt_result == 0 || task->dt_result == -DER_REC2BIG) {
        /* Check for link not found */
        if(udata->md_rw_cb_ud.iod[0].iod_size == (daos_size_t)0)
            D_DONE_ERROR(H5E_LINK, H5E_NOTFOUND, -H5_DAOS_DAOS_GET_ERROR, "link name record not found");
        else {
            /* Add null terminator */
            if(udata->link_name_out && udata->link_name_out_size > 0)
                udata->link_name_out[MIN(udata->md_rw_cb_ud.iod[0].iod_size, udata->link_name_out_size - 1)] = '\0';

            /* Set *link_name_size */
            *udata->link_name_size = (size_t)udata->md_rw_cb_ud.iod[0].iod_size;
        } /* end else */
    } /* end if */

    /* Complete main task */
    tse_task_complete(udata->gnbc_task, ret_value);

    /* Close target_grp */
    if(H5_daos_group_close((H5_daos_group_t *)udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
        D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
    udata->md_rw_cb_ud.obj = NULL;

    /* Handle errors in this function */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->md_rw_cb_ud.req->status = ret_value;
        udata->md_rw_cb_ud.req->failed_task = "get link name by creation order completion callback";
    } /* end if */

    /* Release our reference to req */
    if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
        D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Free udata */
    udata = DV_free(udata);

done:
    D_FUNC_LEAVE;
} /* end H5_daos_link_gnbc_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_name_by_crt_order
 *
 * Purpose:     Given an index iteration order and index value, retrieves
 *              the name of the nth link (as specified by the index value)
 *              within the specified group's link creation order index,
 *              according to the given order (increasing, decreasing or
 *              native order).
 *
 *              The link_name_out parameter may be NULL, in which case the
 *              length of the link's name is simply returned. If non-NULL,
 *              the link's name is stored in link_name_out.
 *
 * Return:      Success:        Non-negative
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_get_name_by_crt_order(H5_daos_group_t *target_grp, H5_iter_order_t iter_order,
    uint64_t index, size_t *link_name_size, char *link_name_out, size_t link_name_out_size,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_gnbc_ud_t *gnbc_udata = NULL;
    int ret;
    ssize_t ret_value = 0;

    assert(target_grp);
    H5daos_compile_assert(H5_DAOS_ENCODED_CRT_ORDER_SIZE == 8);

    /* Allocate task udata struct */
    if(NULL == (gnbc_udata = (H5_daos_link_gnbc_ud_t *)DV_calloc(sizeof(H5_daos_link_gnbc_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate get name by creation order user data");
    gnbc_udata->md_rw_cb_ud.req = req;
    gnbc_udata->md_rw_cb_ud.obj = &target_grp->obj;
    gnbc_udata->iter_order = iter_order;
    gnbc_udata->index = index;
    gnbc_udata->link_name_size = link_name_size;
    gnbc_udata->link_name_out = link_name_out;
    gnbc_udata->link_name_out_size = link_name_out_size;

    /* Retrieve the current number of links in the group */
    if(H5_daos_group_get_num_links(target_grp, &gnbc_udata->grp_nlinks, req, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, (-1), "can't get number of links in group");

    /* Create task to finish this operation */
    if(0 !=  (ret = tse_task_create(H5_daos_link_gnbc_task, &target_grp->obj.item.file->sched, gnbc_udata, &gnbc_udata->gnbc_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task for link get name by creation order: %s", H5_daos_err_to_string(ret));

    /* Register task dependency */
    if(*dep_task && 0 != (ret = tse_task_register_deps(gnbc_udata->gnbc_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for link get name by creation order task: %s", H5_daos_err_to_string(ret));

    /* Schedule gnbc task (or save it to be scheduled later) and give it a
     * reference to the group, req and udata */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(gnbc_udata->gnbc_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task for link get name by creation order: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = gnbc_udata->gnbc_task;
    *dep_task = gnbc_udata->gnbc_task;
    target_grp->obj.item.rc++;
    req->rc++;
    gnbc_udata = NULL;

done:
    /* Clean up */
    if(gnbc_udata) {
        assert(ret_value < 0);
        gnbc_udata = DV_free(gnbc_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_get_name_by_crt_order() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_gnbn_task
 *
 * Purpose:     Asynchronous task for
 *              H5_daos_link_get_name_by_name_order().  Executes once
 *              target_grp is valid.
 *
 * Return:      Success:        0
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_gnbn_task(tse_task_t *task)
{
    H5_daos_link_gnbn_ud_t *udata = NULL;
    H5_daos_iter_data_t iter_data;
    tse_task_t *first_task = NULL;
    tse_task_t *dep_task = NULL;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get name by name task");

    /* Handle errors in previous tasks */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Ensure the index is within range */
    if(udata->index >= (uint64_t)udata->grp_nlinks)
        D_GOTO_ERROR(H5E_LINK, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "index value out of range");

    /* Initialize iteration data */
    udata->iter_cb_ud.target_link_idx = udata->index;
    udata->iter_cb_ud.cur_link_idx = 0;
    udata->iter_cb_ud.link_name_out = udata->link_name_out;
    udata->iter_cb_ud.link_name_out_size = udata->link_name_out_size;
    H5_DAOS_ITER_DATA_INIT(iter_data, H5_DAOS_ITER_TYPE_LINK, H5_INDEX_NAME, udata->iter_order,
            FALSE, NULL, udata->target_grp_id, &udata->iter_cb_ud, H5P_DATASET_XFER_DEFAULT, udata->req,
            &first_task, &dep_task);
    iter_data.u.link_iter_data.u.link_iter_op = H5_daos_link_get_name_by_name_order_cb;

    /* Perform iteration */
    if(H5_daos_link_iterate(udata->target_grp, &iter_data, &first_task, &dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_BADITER, -H5_DAOS_H5_GET_ERROR, "link iteration failed");

done:
    if(udata) {
        tse_task_t *end_task = NULL;

        /* Create task to finalize gnbn task */
        if(0 !=  (ret = tse_task_create(H5_daos_link_gnbn_end_task, &udata->req->file->sched, udata, &end_task)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task for link get name by name order end: %s", H5_daos_err_to_string(ret));
        else {
            /* Register task dependency */
            if(dep_task && 0 != (ret = tse_task_register_deps(end_task, 1, &dep_task)))
                D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create dependencies for link get name by name order end task: %s", H5_daos_err_to_string(ret));

            /* Schedule gnbn end task (or save it to be scheduled later) */
            if(first_task) {
                if(0 != (ret = tse_task_schedule(end_task, false)))
                    D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule task for link get name by name order end: %s", H5_daos_err_to_string(ret));
            } /* end if */
            else
                first_task = end_task;
            dep_task = end_task;
        } /* end else */

        /* Schedule first task */
        if(first_task && 0 != (ret = tse_task_schedule(first_task, false)))
            D_DONE_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't schedule initial task for link get name by name: %s", H5_daos_err_to_string(ret));

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->req->status = ret_value;
            udata->req->failed_task = "link get name by name task";
        } /* end if */
    } /* end if */
    else {
        assert(ret_value == -H5_DAOS_DAOS_GET_ERROR);
        tse_task_complete(task, ret_value);
    } /* end else */

    D_FUNC_LEAVE;
} /* end H5_daos_link_gnbn_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_gnbn_end_task
 *
 * Purpose:     Finalizes the link get name by name operation, setting
 *              output and freeing data.
 *
 * Return:      Success:        0
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_gnbn_end_task(tse_task_t *task)
{
    H5_daos_link_gnbn_ud_t *udata = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get name by name task");

    /* Handle errors in previous tasks */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT)
        ret_value = -H5_DAOS_PRE_ERROR;
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT)
        ret_value = -H5_DAOS_SHORT_CIRCUIT;

    /* Set *link_name_size */
    *udata->link_name_size = udata->iter_cb_ud.link_name_out_size;

    /* Complete main task */
    tse_task_complete(udata->gnbn_task, ret_value);

    /* Close group */
    if((udata->target_grp_id >= 0) && (H5Idec_ref(udata->target_grp_id) < 0))
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group ID");

    /* Handle errors in this function */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->req->status = ret_value;
        udata->req->failed_task = "get link name by name order end task";
    } /* end if */

    /* Release our reference to req */
    if(H5_daos_req_free_int(udata->req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Free udata */
    udata = DV_free(udata);

done:
    /* Complete this task */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_link_gnbn_end_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_name_by_name_order
 *
 * Purpose:     Given an index iteration order and index value, retrieves
 *              the name of the nth link (as specified by the index value)
 *              within the specified group's link name index, according to
 *              the given order (increasing, decreasing or native order).
 *
 *              The link_name_out parameter may be NULL, in which case the
 *              length of the link's name is simply returned. If non-NULL,
 *              the link's name is stored in link_name_out.
 *
 * Return:      Success:        Non-negative
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_get_name_by_name_order(H5_daos_group_t *target_grp, H5_iter_order_t iter_order,
    uint64_t index, size_t *link_name_size, char *link_name_out, size_t link_name_out_size,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_gnbn_ud_t *gnbn_udata = NULL;
    int ret;
    ssize_t ret_value = 0;

    assert(target_grp);

    if(H5_ITER_DEC == iter_order)
        D_GOTO_ERROR(H5E_LINK, H5E_UNSUPPORTED, FAIL, "decreasing order iteration is unsupported");

    /* Allocate task udata struct */
     if(NULL == (gnbn_udata = (H5_daos_link_gnbn_ud_t *)DV_calloc(sizeof(H5_daos_link_gnbn_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate get name by name order user data");
    gnbn_udata->req = req;
    gnbn_udata->target_grp_id = H5I_INVALID_HID;
    gnbn_udata->target_grp = target_grp;
    gnbn_udata->iter_order = iter_order;
    gnbn_udata->index = index;
    gnbn_udata->link_name_size = link_name_size;
    gnbn_udata->link_name_out = link_name_out;
    gnbn_udata->link_name_out_size = link_name_out_size;

    /* Register ID for target group */
    if((gnbn_udata->target_grp_id = H5VLwrap_register(target_grp, H5I_GROUP)) < 0)
        D_GOTO_ERROR(H5E_ATOM, H5E_CANTREGISTER, FAIL, "unable to atomize object handle");
    target_grp->obj.item.rc++;

    /* Retrieve the current number of links in the group */
    if(H5_daos_group_get_num_links(target_grp, &gnbn_udata->grp_nlinks, req, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_LINK, H5E_CANTGET, FAIL, "can't get number of links in group");

    /* Create task to finish this operation */
    if(0 !=  (ret = tse_task_create(H5_daos_link_gnbn_task, &target_grp->obj.item.file->sched, gnbn_udata, &gnbn_udata->gnbn_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create task for link get name by name order: %s", H5_daos_err_to_string(ret));

    /* Register task dependency */
    if(*dep_task && 0 != (ret = tse_task_register_deps(gnbn_udata->gnbn_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't create dependencies for link get name by name order task: %s", H5_daos_err_to_string(ret));

    /* Schedule gnbn task (or save it to be scheduled later) and give it a
     * reference to req and udata */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(gnbn_udata->gnbn_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task for link get name by name order: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = gnbn_udata->gnbn_task;
    *dep_task = gnbn_udata->gnbn_task;
    req->rc++;
    gnbn_udata = NULL;

done:
    /* Clean up */
    if(gnbn_udata) {
        assert(ret_value < 0);
        if(gnbn_udata->target_grp_id >= 0 && H5Idec_ref(gnbn_udata->target_grp_id) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group ID");
        gnbn_udata = DV_free(gnbn_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_get_name_by_name_order() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_name_by_name_order_cb
 *
 * Purpose:     Link iteration callback for
 *              H5_daos_link_get_name_by_name_order which iterates through
 *              links by name order until the specified index value is
 *              reached, at which point the target link has been found and
 *              its name is copied back.
 *
 * Return:      Non-negative (can't fail)
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_get_name_by_name_order_cb(hid_t H5VL_DAOS_UNUSED group, const char *name,
    const H5L_info2_t H5VL_DAOS_UNUSED *info, void *op_data)
{
    H5_daos_link_find_name_by_idx_ud_t *cb_ud = (H5_daos_link_find_name_by_idx_ud_t *) op_data;

    if(cb_ud->cur_link_idx == cb_ud->target_link_idx) {
        size_t name_len = strlen(name);

        /* Copy name to output buffer if present */
        if(cb_ud->link_name_out && cb_ud->link_name_out_size > 0) {
            size_t copy_len = MIN(name_len, cb_ud->link_name_out_size - 1);

            memcpy(cb_ud->link_name_out, name, copy_len);
            cb_ud->link_name_out[copy_len] = '\0';
        } /* end if */

        /* Return name length */
        cb_ud->link_name_out_size = name_len;

        return 1;
    } /* end if */

    cb_ud->cur_link_idx++;
    return 0;
} /* end H5_daos_link_get_name_by_name_order_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_gcbn_comp_cb
 *
 * Purpose:     Completion callback for creation order fetch for
 *              H5_daos_link_get_crt_order_by_name().
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_link_gcbn_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_link_gcbn_ud_t *udata = NULL;
    uint64_t crt_order_val;
    uint8_t *p;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for link get creation order by name task");

    /* Handle errors in fetch task.  Only record error in udata->req_status if
     * it does not already contain an error (it could contain an error if
     * another task this task is not dependent on also failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->md_rw_cb_ud.req->status = task->dt_result;
        udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
    } /* end if */
    else if(task->dt_result == 0) {
        /* Check that creation order is tracked for target group */
        /* Move this check to a custom prep cb? */
        if(!((H5_daos_group_t *)udata->md_rw_cb_ud.obj)->gcpl_cache.track_corder) {
            udata->md_rw_cb_ud.req->status = -H5_DAOS_BAD_VALUE;
            udata->md_rw_cb_ud.req->failed_task = udata->md_rw_cb_ud.task_name;
            D_GOTO_ERROR(H5E_SYM, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "creation order is not tracked for group");
        } /* end if */

        /* Check for no link found */
        if(udata->md_rw_cb_ud.iod[0].iod_size == 0)
            D_GOTO_ERROR(H5E_LINK, H5E_NOTFOUND, -H5_DAOS_DAOS_GET_ERROR, "link creation order value record is missing");

        /* Set output value */
        p = udata->crt_order_buf;
        UINT64DECODE(p, crt_order_val);

        *udata->crt_order = crt_order_val;
    } /* end else */

done:
    /* Clean up */
    if(udata) {
        /* Close target_grp */
        if(H5_daos_group_close((H5_daos_group_t *)udata->md_rw_cb_ud.obj, H5I_INVALID_HID, NULL) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close group");
        udata->md_rw_cb_ud.obj = NULL;

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->md_rw_cb_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->md_rw_cb_ud.req->status = ret_value;
            udata->md_rw_cb_ud.req->failed_task = "get link creation order by name completion callback";
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->md_rw_cb_ud.req) < 0)
            D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Free udata */
        udata = DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_gcbn_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_get_crt_order_by_name
 *
 * Purpose:     Retrieves the creation order value for a link given the
 *              group containing the link and the link's name.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_link_get_crt_order_by_name(H5_daos_group_t *target_grp, const char *link_name,
    uint64_t *crt_order, H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_link_gcbn_ud_t *fetch_udata = NULL;
    tse_task_t *fetch_task;;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(target_grp);
    assert(link_name);
    assert(crt_order);
    H5daos_compile_assert(H5_DAOS_ENCODED_CRT_ORDER_SIZE == 8);

    /* Allocate task udata struct */
     if(NULL == (fetch_udata = (H5_daos_link_gcbn_ud_t *)DV_calloc(sizeof(H5_daos_link_gcbn_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate get creation order by name user data");
    fetch_udata->md_rw_cb_ud.req = req;
    fetch_udata->md_rw_cb_ud.obj = &target_grp->obj;
    fetch_udata->crt_order = crt_order;

    /* Set up dkey */
    daos_iov_set(&fetch_udata->md_rw_cb_ud.dkey, (void *)link_name, strlen(link_name));

    /* Set nr */
    fetch_udata->md_rw_cb_ud.nr = 1;

    /* Set up iod */
    daos_iov_set(&fetch_udata->md_rw_cb_ud.iod[0].iod_name, (void *)H5_daos_link_corder_key_g, H5_daos_link_corder_key_size_g);
    fetch_udata->md_rw_cb_ud.iod[0].iod_nr = 1u;
    fetch_udata->md_rw_cb_ud.iod[0].iod_size = (daos_size_t)H5_DAOS_ENCODED_CRT_ORDER_SIZE;
    fetch_udata->md_rw_cb_ud.iod[0].iod_type = DAOS_IOD_SINGLE;

    /* Set up sgl */
    daos_iov_set(&fetch_udata->md_rw_cb_ud.sg_iov[0], fetch_udata->crt_order_buf, (daos_size_t)H5_DAOS_ENCODED_CRT_ORDER_SIZE);
    fetch_udata->md_rw_cb_ud.sgl[0].sg_nr = 1;
    fetch_udata->md_rw_cb_ud.sgl[0].sg_nr_out = 0;
    fetch_udata->md_rw_cb_ud.sgl[0].sg_iovs = &fetch_udata->md_rw_cb_ud.sg_iov[0];

    /* Do not free buffers */
    fetch_udata->md_rw_cb_ud.free_akeys = FALSE;
    fetch_udata->md_rw_cb_ud.free_dkey = FALSE;

    /* Set task name */
    fetch_udata->md_rw_cb_ud.task_name = "link get crt order by name fetch";

    /* Create task for creation order fetch */
    if(0 != (ret = daos_task_create(DAOS_OPC_OBJ_FETCH, &fetch_udata->md_rw_cb_ud.obj->item.file->sched, 0, NULL, &fetch_task)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't create task to read link creation order by name: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for creation order fetch */
    if(0 != (ret = tse_task_register_cbs(fetch_task, H5_daos_md_rw_prep_cb, NULL, 0, H5_daos_link_gcbn_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, ret, "can't register callbacks for task to get link name by creation order: %s", H5_daos_err_to_string(ret));

    /* Set private data for creation order fetch */
    (void)tse_task_set_priv(fetch_task, fetch_udata);

    /* Schedule fetch task (or save it to be scheduled later) and give it a
     * reference to the group, req and udata */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(fetch_task, false)))
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "can't schedule task for link get name by creation order: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = fetch_task;
    *dep_task = fetch_task;
    target_grp->obj.item.rc++;
    req->rc++;
    fetch_udata = NULL;

done:
    /* Clean up */
    if(fetch_udata) {
        assert(ret_value < 0);
        fetch_udata = DV_free(fetch_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_link_get_crt_order_by_name() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_hash_obj_id
 *
 * Purpose:     Helper function to "hash" a DAOS object ID's lower 64 bits
 *              by simply returning the value passed in.
 *
 * Return:      "hashed" DAOS object ID
 *
 *-------------------------------------------------------------------------
 */
static uint64_t
H5_daos_hash_obj_id(dv_hash_table_key_t obj_id_lo)
{
    return *((uint64_t *) obj_id_lo);
} /* end H5_daos_hash_obj_id() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_cmp_obj_id
 *
 * Purpose:     Helper function to compare two "hashed" DAOS object IDs in
 *              a dv_hash_table_t.
 *
 * Return:      Non-zero if the two keys are equal, zero if the keys are
 *              not equal.
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_cmp_obj_id(dv_hash_table_key_t obj_id_lo1, dv_hash_table_key_t obj_id_lo2)
{
    uint64_t val1 = *((uint64_t *) obj_id_lo1);
    uint64_t val2 = *((uint64_t *) obj_id_lo2);

    return (val1 == val2);
} /* H5_daos_cmp_obj_id() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_free_visited_link_hash_table_key
 *
 * Purpose:     Helper function to free keys in the visited link hash table
 *              used by link iteration.
 *
 * Return:      Nothing
 *
 *-------------------------------------------------------------------------
 */
static void
H5_daos_free_visited_link_hash_table_key(dv_hash_table_key_t value)
{
    DV_free(value);
    value = NULL;
} /* end H5_daos_free_visited_link_hash_table_key() */
