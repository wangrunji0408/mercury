/*
 * Copyright (C) 2013-2019 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "na_plugin.h"

#include "mercury_thread_spin.h"
#include "mercury_time.h"
#include "mercury_poll.h"
#include "mercury_event.h"
#include "mercury_mem.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
# include <process.h>
#else
# include <pwd.h>
# include <ftw.h>
# include <unistd.h>
# include <sys/types.h>
# include <sys/mman.h>
# include <sys/stat.h>
# include <fcntl.h>
# include <sys/socket.h>
# include <sys/un.h>
# if defined(NA_SM_HAS_CMA)
#  include <sys/uio.h>
#  include <limits.h>
# elif defined(__APPLE__)
#  include <mach/mach.h>
#  include <mach/mach_vm.h>
# endif
#endif

/****************/
/* Local Macros */
/****************/

/* Plugin constants */
#define NA_SM_MAX_FILENAME      64
#define NA_SM_NUM_BUFS          64
#define NA_SM_CACHE_LINE_SIZE   HG_UTIL_CACHE_ALIGNMENT
#define NA_SM_RING_BUF_SIZE \
    (sizeof(struct na_sm_ring_buf) + NA_SM_NUM_BUFS * HG_ATOMIC_QUEUE_ELT_SIZE)
#define NA_SM_COPY_BUF_SIZE     4096
#define NA_SM_CLEANUP_NFDS      16

#define NA_SM_LISTEN_BACKLOG    64
#define NA_SM_ACCEPT_INTERVAL   100 /* 100 ms */

/* Msg sizes */
#define NA_SM_UNEXPECTED_SIZE   NA_SM_COPY_BUF_SIZE
#define NA_SM_EXPECTED_SIZE     NA_SM_UNEXPECTED_SIZE

/* Max tag */
#define NA_SM_MAX_TAG           NA_TAG_UB

/* Op ID status bits */
#define NA_SM_OP_COMPLETED      (1 << 0)
#define NA_SM_OP_CANCELED       (1 << 1)
#define NA_SM_OP_QUEUED         (1 << 2)

/* Private data access */
#define NA_SM_CLASS(na_class) \
    ((struct na_sm_class *)(na_class->plugin_class))

/* Min macro */
#define NA_SM_MIN(a, b) \
    (a < b) ? a : b

/* Struct msghdr initializer */
#define NA_SM_MSGHDR_INITIALIZER {NULL, 0, NULL, 0, NULL, 0, 0}

/* Default filenames/paths */
#define NA_SM_SHM_PATH          "/dev/shm"

#define NA_SM_GEN_SHM_NAME(filename, username, na_sm_addr)      \
    do {                                                        \
        sprintf(filename, "%s_%s-%d-%u", NA_SM_SHM_PREFIX,      \
            username, na_sm_addr->pid, na_sm_addr->id);         \
    } while (0)

#define NA_SM_GEN_SOCK_PATH(pathname, username, na_sm_addr)                 \
    do {                                                                    \
        sprintf(pathname, "%s/%s_%s/%d/%u", NA_SM_TMP_DIRECTORY,            \
            NA_SM_SHM_PREFIX, username, na_sm_addr->pid, na_sm_addr->id);   \
    } while (0)

#define NA_SM_SEND_NAME "s" /* used for pair_name */
#define NA_SM_RECV_NAME "r" /* used for pair_name */
#define NA_SM_GEN_RING_NAME(filename, pair_name, username, na_sm_addr)      \
    do {                                                                    \
        sprintf(filename, "%s_%s-%d-%u-%u-" pair_name, NA_SM_SHM_PREFIX,    \
            username, na_sm_addr->pid, na_sm_addr->id, na_sm_addr->conn_id);\
    } while (0)

#ifndef HG_UTIL_HAS_SYSEVENTFD_H
#define NA_SM_GEN_FIFO_NAME(filename, pair_name, username, na_sm_addr)  \
    do {                                                                \
        sprintf(filename, "%s/%s_%s/%d/%u/fifo-%u-" pair_name,          \
            NA_SM_TMP_DIRECTORY, NA_SM_SHM_PREFIX, username,            \
            na_sm_addr->pid, na_sm_addr->id, na_sm_addr->conn_id);      \
    } while (0)
#endif

/************************************/
/* Local Type and Struct Definition */
/************************************/

typedef union {
    hg_atomic_int32_t val;
    char pad[NA_SM_CACHE_LINE_SIZE];
} na_sm_cacheline_atomic_int32_t;

typedef union {
    hg_atomic_int64_t val;
    char pad[NA_SM_CACHE_LINE_SIZE];
} na_sm_cacheline_atomic_int64_t;

typedef union {
    struct {
        unsigned int type       : 4;    /* Message type */
        unsigned int buf_idx    : 8;    /* Index reserved: 64 MAX */
        unsigned int buf_size   : 16;   /* Buffer length: 4KB MAX */
        unsigned int tag        : 32;   /* Message tag : UINT MAX */
        unsigned int pad        : 4;    /* 4 bits left */
    } hdr;
    na_uint64_t val;
} na_sm_cacheline_hdr_t;

/* Ring buffer */
struct na_sm_ring_buf {
    struct hg_atomic_queue queue;
    char pad[NA_SM_COPY_BUF_SIZE - sizeof(struct hg_atomic_queue)
             - NA_SM_NUM_BUFS * HG_ATOMIC_QUEUE_ELT_SIZE];
};

/* Shared copy buffer */
struct na_sm_copy_buf {
    na_sm_cacheline_atomic_int64_t available;       /* Atomic bitmask */
    char buf[NA_SM_NUM_BUFS][NA_SM_COPY_BUF_SIZE];  /* Buffer used for msgs */
    char pad[NA_SM_COPY_BUF_SIZE - NA_SM_CACHE_LINE_SIZE];
};

/* Poll type */
typedef enum na_sm_poll_type {
    NA_SM_ACCEPT = 1,
    NA_SM_SOCK,
    NA_SM_NOTIFY
} na_sm_poll_type_t;

/* Poll data */
struct na_sm_poll_data {
    na_class_t *na_class;
    struct na_sm_addr *addr; /* Address */
    na_sm_poll_type_t type;  /* Type of operation */
};

/* Sock progress type */
typedef enum {
    NA_SM_ADDR_INFO,
    NA_SM_CONN_ID,
    NA_SM_SOCK_DONE
} na_sm_sock_progress_t;

/* Address */
struct na_sm_addr {
    HG_QUEUE_ENTRY(na_sm_addr) entry;       /* Next queue entry */
    HG_QUEUE_ENTRY(na_sm_addr) poll_entry;  /* Next poll queue entry */
    struct na_sm_ring_buf *na_sm_send_ring_buf; /* Shared send ring buffer */
    struct na_sm_ring_buf *na_sm_recv_ring_buf; /* Shared recv ring buffer */
    struct na_sm_copy_buf *na_sm_copy_buf;  /* Shared copy buffer */
    struct na_sm_poll_data *sock_poll_data; /* Sock poll data */
    struct na_sm_poll_data *local_notify_poll_data; /* Notify poll data */
    pid_t pid;                              /* PID */
    na_sm_sock_progress_t sock_progress;    /* Current sock progress state */
    unsigned int id;                        /* SM ID */
    unsigned int conn_id;                   /* Connection ID */
    int sock;                               /* Sock fd */
    int local_notify;                       /* Local notify fd */
    int remote_notify;                      /* Remote notify fd */
    hg_atomic_int32_t ref_count;            /* Ref count */
    na_bool_t accepted;                     /* Created on accept */
    na_bool_t self;                         /* Self address */
};

/* Memory handle */
struct na_sm_mem_handle {
    struct iovec *iov;
    unsigned long iovcnt;
    unsigned long flags; /* Flag of operation access */
    size_t len;
};


/* Unexpected message info */
struct na_sm_unexpected_info {
    HG_QUEUE_ENTRY(na_sm_unexpected_info) entry;
    struct na_sm_addr *na_sm_addr;
    void *buf;
    na_size_t buf_size;
    na_tag_t tag;
};

/* Msg info */
struct na_sm_msg_info {
    union {
        const void *const_ptr;
        void *ptr;
    } buf;
    size_t buf_size;
    na_size_t actual_buf_size;
    na_tag_t tag;
};

/* Operation ID */
struct na_sm_op_id {
    struct na_cb_completion_data completion_data; /* Completion data */
    union {
        struct na_sm_msg_info msg;
    } info;                             /* Op info                  */
    HG_QUEUE_ENTRY(na_sm_op_id) entry;  /* Entry in queue           */
    na_class_t *na_class;               /* NA class associated      */
    na_context_t *context;              /* NA context associated    */
    struct na_sm_addr *na_sm_addr;      /* Address associated       */
    hg_atomic_int32_t status;           /* Operation status         */
    hg_atomic_int32_t ref_count;        /* Refcount                 */
};

/* Private data */
struct na_sm_class {
    HG_QUEUE_HEAD(na_sm_addr) accepted_addr_queue;
    HG_QUEUE_HEAD(na_sm_addr) poll_addr_queue;
    HG_QUEUE_HEAD(na_sm_unexpected_info) unexpected_msg_queue;
    HG_QUEUE_HEAD(na_sm_op_id) lookup_op_queue;
    HG_QUEUE_HEAD(na_sm_op_id) unexpected_op_queue;
    HG_QUEUE_HEAD(na_sm_op_id) expected_op_queue;
    HG_QUEUE_HEAD(na_sm_op_id) retry_op_queue;
    hg_time_t last_accept_time;
    char *username;
    struct na_sm_addr *self_addr;
    hg_poll_set_t *poll_set;
    hg_thread_spin_t accepted_addr_queue_lock;
    hg_thread_spin_t poll_addr_queue_lock;
    hg_thread_spin_t unexpected_msg_queue_lock;
    hg_thread_spin_t lookup_op_queue_lock;
    hg_thread_spin_t unexpected_op_queue_lock;
    hg_thread_spin_t expected_op_queue_lock;
    hg_thread_spin_t retry_op_queue_lock;
    hg_thread_spin_t copy_buf_lock;
    na_bool_t no_wait;
    na_bool_t no_retry;
};

/********************/
/* Local Prototypes */
/********************/

/**
 * utility function: wrapper around getlogin().
 * Allows graceful handling of directory name generation.
 */
static char *
getlogin_safe(void);


/**
 * Open shared buf.
 */
static void *
na_sm_open_shared_buf(
    const char *name,
    size_t buf_size,
    na_bool_t create
    );

/**
 * Close shared buf.
 */
static na_return_t
na_sm_close_shared_buf(
    const char *filename,
    void *buf,
    size_t buf_size
    );

/**
 * Create UNIX domain socket.
 */
static na_return_t
na_sm_create_sock(
    const char *pathname,
    na_bool_t na_listen,
    int *sock);

/**
 * Close socket.
 */
static na_return_t
na_sm_close_sock(
    int sock,
    const char *pathname
    );

/**
 * Clean up file.
 */
static int
na_sm_cleanup_file(
    const char *fpath,
    const struct stat *sb,
    int typeflag,
    struct FTW *ftwbuf
    );

/**
 * Clean up shm segment.
 */
static int
na_sm_cleanup_shm(
    const char *fpath,
    const struct stat *sb,
    int typeflag,
    struct FTW *ftwbuf
    );

#ifndef HG_UTIL_HAS_SYSEVENTFD_H

/**
 * Create event using named pipe.
 */
static int
na_sm_event_create(
    const char *filename
    );

/**
 * Destroy event.
 */
static na_return_t
na_sm_event_destroy(
    const char *filename,
    int fd
    );

/**
 * Set event.
 */
static na_return_t
na_sm_event_set(
    int fd
    );

/**
 * Get event.
 */
static na_return_t
na_sm_event_get(
    int fd,
    na_bool_t *signaled
    );

#endif

/**
 * Register addr to poll set.
 */
static na_return_t
na_sm_poll_register(
    na_class_t *na_class,
    na_sm_poll_type_t poll_type,
    struct na_sm_addr *na_sm_addr
    );

/**
 * Deregister addr from poll set.
 */
static na_return_t
na_sm_poll_deregister(
    na_class_t *na_class,
    na_sm_poll_type_t poll_type,
    struct na_sm_addr *na_sm_addr
    );

/**
 * Create copy buf and sock and register self address.
 */
static na_return_t na_sm_setup_shm(
    na_class_t *na_class,
    struct na_sm_addr *na_sm_addr
    );

/**
 * Send addr info.
 */
static na_return_t
na_sm_send_addr_info(
    na_class_t *na_class,
    struct na_sm_addr *na_sm_addr
    );

/**
 * Recv addr info.
 */
static na_return_t
na_sm_recv_addr_info(
    struct na_sm_addr *na_sm_addr,
    na_bool_t *received
    );

/**
 * Send connection ID.
 */
static na_return_t
na_sm_send_conn_id(
    struct na_sm_addr *na_sm_addr
    );

/**
 * Recv connection ID.
 */
static na_return_t
na_sm_recv_conn_id(
    struct na_sm_addr *na_sm_addr,
    na_bool_t *received
    );

/**
 * Initialize ring buffer.
 */
static void
na_sm_ring_buf_init(
    struct na_sm_ring_buf *na_sm_ring_buf
    );

/**
 * Multi-producer safe lock-free ring buffer enqueue.
 */
static NA_INLINE na_bool_t
na_sm_ring_buf_push(
    struct na_sm_ring_buf *na_sm_ring_buf,
    na_sm_cacheline_hdr_t na_sm_hdr
    );

/**
 * Multi-consumer dequeue.
 */
static NA_INLINE na_bool_t
na_sm_ring_buf_pop(
    struct na_sm_ring_buf *na_sm_ring_buf,
    na_sm_cacheline_hdr_t *na_sm_hdr_ptr
    );

/**
 * Check whether queue is empty.
 */
static NA_INLINE na_bool_t
na_sm_ring_buf_is_empty(
    struct na_sm_ring_buf *na_sm_ring_buf
    );

/**
 * Reserve shared copy buf.
 */
static NA_INLINE na_return_t
na_sm_reserve_and_copy_buf(
    na_class_t *na_class,
    struct na_sm_copy_buf *na_sm_copy_buf,
    const void *buf,
    size_t buf_size,
    unsigned int *idx_reserved
    );

/**
 * Free and copy buf.
 */
static NA_INLINE void
na_sm_copy_and_free_buf(
    na_class_t *na_class,
    struct na_sm_copy_buf *na_sm_copy_buf,
    void *buf,
    size_t buf_size,
    unsigned int idx_reserved
    );

/**
 * Release shared copy buf.
 */
static NA_INLINE void
na_sm_release_buf(
    struct na_sm_copy_buf *na_sm_copy_buf,
    unsigned int idx_reserved
    );

/**
 * Insert message header into ring buffer.
 */
static na_return_t
na_sm_msg_insert(
    na_class_t *na_class,
    struct na_sm_op_id *na_sm_op_id,
    unsigned int idx_reserved
    );

/**
 * Translate offset from mem_handle into usable iovec.
 */
static void
na_sm_offset_translate(
    struct na_sm_mem_handle *mem_handle,
    na_offset_t offset,
    na_size_t length,
    struct iovec *iov,
    unsigned long *iovcnt
    );

/**
 * Progress callback.
 */
static int
na_sm_progress_cb(
    void *arg,
    int error,
    hg_util_bool_t *progressed
    );

/**
 * Progress error.
 */
static na_return_t
na_sm_progress_error(
    na_class_t *na_class,
    struct na_sm_addr *poll_addr
    );

/**
 * Progress on accept.
 */
static na_return_t
na_sm_progress_accept(
    na_class_t *na_class,
    struct na_sm_addr *poll_addr,
    na_bool_t *progressed
    );

/**
 * Progress on socket.
 */
static na_return_t
na_sm_progress_sock(
    na_class_t *na_class,
    struct na_sm_addr *poll_addr,
    na_bool_t *progressed
    );

/**
 * Progress on notifications.
 */
static na_return_t
na_sm_progress_notify(
    na_class_t *na_class,
    struct na_sm_addr *poll_addr,
    na_bool_t *progressed
    );

/**
 * Progress on unexpected messages.
 */
static na_return_t
na_sm_progress_unexpected(
    na_class_t *na_class,
    struct na_sm_addr *poll_addr,
    na_sm_cacheline_hdr_t na_sm_hdr
    );

/**
 * Progress on expected messages.
 */
static na_return_t
na_sm_progress_expected(
    na_class_t *na_class,
    struct na_sm_addr *poll_addr,
    na_sm_cacheline_hdr_t na_sm_hdr
    );

/**
 * Progress retries.
 */
static na_return_t
na_sm_progress_retries(
    na_class_t *na_class
    );

/**
 * Complete operation.
 */
static na_return_t
na_sm_complete(
    struct na_sm_op_id *na_sm_op_id
    );

/**
 * Release memory.
 */
static NA_INLINE void
na_sm_release(
    void *arg
    );

/* check_protocol */
static na_bool_t
na_sm_check_protocol(
    const char *protocol_name
    );

/* initialize */
static na_return_t
na_sm_initialize(
    na_class_t *na_class,
    const struct na_info *na_info,
    na_bool_t listen
    );

/* finalize */
static na_return_t
na_sm_finalize(
    na_class_t *na_class
    );

/* cleanup */
static void
na_sm_cleanup(
    void
    );

/* op_create */
static na_op_id_t
na_sm_op_create(
    na_class_t *na_class
    );

/* op_destroy */
static na_return_t
na_sm_op_destroy(
    na_class_t *na_class,
    na_op_id_t op_id
    );

/* addr_lookup */
static na_return_t
na_sm_addr_lookup(
    na_class_t *na_class,
    na_context_t *context,
    na_cb_t callback,
    void *arg,
    const char *name,
    na_op_id_t *op_id
    );

/* addr_free */
static na_return_t
na_sm_addr_free(
    na_class_t *na_class,
    na_addr_t addr
    );

/* addr_self */
static na_return_t
na_sm_addr_self(
    na_class_t *na_class,
    na_addr_t *addr
    );

/* addr_dup */
static na_return_t
na_sm_addr_dup(
    na_class_t *na_class,
    na_addr_t   addr,
    na_addr_t  *new_addr
    );

/* addr_cmp */
static na_bool_t
na_sm_addr_cmp(
    na_class_t *na_class,
    na_addr_t   addr1,
    na_addr_t   addr2
    );

/* addr_is_self */
static NA_INLINE na_bool_t
na_sm_addr_is_self(
    na_class_t *na_class,
    na_addr_t addr
    );

/* addr_to_string */
static na_return_t
na_sm_addr_to_string(
    na_class_t *na_class,
    char *buf,
    na_size_t *buf_size,
    na_addr_t addr
    );

/* msg_get_max_unexpected_size */
static NA_INLINE na_size_t
na_sm_msg_get_max_unexpected_size(
    const na_class_t *na_class
    );

/* msg_get_max_expected_size */
static NA_INLINE na_size_t
na_sm_msg_get_max_expected_size(
    const na_class_t *na_class
    );

/* msg_get_max_tag */
static NA_INLINE na_tag_t
na_sm_msg_get_max_tag(
    const na_class_t *na_class
    );

/* msg_send_unexpected */
static na_return_t
na_sm_msg_send_unexpected(
    na_class_t *na_class,
    na_context_t *context,
    na_cb_t callback,
    void *arg,
    const void *buf,
    na_size_t buf_size,
    void *plugin_data,
    na_addr_t dest_addr,
    na_uint8_t dest_id,
    na_tag_t tag,
    na_op_id_t *op_id
    );

/* msg_recv_unexpected */
static na_return_t
na_sm_msg_recv_unexpected(
    na_class_t *na_class,
    na_context_t *context,
    na_cb_t callback,
    void *arg,
    void *buf,
    na_size_t buf_size,
    void *plugin_data,
    na_op_id_t *op_id
    );

/* msg_send_expected */
static na_return_t
na_sm_msg_send_expected(
    na_class_t *na_class,
    na_context_t *context,
    na_cb_t callback,
    void *arg,
    const void *buf,
    na_size_t buf_size,
    void *plugin_data,
    na_addr_t dest_addr,
    na_uint8_t dest_id,
    na_tag_t tag,
    na_op_id_t *op_id
    );

/* msg_recv_expected */
static na_return_t
na_sm_msg_recv_expected(
    na_class_t *na_class,
    na_context_t *context,
    na_cb_t callback,
    void *arg,
    void *buf,
    na_size_t buf_size,
    void *plugin_data,
    na_addr_t source_addr,
    na_uint8_t source_id,
    na_tag_t tag,
    na_op_id_t *op_id
    );

/* mem_handle_create */
static na_return_t
na_sm_mem_handle_create(
    na_class_t *na_class,
    void *buf,
    na_size_t buf_size,
    unsigned long flags,
    na_mem_handle_t *mem_handle
    );

#ifdef NA_SM_HAS_CMA
/* mem_handle_create_segments */
static na_return_t
na_sm_mem_handle_create_segments(
    na_class_t *na_class,
    struct na_segment *segments,
    na_size_t segment_count,
    unsigned long flags,
    na_mem_handle_t *mem_handle
    );
#endif

/* mem_handle_free */
static na_return_t
na_sm_mem_handle_free(
    na_class_t *na_class,
    na_mem_handle_t mem_handle
    );

/* mem_handle_get_serialize_size */
static NA_INLINE na_size_t
na_sm_mem_handle_get_serialize_size(
    na_class_t *na_class,
    na_mem_handle_t mem_handle
    );

/* mem_handle_serialize */
static na_return_t
na_sm_mem_handle_serialize(
    na_class_t *na_class,
    void *buf,
    na_size_t buf_size,
    na_mem_handle_t mem_handle
    );

/* mem_handle_deserialize */
static na_return_t
na_sm_mem_handle_deserialize(
    na_class_t *na_class,
    na_mem_handle_t *mem_handle,
    const void *buf,
    na_size_t buf_size
    );

/* put */
static na_return_t
na_sm_put(
    na_class_t *na_class,
    na_context_t *context,
    na_cb_t callback,
    void *arg,
    na_mem_handle_t local_mem_handle,
    na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle,
    na_offset_t remote_offset,
    na_size_t length,
    na_addr_t remote_addr,
    na_uint8_t remote_id,
    na_op_id_t *op_id
    );

/* get */
static na_return_t
na_sm_get(
    na_class_t *na_class,
    na_context_t *context,
    na_cb_t callback,
    void *arg,
    na_mem_handle_t local_mem_handle,
    na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle,
    na_offset_t remote_offset,
    na_size_t length,
    na_addr_t remote_addr,
    na_uint8_t remote_id,
    na_op_id_t *op_id
    );

/* poll_get_fd */
static NA_INLINE int
na_sm_poll_get_fd(
    na_class_t      *na_class,
    na_context_t    *context
    );

/* poll_try_wait */
static NA_INLINE na_bool_t
na_sm_poll_try_wait(
    na_class_t      *na_class,
    na_context_t    *context
    );

/* progress */
static na_return_t
na_sm_progress(
    na_class_t *na_class,
    na_context_t *context,
    unsigned int timeout
    );

/* cancel */
static na_return_t
na_sm_cancel(
    na_class_t *na_class,
    na_context_t *context,
    na_op_id_t op_id
    );

/*******************/
/* Local Variables */
/*******************/

const struct na_class_ops NA_PLUGIN_OPS(sm) = {
    "na",                                   /* name */
    na_sm_check_protocol,                   /* check_protocol */
    na_sm_initialize,                       /* initialize */
    na_sm_finalize,                         /* finalize */
    na_sm_cleanup,                          /* cleanup */
    NULL,                                   /* context_create */
    NULL,                                   /* context_destroy */
    na_sm_op_create,                        /* op_create */
    na_sm_op_destroy,                       /* op_destroy */
    na_sm_addr_lookup,                      /* addr_lookup */
    NULL,                                   /* addr_lookup2 */
    na_sm_addr_free,                        /* addr_free */
    NULL,                                   /* addr_set_remove */
    na_sm_addr_self,                        /* addr_self */
    na_sm_addr_dup,                         /* addr_dup */
    na_sm_addr_cmp,                         /* addr_cmp */
    na_sm_addr_is_self,                     /* addr_is_self */
    na_sm_addr_to_string,                   /* addr_to_string */
    NULL,                                   /* addr_get_serialize_size */
    NULL,                                   /* addr_serialize */
    NULL,                                   /* addr_deserialize */
    na_sm_msg_get_max_unexpected_size,      /* msg_get_max_unexpected_size */
    na_sm_msg_get_max_expected_size,        /* msg_get_max_expected_size */
    NULL,                                   /* msg_get_unexpected_header_size */
    NULL,                                   /* msg_get_expected_header_size */
    na_sm_msg_get_max_tag,                  /* msg_get_max_tag */
    NULL,                                   /* msg_buf_alloc */
    NULL,                                   /* msg_buf_free */
    NULL,                                   /* msg_init_unexpected */
    na_sm_msg_send_unexpected,              /* msg_send_unexpected */
    na_sm_msg_recv_unexpected,              /* msg_recv_unexpected */
    NULL,                                   /* msg_init_expected */
    na_sm_msg_send_expected,                /* msg_send_expected */
    na_sm_msg_recv_expected,                /* msg_recv_expected */
    na_sm_mem_handle_create,                /* mem_handle_create */
#ifdef NA_SM_HAS_CMA
    na_sm_mem_handle_create_segments,       /* mem_handle_create_segments */
#else
    NULL,                                   /* mem_handle_create_segments */
#endif
    na_sm_mem_handle_free,                  /* mem_handle_free */
    NULL,                                   /* mem_register */
    NULL,                                   /* mem_deregister */
    NULL,                                   /* mem_publish */
    NULL,                                   /* mem_unpublish */
    na_sm_mem_handle_get_serialize_size,    /* mem_handle_get_serialize_size */
    na_sm_mem_handle_serialize,             /* mem_handle_serialize */
    na_sm_mem_handle_deserialize,           /* mem_handle_deserialize */
    na_sm_put,                              /* put */
    na_sm_get,                              /* get */
    na_sm_poll_get_fd,                      /* poll_get_fd */
    na_sm_poll_try_wait,                    /* poll_try_wait */
    na_sm_progress,                         /* progress */
    na_sm_cancel                            /* cancel */
};

/********************/
/* Plugin callbacks */
/********************/

/* Debug information */
#ifdef NA_HAS_DEBUG
static char *
itoa(uint64_t val, int base)
{
    static char buf[64] = {0};
    int i = 62;

    for (; val && i; --i, val /= (uint64_t) base)
        buf[i] = "0123456789abcdef"[val % (uint64_t) base];

    return &buf[i + 1];
}
#endif

/*---------------------------------------------------------------------------*/
static char *
getlogin_safe(void)
{
    struct passwd *passwd;

    /* statically allocated */
    passwd = getpwuid(getuid());

    return passwd ? passwd->pw_name : "unknown";
}

/*---------------------------------------------------------------------------*/
static void *
na_sm_open_shared_buf(const char *name, size_t buf_size, na_bool_t create)
{
    na_size_t page_size = (na_size_t) hg_mem_get_page_size();
    void *ret = NULL;

    /* Check alignment */
    NA_CHECK_WARNING(buf_size / page_size * page_size != buf_size,
        "Not aligned properly, page size=%zu bytes, buf size=%zu bytes",
        page_size, buf_size);

    ret = hg_mem_shm_map(name, buf_size, create);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_close_shared_buf(const char *filename, void *buf, size_t buf_size)
{
    return hg_mem_shm_unmap(filename, buf, buf_size);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_create_sock(const char *pathname, na_bool_t na_listen, int *sock)
{
    struct sockaddr_un addr;
    char *dup_path = NULL;
    na_return_t ret = NA_SUCCESS;
    int fd = -1, rc;

    /* Create a non-blocking socket so that we can poll for incoming connections */
#ifdef SOCK_NONBLOCK
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
#else
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    NA_CHECK_ERROR(fd == -1, error, ret, NA_PROTOCOL_ERROR,
        "socket() failed (%s)", strerror(errno));

#ifndef SOCK_NONBLOCK
    rc = fcntl(fd, F_SETFL, O_NONBLOCK);
    NA_CHECK_ERROR(rc == -1, error, ret, NA_PROTOCOL_ERROR,
        "fcntl() failed (%s)", strerror(errno));
#endif

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    NA_CHECK_ERROR(strlen(pathname) + strlen("/sock") > sizeof(addr.sun_path) - 1,
        error, ret, NA_OVERFLOW, "Exceeds maximum AF UNIX socket path length");
    strcpy(addr.sun_path, pathname);
    strcat(addr.sun_path, "/sock");

    if (na_listen) {
        char stat_path[NA_SM_MAX_FILENAME];
        char *path_ptr;

        dup_path = strdup(pathname);
        NA_CHECK_ERROR(dup_path == NULL, error, ret, NA_NOMEM,
            "Could not dup pathname");
        path_ptr = dup_path;

        memset(stat_path, '\0', NA_SM_MAX_FILENAME);
        if (dup_path[0] == '/') {
            path_ptr++;
            stat_path[0] = '/';
        }

        /* Create path */
        while (path_ptr) {
            struct stat sb;
            char *current = strtok_r(path_ptr, "/", &path_ptr);
            if (!current)
                break;

            strcat(stat_path, current);
            if (stat(stat_path, &sb) == -1) {
                rc = mkdir(stat_path, 0775);
                NA_CHECK_ERROR(rc == -1 && errno != EEXIST, error, ret,
                    NA_PROTOCOL_ERROR, "Could not create directory: %s (%s)",
                    stat_path, strerror(errno));
            }
            strcat(stat_path, "/");
        }

        /* Bind */
        rc = bind(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
        NA_CHECK_ERROR(rc == -1, error, ret, NA_PROTOCOL_ERROR,
            "bind() failed (%s)", strerror(errno));

        /* Listen */
        rc = listen(fd, NA_SM_LISTEN_BACKLOG);
        NA_CHECK_ERROR(rc == -1, error, ret, NA_PROTOCOL_ERROR,
            "listen() failed (%s)", strerror(errno));
    } else {
        /* Connect */
        rc = connect(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
        NA_CHECK_ERROR(rc == -1, error, ret, NA_PROTOCOL_ERROR,
            "connect() failed (%s)", strerror(errno));
    }

    *sock = fd;

    free(dup_path);
    return ret;

error:
    if (fd != -1) {
        rc = close(fd);
        NA_CHECK_ERROR_DONE(rc == -1, "close() failed (%s)", strerror(errno));
    }
    free(dup_path);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_close_sock(int sock, const char *pathname)
{
    na_return_t ret = NA_SUCCESS;
    int rc;

    rc = close(sock);
    NA_CHECK_ERROR(rc == -1, done, ret, NA_PROTOCOL_ERROR,
        "close() failed (%s)", strerror(errno));

    if (pathname) {
        char dup_path[NA_SM_MAX_FILENAME];
        char *path_ptr = NULL;

        strcpy(dup_path, pathname);
        strcat(dup_path, "/sock");

        rc = unlink(dup_path);
        NA_CHECK_ERROR(rc == -1, done, ret, NA_PROTOCOL_ERROR,
            "unlink() failed (%s)", strerror(errno));

        /* Delete path */
        path_ptr = strrchr(dup_path, '/');
        while (path_ptr) {
            *path_ptr = '\0';
            if (rmdir(dup_path) == -1) {
                /* Silently ignore */
            }
            path_ptr = strrchr(dup_path, '/');
        }
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static int
na_sm_cleanup_file(const char *fpath, const struct stat NA_UNUSED *sb,
    int NA_UNUSED typeflag, struct FTW NA_UNUSED *ftwbuf)
{
    return remove(fpath);
}

/*---------------------------------------------------------------------------*/
static int
na_sm_cleanup_shm(const char *fpath, const struct stat NA_UNUSED *sb,
    int NA_UNUSED typeflag, struct FTW NA_UNUSED *ftwbuf)
{
    const char *prefix = NA_SM_SHM_PATH "/" NA_SM_SHM_PREFIX "_";
    int ret = 0;

    if (strncmp(fpath, prefix, strlen(prefix)) == 0) {
        const char *file = fpath + strlen(NA_SM_SHM_PATH "/");
        char *username = getlogin_safe();

        if (strncmp(file + strlen(NA_SM_SHM_PREFIX "_"),
            username, strlen(username)) == 0)
            ret = hg_mem_shm_unmap(file, NULL, 0);
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
#ifndef HG_UTIL_HAS_SYSEVENTFD_H

static int
na_sm_event_create(const char *filename)
{
    int fd = -1;
    int rc;

    /* Create FIFO */
    rc = mkfifo(filename, S_IRUSR | S_IWUSR);
    NA_CHECK_ERROR_NORET(rc == -1, error, "mkfifo() failed (%s)",
        strerror(errno));

    /* Open FIFO (RDWR for convenience) */
    fd = open(filename, O_RDWR);
    NA_CHECK_ERROR_NORET(fd == -1, error, "open() failed (%s)", strerror(errno));

    /* Set FIFO to be non-blocking */
    rc = fcntl(fd, F_SETFL, O_NONBLOCK);
    NA_CHECK_ERROR_NORET(rc == -1, error, "fcntl() failed (%s)",
        strerror(errno));

    return fd;

error:
    if (fd != -1) {
        rc = close(fd);
        NA_CHECK_ERROR_DONE(rc == -1, "close() failed (%s)", strerror(errno));
    }

    return -1;
}

/*---------------------------------------------------------------------------*/
na_return_t
na_sm_event_destroy(const char *filename, int fd)
{
    na_return_t ret = NA_SUCCESS;
    int rc;

    rc = close(fd);
    NA_CHECK_ERROR(rc == -1, done, ret, NA_PROTOCOL_ERROR,
        "close() failed (%s)", strerror(errno));

    if (filename) {
        rc = unlink(filename);
        NA_CHECK_ERROR(rc == -1, done, ret, NA_PROTOCOL_ERROR,
            "unlink() failed (%s)", strerror(errno));
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
na_sm_event_set(int fd)
{
    na_return_t ret = NA_SUCCESS;
    uint64_t count = 1;
    ssize_t s;

    s = write(fd, &count, sizeof(uint64_t));
    NA_CHECK_ERROR(s != sizeof(uint64_t), done, ret, NA_PROTOCOL_ERROR,
        "write() failed (%s)", strerror(errno));

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
na_sm_event_get(int fd, na_bool_t *signaled)
{
    na_return_t ret = NA_SUCCESS;
    uint64_t count = 1;
    ssize_t s;

    s = read(fd, &count, sizeof(uint64_t));
    if (s != sizeof(uint64_t)) {
        if (likely(errno == EAGAIN)) {
            *signaled = NA_FALSE;
            goto done;
        } else
            NA_GOTO_ERROR(done, ret, NA_PROTOCOL_ERROR, "read() failed (%s)",
                strerror(errno));
    }

    *signaled = NA_TRUE;

done:
    return ret;
}

#endif

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_poll_register(na_class_t *na_class, na_sm_poll_type_t poll_type,
    struct na_sm_addr *na_sm_addr)
{
    struct na_sm_poll_data *na_sm_poll_data = NULL;
    struct na_sm_poll_data **na_sm_poll_data_ptr = NULL;
    unsigned int flags = HG_POLLIN;
    int fd, rc;
    na_return_t ret = NA_SUCCESS;

    switch (poll_type) {
        case NA_SM_ACCEPT:
            fd = na_sm_addr->sock;
            na_sm_poll_data_ptr = &na_sm_addr->sock_poll_data;
            break;
        case NA_SM_SOCK:
            fd = na_sm_addr->sock;
            na_sm_poll_data_ptr = &na_sm_addr->sock_poll_data;
            break;
        case NA_SM_NOTIFY:
            fd = na_sm_addr->local_notify;
            na_sm_poll_data_ptr = &na_sm_addr->local_notify_poll_data;
            break;
        default:
            NA_GOTO_ERROR(error, ret, NA_INVALID_ARG, "Invalid poll type");
    }

    na_sm_poll_data =
        (struct na_sm_poll_data *) malloc(sizeof(struct na_sm_poll_data));
    NA_CHECK_ERROR(na_sm_poll_data == NULL, error, ret, NA_NOMEM,
        "Could not allocate NA SM poll data");
    na_sm_poll_data->na_class = na_class;
    na_sm_poll_data->type = poll_type;
    na_sm_poll_data->addr = na_sm_addr;
    *na_sm_poll_data_ptr = na_sm_poll_data;

    rc = hg_poll_add(NA_SM_CLASS(na_class)->poll_set, fd, flags,
        na_sm_progress_cb, na_sm_poll_data);
    NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, error, ret, NA_PROTOCOL_ERROR,
        "hg_poll_add() failed");

    return ret;

error:
    free(na_sm_poll_data);
    if (na_sm_poll_data_ptr)
        *na_sm_poll_data_ptr = NULL;

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_poll_deregister(na_class_t *na_class, na_sm_poll_type_t poll_type,
    struct na_sm_addr *na_sm_addr)
{
    int fd, rc;
    struct na_sm_poll_data *na_sm_poll_data = NULL;
    na_return_t ret = NA_SUCCESS;

    switch (poll_type) {
        case NA_SM_ACCEPT:
            na_sm_poll_data = na_sm_addr->sock_poll_data;
            fd = na_sm_addr->sock;
            break;
        case NA_SM_SOCK:
            na_sm_poll_data = na_sm_addr->sock_poll_data;
            fd = na_sm_addr->sock;
            break;
        case NA_SM_NOTIFY:
            na_sm_poll_data = na_sm_addr->local_notify_poll_data;
            fd = na_sm_addr->local_notify;
            break;
        default:
            NA_GOTO_ERROR(done, ret, NA_INVALID_ARG, "Invalid poll type");
    }

    rc = hg_poll_remove(NA_SM_CLASS(na_class)->poll_set, fd);
    NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
        "hg_poll_remove() failed");
    free(na_sm_poll_data);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_setup_shm(na_class_t *na_class, struct na_sm_addr *na_sm_addr)
{
    char filename[NA_SM_MAX_FILENAME], pathname[NA_SM_MAX_FILENAME];
    struct na_sm_copy_buf *na_sm_copy_buf = NULL;
    int listen_sock = -1;
    na_return_t ret = NA_SUCCESS;

    /* Create SHM buffer */
    NA_SM_GEN_SHM_NAME(filename, NA_SM_CLASS(na_class)->username, na_sm_addr);
    na_sm_copy_buf = (struct na_sm_copy_buf *) na_sm_open_shared_buf(
        filename, sizeof(struct na_sm_copy_buf), NA_TRUE);
    NA_CHECK_ERROR(na_sm_copy_buf == NULL, error, ret, NA_PROTOCOL_ERROR,
        "Could not create copy buffer");

    /* Initialize copy buf, store 1111111111...1111 */
    hg_atomic_init64(&na_sm_copy_buf->available.val, ~((hg_util_int64_t)0));
    na_sm_addr->na_sm_copy_buf = na_sm_copy_buf;

    /* Create SHM sock */
    NA_SM_GEN_SOCK_PATH(pathname, NA_SM_CLASS(na_class)->username, na_sm_addr);
    ret = na_sm_create_sock(pathname, NA_TRUE, &listen_sock);
    NA_CHECK_NA_ERROR(error, ret, "Could not create sock");
    na_sm_addr->sock = listen_sock;

    /* Add listen_sock to poll set */
    ret = na_sm_poll_register(na_class, NA_SM_ACCEPT, na_sm_addr);
    NA_CHECK_NA_ERROR(error, ret, "Could not add listen_sock to poll set");

    return ret;

error:
    if (listen_sock != -1) {
        na_sm_close_sock(listen_sock, pathname);
        na_sm_addr->sock = -1;
    }
    if (na_sm_copy_buf) {
        na_sm_close_shared_buf(filename, na_sm_copy_buf,
            sizeof(struct na_sm_copy_buf));
        na_sm_addr->na_sm_copy_buf = NULL;
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_send_addr_info(na_class_t *na_class, struct na_sm_addr *na_sm_addr)
{
    struct msghdr msg = NA_SM_MSGHDR_INITIALIZER;
    ssize_t nsend;
    struct iovec iovec[2];
    na_return_t ret = NA_SUCCESS;

    /* Send local PID / ID */
    iovec[0].iov_base = &NA_SM_CLASS(na_class)->self_addr->pid;
    iovec[0].iov_len = sizeof(pid_t);
    iovec[1].iov_base = &NA_SM_CLASS(na_class)->self_addr->id;
    iovec[1].iov_len = sizeof(unsigned int);
    msg.msg_iov = iovec;
    msg.msg_iovlen = 2;

    nsend = sendmsg(na_sm_addr->sock, &msg, 0);
    NA_CHECK_ERROR(nsend == -1, done, ret, NA_PROTOCOL_ERROR,
        "sendmsg() failed (%s)", strerror(errno));

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_recv_addr_info(struct na_sm_addr *na_sm_addr, na_bool_t *received)
{
    struct msghdr msg = NA_SM_MSGHDR_INITIALIZER;
    ssize_t nrecv;
    struct iovec iovec[2];
    na_return_t ret = NA_SUCCESS;

    /* Receive remote PID / ID */
    iovec[0].iov_base = &na_sm_addr->pid;
    iovec[0].iov_len = sizeof(pid_t);
    iovec[1].iov_base = &na_sm_addr->id;
    iovec[1].iov_len = sizeof(unsigned int);
    msg.msg_iov = iovec;
    msg.msg_iovlen = 2;

    nrecv = recvmsg(na_sm_addr->sock, &msg, 0);
    if (nrecv == -1) {
        if (likely(errno == EAGAIN)) {
            *received = NA_FALSE;
            goto done;
        } else
            NA_GOTO_ERROR(done, ret, NA_PROTOCOL_ERROR, "recvmsg() failed (%s)",
                strerror(errno));
    }

    *received = NA_TRUE;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_send_conn_id(struct na_sm_addr *na_sm_addr)
{
    struct msghdr msg = NA_SM_MSGHDR_INITIALIZER;
    struct cmsghdr *cmsg;
    /* Contains the file descriptors to pass */
    int fds[2] = {na_sm_addr->local_notify, na_sm_addr->remote_notify};
    union {
        /* ancillary data buffer, wrapped in a union in order to ensure
           it is suitably aligned */
        char buf[CMSG_SPACE(sizeof(fds))];
        struct cmsghdr align;
    } u;
    int *fdptr;
    struct iovec iovec[1];
    ssize_t nsend;
    na_return_t ret = NA_SUCCESS;

    /* Send local PID / ID */
    iovec[0].iov_base = &na_sm_addr->conn_id;
    iovec[0].iov_len = sizeof(unsigned int);
    msg.msg_iov = iovec;
    msg.msg_iovlen = 1;

    /* Send notify event descriptors as ancillary data */
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fds));

    /* Initialize the payload */
    fdptr = (int *) CMSG_DATA(cmsg);
    memcpy(fdptr, fds, sizeof(fds));

    nsend = sendmsg(na_sm_addr->sock, &msg, 0);
    NA_CHECK_ERROR(nsend == -1, done, ret, NA_PROTOCOL_ERROR,
        "sendmsg() failed (%s)", strerror(errno));

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_recv_conn_id(struct na_sm_addr *na_sm_addr, na_bool_t *received)
{
    struct msghdr msg = NA_SM_MSGHDR_INITIALIZER;
    struct cmsghdr *cmsg;
    int *fdptr;
    int fds[2];
    union {
        /* ancillary data buffer, wrapped in a union in order to ensure
           it is suitably aligned */
        char buf[CMSG_SPACE(sizeof(fds))];
        struct cmsghdr align;
    } u;
    ssize_t nrecv;
    struct iovec iovec[1];
    na_return_t ret = NA_SUCCESS;

    /* Receive remote PID / ID */
    iovec[0].iov_base = &na_sm_addr->conn_id;
    iovec[0].iov_len = sizeof(unsigned int);
    msg.msg_iov = iovec;
    msg.msg_iovlen = 1;

    /* Recv notify event descriptor as ancillary data */
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;

    nrecv = recvmsg(na_sm_addr->sock, &msg, 0);
    if (nrecv == -1) {
        if (likely(errno == EAGAIN)) {
            *received = NA_FALSE;
            goto done;
        } else
            NA_GOTO_ERROR(done, ret, NA_PROTOCOL_ERROR, "recvmsg() failed (%s)",
                strerror(errno));
    }

    *received = NA_TRUE;

    /* Retrieve ancillary data */
    cmsg = CMSG_FIRSTHDR(&msg);
    NA_CHECK_ERROR(cmsg == NULL, done, ret, NA_PROTOCOL_ERROR, "NULL cmsg");

    fdptr = (int *) CMSG_DATA(cmsg);
    memcpy(fds, fdptr ,sizeof(fds));
    /* Invert descriptors so that local is remote and remote is local */
    na_sm_addr->local_notify = fds[1];
    na_sm_addr->remote_notify = fds[0];

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static void
na_sm_ring_buf_init(struct na_sm_ring_buf *na_sm_ring_buf)
{
    struct hg_atomic_queue *hg_atomic_queue = &na_sm_ring_buf->queue;
    unsigned int count = NA_SM_NUM_BUFS;

    hg_atomic_queue->prod_size = hg_atomic_queue->cons_size = count;
    hg_atomic_queue->prod_mask = hg_atomic_queue->cons_mask = count - 1;
    hg_atomic_init32(&hg_atomic_queue->prod_head, 0);
    hg_atomic_init32(&hg_atomic_queue->cons_head, 0);
    hg_atomic_init32(&hg_atomic_queue->prod_tail, 0);
    hg_atomic_init32(&hg_atomic_queue->cons_tail, 0);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_ring_buf_push(struct na_sm_ring_buf *na_sm_ring_buf,
    na_sm_cacheline_hdr_t na_sm_hdr)
{
    int rc = hg_atomic_queue_push(&na_sm_ring_buf->queue,
        (void *) na_sm_hdr.val);

    return (likely(rc == HG_UTIL_SUCCESS)) ? NA_TRUE : NA_FALSE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_ring_buf_pop(struct na_sm_ring_buf *na_sm_ring_buf,
    na_sm_cacheline_hdr_t *na_sm_hdr_ptr)
{
    na_sm_hdr_ptr->val = (na_uint64_t) hg_atomic_queue_pop_mc(
        &na_sm_ring_buf->queue);

    return (likely(na_sm_hdr_ptr->val)) ? NA_TRUE : NA_FALSE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_ring_buf_is_empty(struct na_sm_ring_buf *na_sm_ring_buf)
{
    return hg_atomic_queue_is_empty(&na_sm_ring_buf->queue);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_return_t
na_sm_reserve_and_copy_buf(na_class_t *na_class,
    struct na_sm_copy_buf *na_sm_copy_buf, const void *buf, size_t buf_size,
    unsigned int *idx_reserved)
{
    hg_util_int64_t bits = 1LL;
    na_return_t ret = NA_AGAIN;
    unsigned int i = 0;

    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->copy_buf_lock);

    do {
        hg_util_int64_t available = hg_atomic_get64(
            &na_sm_copy_buf->available.val);
        if (!available) {
            /* Nothing available */
            break;
        }
        if ((available & bits) != bits) {
            /* Already reserved */
            hg_atomic_fence();
            i++;
            bits <<= 1;
            continue;
        }

        if (hg_atomic_cas64(&na_sm_copy_buf->available.val, available,
            available & ~bits)) {
            /* Reservation succeeded, copy buffer */
            memcpy(na_sm_copy_buf->buf[i], buf, buf_size);
            *idx_reserved = i;
            NA_LOG_DEBUG("Reserved bit index %u:\n%s", i, itoa((hg_util_uint64_t)
                hg_atomic_get64(&na_sm_copy_buf->available.val), 2));
            ret = NA_SUCCESS;
            break;
        }
        /* Can't use atomic XOR directly, if there is a race and the cas
         * fails, we should be able to pick the next one available */
    } while (i < (NA_SM_NUM_BUFS - 1));

    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->copy_buf_lock);

    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE void
na_sm_copy_and_free_buf(na_class_t *na_class,
    struct na_sm_copy_buf *na_sm_copy_buf, void *buf, size_t buf_size,
    unsigned int idx_reserved)
{
    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->copy_buf_lock);
    memcpy(buf, na_sm_copy_buf->buf[idx_reserved], buf_size);
    hg_atomic_or64(&na_sm_copy_buf->available.val, 1LL << idx_reserved);
    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->copy_buf_lock);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE void
na_sm_release_buf(struct na_sm_copy_buf *na_sm_copy_buf,
    unsigned int idx_reserved)
{
    hg_atomic_or64(&na_sm_copy_buf->available.val, 1LL << idx_reserved);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_insert(na_class_t *na_class, struct na_sm_op_id *na_sm_op_id,
    unsigned int idx_reserved)
{
    na_sm_cacheline_hdr_t na_sm_hdr;
    na_return_t ret = NA_SUCCESS;
    int rc;

    /* Post the SM send request */
    na_sm_hdr.hdr.type = na_sm_op_id->completion_data.callback_info.type;
    na_sm_hdr.hdr.buf_idx = idx_reserved & 0xff;
    na_sm_hdr.hdr.buf_size = na_sm_op_id->info.msg.buf_size & 0xffff;
    na_sm_hdr.hdr.tag = na_sm_op_id->info.msg.tag;
    rc = (int) na_sm_ring_buf_push(na_sm_op_id->na_sm_addr->na_sm_send_ring_buf,
        na_sm_hdr);
    NA_CHECK_ERROR(rc == NA_FALSE, done, ret, NA_PROTOCOL_ERROR,
        "Full ring buffer");

    if (!NA_SM_CLASS(na_class)->no_wait) {
        /* Notify remote */
#ifdef HG_UTIL_HAS_SYSEVENTFD_H
        rc = hg_event_set(na_sm_op_id->na_sm_addr->remote_notify);
        NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
            "Could not send completion notification");
#else
        ret = na_sm_event_set(na_sm_op_id->na_sm_addr->remote_notify);
        NA_CHECK_NA_ERROR(done, ret, "Could not send completion notification");
#endif
    }

    /* Immediate completion, add directly to completion queue. */
    ret = na_sm_complete(na_sm_op_id);
    NA_CHECK_NA_ERROR(done, ret, "Could not complete operation");

    if (!NA_SM_CLASS(na_class)->no_wait) {
        /* Notify local completion */
        rc = hg_event_set(NA_SM_CLASS(na_class)->self_addr->local_notify);
        NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
            "Could not signal local completion");
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static void
na_sm_offset_translate(struct na_sm_mem_handle *mem_handle, na_offset_t offset,
    na_size_t length, struct iovec *iov, unsigned long *iovcnt)
{
    unsigned long i, new_start_index = 0;
    na_offset_t new_offset = offset, next_offset = 0;
    na_size_t remaining_len = length;

    /* Get start index and handle offset */
    for (i = 0; i < mem_handle->iovcnt; i++) {
        next_offset += mem_handle->iov[i].iov_len;
        if (offset < next_offset) {
            new_start_index = i;
            break;
        }
        new_offset -= mem_handle->iov[i].iov_len;
    }

    iov[0].iov_base = (char *) mem_handle->iov[new_start_index].iov_base +
        new_offset;
    iov[0].iov_len = NA_SM_MIN(remaining_len,
        mem_handle->iov[new_start_index].iov_len - new_offset);
    remaining_len -= iov[0].iov_len;

    for (i = 1; remaining_len && (i < mem_handle->iovcnt - new_start_index); i++) {
        iov[i].iov_base = mem_handle->iov[i + new_start_index].iov_base;
        /* Can only transfer smallest size */
        iov[i].iov_len = NA_SM_MIN(remaining_len,
            mem_handle->iov[i + new_start_index].iov_len);

        /* Decrease remaining len from the len of data */
        remaining_len -= iov[i].iov_len;
    }

    *iovcnt = i;
}

/*---------------------------------------------------------------------------*/
static int
na_sm_progress_cb(void *arg, int error, hg_util_bool_t *progressed)
{
    na_class_t *na_class;
    struct na_sm_poll_data *na_sm_poll_data = (struct na_sm_poll_data *) arg;
    na_return_t ret;

    NA_CHECK_ERROR_NORET(na_sm_poll_data == NULL, error, "NULL SM poll data");

    na_class = na_sm_poll_data->na_class;

    if (error) {
        ret = na_sm_progress_error(na_class, na_sm_poll_data->addr);
        NA_CHECK_ERROR_NORET(ret != NA_SUCCESS, error,
            "Could not process error");
    } else switch (na_sm_poll_data->type) {
        case NA_SM_ACCEPT:
            ret = na_sm_progress_accept(na_class, na_sm_poll_data->addr,
                (hg_util_bool_t *) progressed);
            NA_CHECK_ERROR_NORET(ret != NA_SUCCESS, error,
                "Could not make progress on accept");
            break;
        case NA_SM_SOCK:
            if (na_sm_poll_data->addr != NA_SM_CLASS(na_class)->self_addr) {
                ret = na_sm_progress_sock(na_class, na_sm_poll_data->addr,
                    (hg_util_bool_t *) progressed);
                NA_CHECK_ERROR_NORET(ret != NA_SUCCESS, error,
                    "Could not make progress on sock");
            }
            break;
        case NA_SM_NOTIFY:
            ret = na_sm_progress_notify(na_class, na_sm_poll_data->addr,
                (hg_util_bool_t *) progressed);
            NA_CHECK_ERROR_NORET(ret != NA_SUCCESS, error,
                "Could not make progress on notify");
            break;
        default:
            NA_LOG_ERROR("Unknown poll data type");
            goto error;
    }

    return HG_UTIL_SUCCESS;

error:
    return HG_UTIL_FAIL;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_error(na_class_t *na_class, struct na_sm_addr *poll_addr)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(poll_addr == NA_SM_CLASS(na_class)->self_addr,
        done, ret, NA_PROTOCOL_ERROR, "Unsupported error occurred");

    /* Handle case of peer disconnection */
    ret = na_sm_addr_free(na_class, poll_addr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_accept(na_class_t *na_class, struct na_sm_addr *poll_addr,
    na_bool_t *progressed)
{
    struct na_sm_addr *na_sm_addr = NULL;
    struct na_sm_ring_buf *na_sm_ring_buf = NULL;
    char filename[NA_SM_MAX_FILENAME];
    int conn_sock, local_notify, remote_notify;
    hg_time_t now;
    double elapsed_ms;
    na_return_t ret = NA_SUCCESS;
#ifndef SOCK_NONBLOCK
    int rc;
#endif

    NA_CHECK_ERROR(poll_addr != NA_SM_CLASS(na_class)->self_addr,
        done, ret, NA_PROTOCOL_ERROR, "Unrecognized poll addr");

    /* Prevent from entering accept too often */
    hg_time_get_current(&now);
    elapsed_ms = hg_time_to_double(hg_time_subtract(now,
        NA_SM_CLASS(na_class)->last_accept_time)) * 1000.0;
    if (elapsed_ms < NA_SM_ACCEPT_INTERVAL) {
        *progressed = NA_FALSE;
        goto done;
    }
    NA_SM_CLASS(na_class)->last_accept_time = now;

#ifdef SOCK_NONBLOCK
    conn_sock = accept4(poll_addr->sock, NULL, NULL, SOCK_NONBLOCK);
#else
    conn_sock = accept(poll_addr->sock, NULL, NULL);
#endif
    if (conn_sock == -1) {
        if (likely(errno == EAGAIN)) {
            *progressed = NA_FALSE;
            goto done;
        } else
            NA_GOTO_ERROR(done, ret, NA_PROTOCOL_ERROR, "accept() failed (%s)",
                strerror(errno));
    }
#ifndef SOCK_NONBLOCK
    rc = fcntl(conn_sock, F_SETFL, O_NONBLOCK);
    NA_CHECK_ERROR(rc == -1, done, ret, NA_PROTOCOL_ERROR,
        "fcntl() failed (%s)", strerror(errno));
#endif

    /* Allocate new addr and pass it to poll set */
    na_sm_addr = (struct na_sm_addr *) malloc(sizeof(struct na_sm_addr));
    NA_CHECK_ERROR(na_sm_addr == NULL, done, ret, NA_NOMEM,
        "Could not allocate NA SM addr");

    memset(na_sm_addr, 0, sizeof(struct na_sm_addr));
    hg_atomic_init32(&na_sm_addr->ref_count, 1);
    na_sm_addr->accepted = NA_TRUE;
    na_sm_addr->na_sm_copy_buf = poll_addr->na_sm_copy_buf;
    na_sm_addr->sock = conn_sock;
    /* We need to receive addr info in sock progress */
    na_sm_addr->sock_progress = NA_SM_ADDR_INFO;

    /* Add conn_sock to poll set */
    ret = na_sm_poll_register(na_class, NA_SM_SOCK, na_sm_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not add conn_sock to poll set");

    /* Set up ring buffer pair (send/recv) for connection IDs */
    na_sm_addr->conn_id = NA_SM_CLASS(na_class)->self_addr->conn_id;
    NA_SM_GEN_RING_NAME(filename, NA_SM_SEND_NAME,
        NA_SM_CLASS(na_class)->username, NA_SM_CLASS(na_class)->self_addr);
    na_sm_ring_buf = (struct na_sm_ring_buf *) na_sm_open_shared_buf(filename,
        NA_SM_RING_BUF_SIZE, NA_TRUE);
    NA_CHECK_ERROR(na_sm_ring_buf == NULL, done, ret, NA_PROTOCOL_ERROR,
        "Could not open ring buf");

    /* Initialize ring buffer */
    na_sm_ring_buf_init(na_sm_ring_buf);
    na_sm_addr->na_sm_send_ring_buf = na_sm_ring_buf;

    NA_SM_GEN_RING_NAME(filename, NA_SM_RECV_NAME,
        NA_SM_CLASS(na_class)->username, NA_SM_CLASS(na_class)->self_addr);
    na_sm_ring_buf = (struct na_sm_ring_buf *) na_sm_open_shared_buf(filename,
        NA_SM_RING_BUF_SIZE, NA_TRUE);
    NA_CHECK_ERROR(na_sm_ring_buf == NULL, done, ret, NA_PROTOCOL_ERROR,
        "Could not open ring buf");

    /* Initialize ring buffer */
    na_sm_ring_buf_init(na_sm_ring_buf);
    na_sm_addr->na_sm_recv_ring_buf = na_sm_ring_buf;

    /* Create local signal event */
#ifdef HG_UTIL_HAS_SYSEVENTFD_H
    local_notify = hg_event_create();
    NA_CHECK_ERROR(local_notify == -1, done, ret, NA_PROTOCOL_ERROR,
        "hg_event_create() failed");
#else
    /**
     * If eventfd is not supported, we need to explicitly use named pipes in
     * this case as kqueue file descriptors cannot be exchanged through
     * ancillary data
     */
    NA_SM_GEN_FIFO_NAME(filename, NA_SM_RECV_NAME,
        NA_SM_CLASS(na_class)->username, NA_SM_CLASS(na_class)->self_addr);
    local_notify = na_sm_event_create(filename);
    NA_CHECK_ERROR(local_notify == -1, done, ret, NA_PROTOCOL_ERROR,
        "na_sm_event_create() failed");
#endif
    na_sm_addr->local_notify = local_notify;

    /* Create remote signal event */
#ifdef HG_UTIL_HAS_SYSEVENTFD_H
    remote_notify = hg_event_create();
    NA_CHECK_ERROR(remote_notify == -1, done, ret, NA_PROTOCOL_ERROR,
        "hg_event_create() failed");
#else
    /**
     * If eventfd is not supported, we need to explicitly use named pipes in
     * this case as kqueue file descriptors cannot be exchanged through
     * ancillary data
     */
    NA_SM_GEN_FIFO_NAME(filename, NA_SM_SEND_NAME,
        NA_SM_CLASS(na_class)->username, NA_SM_CLASS(na_class)->self_addr);
    remote_notify = na_sm_event_create(filename);
    NA_CHECK_ERROR(remote_notify == -1, done, ret, NA_PROTOCOL_ERROR,
        "na_sm_event_create() failed");
#endif
    na_sm_addr->remote_notify = remote_notify;

    /* Add local notify to poll set */
    ret = na_sm_poll_register(na_class, NA_SM_NOTIFY, na_sm_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not add notify to poll set");

    /* Send connection ID / event IDs */
    ret = na_sm_send_conn_id(na_sm_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not send connection ID");

    /* Increment connection ID */
    NA_SM_CLASS(na_class)->self_addr->conn_id++;

    /* Push the addr to accepted addr queue so that we can free it later */
    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->accepted_addr_queue_lock);
    HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->accepted_addr_queue, na_sm_addr,
        entry);
    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->accepted_addr_queue_lock);

    *progressed = NA_TRUE;

done:
    return ret;

//error:
// TODO
//    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_sock(na_class_t *na_class, struct na_sm_addr *poll_addr,
    na_bool_t *progressed)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(poll_addr == NA_SM_CLASS(na_class)->self_addr,
        done, ret, NA_PROTOCOL_ERROR, "Unrecognized poll addr");

    switch (poll_addr->sock_progress) {
        case NA_SM_ADDR_INFO: {
            na_bool_t received = NA_FALSE;

            /* Receive addr info (PID / ID) */
            ret = na_sm_recv_addr_info(poll_addr, &received);
            NA_CHECK_NA_ERROR(done, ret, "Could not recv addr info");
            if (!received) {
                *progressed = NA_FALSE;
                goto done;
            }

            poll_addr->sock_progress = NA_SM_SOCK_DONE;

            /* Add addr to poll addr queue */
            hg_thread_spin_lock(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);
            HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->poll_addr_queue,
                poll_addr, poll_entry);
            hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);

            /* Progressed */
            *progressed = NA_TRUE;
        }
        break;
        case NA_SM_CONN_ID: {
            char filename[NA_SM_MAX_FILENAME];
            struct na_sm_ring_buf *na_sm_ring_buf;
            struct na_sm_op_id *na_sm_op_id = NULL;
            na_bool_t received = NA_FALSE;

            /* Receive connection ID / event IDs */
            ret = na_sm_recv_conn_id(poll_addr, &received);
            NA_CHECK_NA_ERROR(done, ret, "Could not recv connection ID");
            if (!received) {
                *progressed = NA_FALSE;
                goto done;
            }
            poll_addr->sock_progress = NA_SM_SOCK_DONE;

            /* Find op ID that corresponds to addr */
            hg_thread_spin_lock(&NA_SM_CLASS(na_class)->lookup_op_queue_lock);
            HG_QUEUE_FOREACH(na_sm_op_id,
                &NA_SM_CLASS(na_class)->lookup_op_queue, entry) {
                if (na_sm_op_id->na_sm_addr == poll_addr) {
                    HG_QUEUE_REMOVE(&NA_SM_CLASS(na_class)->lookup_op_queue,
                        na_sm_op_id, na_sm_op_id, entry);
                    break;
                }
            }
            hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->lookup_op_queue_lock);

            NA_CHECK_ERROR(na_sm_op_id == NULL, done, ret, NA_PROTOCOL_ERROR,
                "Could not find lookup op ID, conn ID=%u, PID=%u",
                poll_addr->conn_id, (unsigned int) poll_addr->pid);

            /* Open remote ring buf pair (send and recv names correspond to
             * remote ring buffer pair) */
            NA_SM_GEN_RING_NAME(filename, NA_SM_RECV_NAME,
                NA_SM_CLASS(na_class)->username, poll_addr);
            na_sm_ring_buf = (struct na_sm_ring_buf *) na_sm_open_shared_buf(
                filename, NA_SM_RING_BUF_SIZE, NA_FALSE);
            NA_CHECK_ERROR(na_sm_ring_buf == NULL, done, ret, NA_PROTOCOL_ERROR,
                "Could not open ring buf");
            poll_addr->na_sm_send_ring_buf = na_sm_ring_buf;

            NA_SM_GEN_RING_NAME(filename, NA_SM_SEND_NAME,
                NA_SM_CLASS(na_class)->username, poll_addr);
            na_sm_ring_buf = (struct na_sm_ring_buf *) na_sm_open_shared_buf(
                filename, NA_SM_RING_BUF_SIZE, NA_FALSE);
            NA_CHECK_ERROR(na_sm_ring_buf == NULL, done, ret, NA_PROTOCOL_ERROR,
                "Could not open ring buf");
            poll_addr->na_sm_recv_ring_buf = na_sm_ring_buf;

            /* Add received local notify to poll set */
            ret = na_sm_poll_register(na_class, NA_SM_NOTIFY, poll_addr);
            NA_CHECK_NA_ERROR(done, ret, "Could not add notify to poll set");

            /* Add addr to poll addr queue */
            hg_thread_spin_lock(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);
            HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->poll_addr_queue,
                poll_addr, poll_entry);
            hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);

            /* Completion */
            ret = na_sm_complete(na_sm_op_id);
            NA_CHECK_NA_ERROR(done, ret, "Could not complete operation");

            /* Progressed */
            *progressed = NA_TRUE;
        }
        break;
        default:
            /* TODO Silently ignore, no progress */
            *progressed = NA_FALSE;
            break;
    }

done:
    return ret;

//error:
// TODO
//    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_notify(na_class_t *na_class, struct na_sm_addr *poll_addr,
    na_bool_t *progressed)
{
    na_sm_cacheline_hdr_t na_sm_hdr;
    na_return_t ret = NA_SUCCESS;
    int rc;

    if (poll_addr == NA_SM_CLASS(na_class)->self_addr) {
        /* Local notification */
        if (!NA_SM_CLASS(na_class)->no_wait) {
            rc = hg_event_get(poll_addr->local_notify,
                (hg_util_bool_t *) progressed);
            NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
                "Could not get completion notification");
        } else
            *progressed = NA_FALSE;
        goto done;
    }

    /* Remote notification */
    if (!NA_SM_CLASS(na_class)->no_wait) {
        na_bool_t notified = NA_FALSE;

#ifdef HG_UTIL_HAS_SYSEVENTFD_H
        rc = hg_event_get(poll_addr->local_notify,
            (hg_util_bool_t *) &notified);
        NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
            "Could not get completion notification");
#else
        ret = na_sm_event_get(poll_addr->local_notify, &notified);
        NA_CHECK_NA_ERROR(done, ret, "Could not get completion notification");
#endif
        if (!notified) {
            *progressed = NA_FALSE;
            goto done;
        }
    }

    if (!na_sm_ring_buf_pop(poll_addr->na_sm_recv_ring_buf, &na_sm_hdr)) {
        *progressed = NA_FALSE;
        goto done;
    }

    /* Progress expected and unexpected messages */
    switch (na_sm_hdr.hdr.type) {
        case NA_CB_SEND_UNEXPECTED:
            ret = na_sm_progress_unexpected(na_class, poll_addr, na_sm_hdr);
            NA_CHECK_NA_ERROR(done, ret,
                "Could not make progress on unexpected msg");
            break;
        case NA_CB_SEND_EXPECTED:
            ret = na_sm_progress_expected(na_class, poll_addr, na_sm_hdr);
            NA_CHECK_NA_ERROR(done, ret,
                "Could not make progress on expected msg");
            break;
        default:
            NA_GOTO_ERROR(done, ret, NA_PROTOCOL_ERROR,
                "Unknown type of operation");
    }

    /* Progress retries */
    if (!NA_SM_CLASS(na_class)->no_retry) {
        ret = na_sm_progress_retries(na_class);
        NA_CHECK_NA_ERROR(done, ret, "Could not make progress on retried msgs");
    }

    *progressed = NA_TRUE;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_unexpected(na_class_t *na_class, struct na_sm_addr *poll_addr,
    na_sm_cacheline_hdr_t na_sm_hdr)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    na_return_t ret = NA_SUCCESS;

    /* Pop op ID from queue */
    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->unexpected_op_queue_lock);
    na_sm_op_id = HG_QUEUE_FIRST(&NA_SM_CLASS(na_class)->unexpected_op_queue);
    HG_QUEUE_POP_HEAD(&NA_SM_CLASS(na_class)->unexpected_op_queue, entry);
    hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->unexpected_op_queue_lock);

    if (likely(na_sm_op_id)) {
        /* Fill info */
        na_sm_op_id->na_sm_addr = poll_addr;
        hg_atomic_incr32(&na_sm_op_id->na_sm_addr->ref_count);
        na_sm_op_id->info.msg.actual_buf_size =
            (na_size_t) na_sm_hdr.hdr.buf_size;
        na_sm_op_id->info.msg.tag = (na_tag_t) na_sm_hdr.hdr.tag;

        /* Copy and free buffer atomically */
        na_sm_copy_and_free_buf(na_class, poll_addr->na_sm_copy_buf,
            na_sm_op_id->info.msg.buf.ptr, na_sm_hdr.hdr.buf_size,
            na_sm_hdr.hdr.buf_idx);

        /* Complete operation */
        ret = na_sm_complete(na_sm_op_id);
        NA_CHECK_NA_ERROR(done, ret, "Could not complete operation");
    } else {
        struct na_sm_unexpected_info *na_sm_unexpected_info = NULL;

        /* If no error and message arrived, keep a copy of the struct in
         * the unexpected message queue (should rarely happen) */
        na_sm_unexpected_info = (struct na_sm_unexpected_info *) malloc(
            sizeof(struct na_sm_unexpected_info));
        NA_CHECK_ERROR(na_sm_unexpected_info == NULL, done, ret, NA_NOMEM,
            "Could not allocate unexpected info");

        na_sm_unexpected_info->na_sm_addr = poll_addr;
        na_sm_unexpected_info->buf_size = (na_size_t) na_sm_hdr.hdr.buf_size;
        na_sm_unexpected_info->tag = (na_tag_t) na_sm_hdr.hdr.tag;

        /* Allocate buf */
        na_sm_unexpected_info->buf = malloc(na_sm_unexpected_info->buf_size);
        NA_CHECK_ERROR(na_sm_unexpected_info->buf == NULL, done, ret, NA_NOMEM,
            "Could not allocate na_sm_unexpected_info buf");

        /* Copy and free buffer atomically */
        na_sm_copy_and_free_buf(na_class, poll_addr->na_sm_copy_buf,
            na_sm_unexpected_info->buf, na_sm_hdr.hdr.buf_size,
            na_sm_hdr.hdr.buf_idx);

        /* Otherwise push the unexpected message into our unexpected queue so
         * that we can treat it later when a recv_unexpected is posted */
        hg_thread_spin_lock(&NA_SM_CLASS(na_class)->unexpected_msg_queue_lock);
        HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->unexpected_msg_queue,
            na_sm_unexpected_info, entry);
        hg_thread_spin_unlock(
            &NA_SM_CLASS(na_class)->unexpected_msg_queue_lock);
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_expected(na_class_t *na_class, struct na_sm_addr *poll_addr,
    na_sm_cacheline_hdr_t na_sm_hdr)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    na_return_t ret = NA_SUCCESS;

    hg_thread_spin_lock(
        &NA_SM_CLASS(na_class)->expected_op_queue_lock);
    HG_QUEUE_FOREACH(na_sm_op_id,
        &NA_SM_CLASS(na_class)->expected_op_queue, entry) {
        if (na_sm_op_id->na_sm_addr == poll_addr
            && na_sm_op_id->info.msg.tag == na_sm_hdr.hdr.tag) {
            HG_QUEUE_REMOVE(&NA_SM_CLASS(na_class)->expected_op_queue,
                na_sm_op_id, na_sm_op_id, entry);
            hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
            break;
        }
    }
    hg_thread_spin_unlock(
        &NA_SM_CLASS(na_class)->expected_op_queue_lock);

    NA_CHECK_ERROR(na_sm_op_id == NULL, done, ret, NA_INVALID_ARG,
        "Invalid operation ID");
    /* Cannot have an already completed operation ID, TODO add sanity check */

    na_sm_op_id->info.msg.actual_buf_size = na_sm_hdr.hdr.buf_size;

    /* Copy and free buffer atomically */
    na_sm_copy_and_free_buf(na_class, poll_addr->na_sm_copy_buf,
        na_sm_op_id->info.msg.buf.ptr, na_sm_hdr.hdr.buf_size,
        na_sm_hdr.hdr.buf_idx);

    /* Complete operation */
    ret = na_sm_complete(na_sm_op_id);
    NA_CHECK_NA_ERROR(done, ret, "Could not complete operation");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_retries(na_class_t *na_class)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    unsigned int idx_reserved;
    na_return_t ret = NA_SUCCESS;

    do {
        na_bool_t canceled = NA_FALSE;

        hg_thread_spin_lock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);
        na_sm_op_id = HG_QUEUE_FIRST(&NA_SM_CLASS(na_class)->retry_op_queue);
        hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);

        if (!na_sm_op_id)
            break;

        NA_LOG_DEBUG("Attempting to retry %p", na_sm_op_id);

        /* Try to reserve buffer atomically */
        if (na_sm_reserve_and_copy_buf(na_class,
            na_sm_op_id->na_sm_addr->na_sm_copy_buf,
            na_sm_op_id->info.msg.buf.const_ptr,
            na_sm_op_id->info.msg.buf_size, &idx_reserved) == NA_AGAIN)
            break;

        /* Successfully reserved a buffer */
        hg_thread_spin_lock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);
        if ((hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_CANCELED)) {
            canceled = NA_TRUE;
            na_sm_release_buf(na_sm_op_id->na_sm_addr->na_sm_copy_buf,
                idx_reserved);
        } else {
            HG_QUEUE_REMOVE(&NA_SM_CLASS(na_class)->retry_op_queue,
                na_sm_op_id, na_sm_op_id, entry);
            hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
        }
        hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);

        if (!canceled) {
            /* Insert message into ring buffer (complete OP ID) */
            ret = na_sm_msg_insert(na_class, na_sm_op_id, idx_reserved);
            NA_CHECK_NA_ERROR(error, ret, "Could not insert message");
        }
    } while (1);

    return ret;

error:
    na_sm_release_buf(na_sm_op_id->na_sm_addr->na_sm_copy_buf, idx_reserved);
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_complete(struct na_sm_op_id *na_sm_op_id)
{
    struct na_cb_info *callback_info = NULL;
    na_bool_t canceled = NA_FALSE;
    na_return_t ret = NA_SUCCESS;

    /* Mark op id as completed before checking for cancelation */
    if (hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_COMPLETED)
        & NA_SM_OP_CANCELED) {
        /* If it was canceled while being processed, set callback ret accordingly */
        NA_LOG_DEBUG("Operation ID %p was canceled", na_sm_op_id);
        canceled = NA_TRUE;
    }

    /* Init callback info */
    callback_info = &na_sm_op_id->completion_data.callback_info;
    callback_info->ret = (canceled) ? NA_CANCELED : ret;

    switch (callback_info->type) {
        case NA_CB_LOOKUP:
            callback_info->info.lookup.addr =
                (na_addr_t) na_sm_op_id->na_sm_addr;
            break;
        case NA_CB_SEND_UNEXPECTED:
            break;
        case NA_CB_RECV_UNEXPECTED:
            if (canceled) {
                /* In case of cancellation where no recv'd data */
                callback_info->info.recv_unexpected.actual_buf_size = 0;
                callback_info->info.recv_unexpected.source = NA_ADDR_NULL;
                callback_info->info.recv_unexpected.tag = 0;
            } else {
                /* Increment addr ref count */
                hg_atomic_incr32(&na_sm_op_id->na_sm_addr->ref_count);

                /* Fill callback info */
                callback_info->info.recv_unexpected.actual_buf_size =
                    na_sm_op_id->info.msg.actual_buf_size;
                callback_info->info.recv_unexpected.source =
                    (na_addr_t) na_sm_op_id->na_sm_addr;
                callback_info->info.recv_unexpected.tag =
                    na_sm_op_id->info.msg.tag;
            }
            break;
        case NA_CB_SEND_EXPECTED:
            break;
        case NA_CB_RECV_EXPECTED:
            break;
        case NA_CB_PUT:
            break;
        case NA_CB_GET:
            break;
        default:
            NA_GOTO_ERROR(done, ret, NA_INVALID_ARG,
                "Operation type %d not supported", callback_info->type);
    }

    /* Add OP to NA completion queue */
    ret = na_cb_completion_add(na_sm_op_id->context,
        &na_sm_op_id->completion_data);
    NA_CHECK_NA_ERROR(done, ret, "Could not add callback to completion queue");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE void
na_sm_release(void *arg)
{
    struct na_sm_op_id *na_sm_op_id = (struct na_sm_op_id *) arg;

    NA_CHECK_WARNING(na_sm_op_id
        && (!(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED)),
        "Releasing resources from an uncompleted operation");

    if (na_sm_op_id->na_sm_addr) {
        na_sm_addr_free(na_sm_op_id->na_class, na_sm_op_id->na_sm_addr);
        na_sm_op_id->na_sm_addr = NULL;
    }
    na_sm_op_destroy(na_sm_op_id->na_class, na_sm_op_id);
}

/********************/
/* Plugin callbacks */
/********************/

static na_bool_t
na_sm_check_protocol(const char *protocol_name)
{
    na_bool_t accept = NA_FALSE;

    if (!strcmp("sm", protocol_name))
        accept = NA_TRUE;

    return accept;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_initialize(na_class_t *na_class, const struct na_info NA_UNUSED *na_info,
    na_bool_t listen)
{
    static hg_atomic_int32_t id = HG_ATOMIC_VAR_INIT(0);
    struct na_sm_addr *na_sm_addr = NULL;
    pid_t pid;
    char *username = NULL;
    hg_poll_set_t *poll_set;
    na_bool_t no_wait = NA_FALSE, no_retry = NA_FALSE;
    int local_notify;
    na_return_t ret = NA_SUCCESS;

    /* TODO parse host name */

    /* Get init info */
    if (na_info->na_init_info) {
        /* Progress mode */
        if (na_info->na_init_info->progress_mode & NA_NO_BLOCK)
            no_wait = NA_TRUE;
        if (na_info->na_init_info->progress_mode & NA_NO_RETRY)
            no_retry = NA_TRUE;
    }

    /* Get PID */
    pid = getpid();

    /* Get username */
    username = getlogin_safe();
    NA_CHECK_ERROR(username == NULL, done, ret, NA_PROTOCOL_ERROR,
        "Could not query login name");

    /* Initialize errno */
    errno = 0;

    /* Initialize private data */
    na_class->plugin_class = malloc(sizeof(struct na_sm_class));
    NA_CHECK_ERROR(na_class->plugin_class == NULL, done, ret, NA_NOMEM,
        "Could not allocate NA private data class");
    memset(na_class->plugin_class, 0, sizeof(struct na_sm_class));
    NA_SM_CLASS(na_class)->no_wait = no_wait;
    NA_SM_CLASS(na_class)->no_retry = no_retry;

    /* Copy username */
    NA_SM_CLASS(na_class)->username = strdup(username);
    NA_CHECK_ERROR(NA_SM_CLASS(na_class)->username == NULL, done, ret, NA_NOMEM,
        "Could not dup username");

    /* Create poll set to wait for events */
    poll_set = hg_poll_create();
    NA_CHECK_ERROR(poll_set == NULL, done, ret, NA_PROTOCOL_ERROR,
        "Cannot create poll set");
    NA_SM_CLASS(na_class)->poll_set = poll_set;

    /* Create self addr */
    na_sm_addr = (struct na_sm_addr *) malloc(sizeof(struct na_sm_addr));
    NA_CHECK_ERROR(na_sm_addr == NULL, done, ret, NA_NOMEM,
        "Could not allocate NA SM addr");
    memset(na_sm_addr, 0, sizeof(struct na_sm_addr));
    na_sm_addr->pid = pid;
    na_sm_addr->id = (unsigned int) hg_atomic_incr32(&id) - 1;
    na_sm_addr->self = NA_TRUE;
    hg_atomic_init32(&na_sm_addr->ref_count, 1);
    /* If we're listening, create a new shm region */
    if (listen) {
        ret = na_sm_setup_shm(na_class, na_sm_addr);
        NA_CHECK_NA_ERROR(done, ret, "Could not setup shm");
    }
    /* Create local signal event on self address */
    local_notify = hg_event_create();
    NA_CHECK_ERROR(local_notify == -1, done, ret, NA_PROTOCOL_ERROR,
        "hg_event_create() failed");
    na_sm_addr->local_notify = local_notify;

    /* Add local notify to poll set */
    ret = na_sm_poll_register(na_class, NA_SM_NOTIFY, na_sm_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not add notify to poll set");
    NA_SM_CLASS(na_class)->self_addr = na_sm_addr;

    /* Initialize queues */
    HG_QUEUE_INIT(&NA_SM_CLASS(na_class)->accepted_addr_queue);
    HG_QUEUE_INIT(&NA_SM_CLASS(na_class)->poll_addr_queue);
    HG_QUEUE_INIT(&NA_SM_CLASS(na_class)->unexpected_msg_queue);
    HG_QUEUE_INIT(&NA_SM_CLASS(na_class)->lookup_op_queue);
    HG_QUEUE_INIT(&NA_SM_CLASS(na_class)->unexpected_op_queue);
    HG_QUEUE_INIT(&NA_SM_CLASS(na_class)->expected_op_queue);
    HG_QUEUE_INIT(&NA_SM_CLASS(na_class)->retry_op_queue);

    /* Initialize mutexes */
    hg_thread_spin_init(&NA_SM_CLASS(na_class)->accepted_addr_queue_lock);
    hg_thread_spin_init(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);
    hg_thread_spin_init(&NA_SM_CLASS(na_class)->unexpected_msg_queue_lock);
    hg_thread_spin_init(&NA_SM_CLASS(na_class)->lookup_op_queue_lock);
    hg_thread_spin_init(&NA_SM_CLASS(na_class)->unexpected_op_queue_lock);
    hg_thread_spin_init(&NA_SM_CLASS(na_class)->expected_op_queue_lock);
    hg_thread_spin_init(&NA_SM_CLASS(na_class)->retry_op_queue_lock);
    hg_thread_spin_init(&NA_SM_CLASS(na_class)->copy_buf_lock);

done:
    return ret;

//error:
// TODO
//    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_finalize(na_class_t *na_class)
{
    na_return_t ret = NA_SUCCESS;
    na_bool_t empty;
    int rc;

    if (!na_class->plugin_class)
        goto done;

    /* Check that lookup op queue is empty */
    empty = HG_QUEUE_IS_EMPTY(&NA_SM_CLASS(na_class)->lookup_op_queue);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_PROTOCOL_ERROR,
        "Lookup op queue should be empty");

    /* Check that unexpected op queue is empty */
    empty = HG_QUEUE_IS_EMPTY(&NA_SM_CLASS(na_class)->unexpected_op_queue);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_PROTOCOL_ERROR,
        "Unexpected op queue should be empty");

    /* Check that unexpected message queue is empty */
    empty = HG_QUEUE_IS_EMPTY(&NA_SM_CLASS(na_class)->unexpected_msg_queue);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_PROTOCOL_ERROR,
        "Unexpected msg queue should be empty");

    /* Check that expected op queue is empty */
    empty = HG_QUEUE_IS_EMPTY(&NA_SM_CLASS(na_class)->expected_op_queue);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_PROTOCOL_ERROR,
        "Expected op queue should be empty");

    /* Check that retry op queue is empty */
    empty = HG_QUEUE_IS_EMPTY(&NA_SM_CLASS(na_class)->retry_op_queue);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_PROTOCOL_ERROR,
        "Retry op queue should be empty");

    /* Check that accepted addr queue is empty */
    while (!HG_QUEUE_IS_EMPTY(&NA_SM_CLASS(na_class)->accepted_addr_queue)) {
        struct na_sm_addr *na_sm_addr = HG_QUEUE_FIRST(
            &NA_SM_CLASS(na_class)->accepted_addr_queue);
        ret = na_sm_addr_free(na_class, na_sm_addr);
        NA_CHECK_NA_ERROR(done, ret, "Could not free accepted addr");
    }

    /* Free self addr */
    ret = na_sm_addr_free(na_class, NA_SM_CLASS(na_class)->self_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not free self addr");

    /* Close poll set */
    rc = hg_poll_destroy(NA_SM_CLASS(na_class)->poll_set);
    NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
        "hg_poll_destroy() failed");

    /* Destroy mutexes */
    hg_thread_spin_destroy(&NA_SM_CLASS(na_class)->accepted_addr_queue_lock);
    hg_thread_spin_destroy(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);
    hg_thread_spin_destroy(&NA_SM_CLASS(na_class)->unexpected_msg_queue_lock);
    hg_thread_spin_destroy(&NA_SM_CLASS(na_class)->lookup_op_queue_lock);
    hg_thread_spin_destroy(&NA_SM_CLASS(na_class)->unexpected_op_queue_lock);
    hg_thread_spin_destroy(&NA_SM_CLASS(na_class)->expected_op_queue_lock);
    hg_thread_spin_destroy(&NA_SM_CLASS(na_class)->retry_op_queue_lock);
    hg_thread_spin_destroy(&NA_SM_CLASS(na_class)->copy_buf_lock);

    free(NA_SM_CLASS(na_class)->username);
    free(na_class->plugin_class);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static void
na_sm_cleanup(void)
{
    char pathname[NA_SM_MAX_FILENAME] = {'\0'};
    char *username = getlogin_safe();
    int ret;

    sprintf(pathname, "%s/%s_%s", NA_SM_TMP_DIRECTORY,
        NA_SM_SHM_PREFIX, username);

    /* We need to remove all files first before being able to remove the
     * directories */
    ret = nftw(pathname, na_sm_cleanup_file, NA_SM_CLEANUP_NFDS,
        FTW_PHYS | FTW_DEPTH);
    NA_CHECK_WARNING(ret != 0 && errno != ENOENT, "nftw() failed (%s)",
        strerror(errno));

    ret = nftw(NA_SM_SHM_PATH, na_sm_cleanup_shm, NA_SM_CLEANUP_NFDS,
        FTW_PHYS);
    NA_CHECK_WARNING(ret != 0 && errno != ENOENT, "nftw() failed (%s)",
        strerror(errno));
}

/*---------------------------------------------------------------------------*/
static na_op_id_t
na_sm_op_create(na_class_t *na_class)
{
    struct na_sm_op_id *na_sm_op_id = NULL;

    na_sm_op_id = (struct na_sm_op_id *) malloc(sizeof(struct na_sm_op_id));
    NA_CHECK_ERROR_NORET(na_sm_op_id == NULL, done,
        "Could not allocate NA SM operation ID");
    memset(na_sm_op_id, 0, sizeof(struct na_sm_op_id));
    na_sm_op_id->na_class = na_class;
    hg_atomic_init32(&na_sm_op_id->ref_count, 1);
    /* Completed by default */
    hg_atomic_init32(&na_sm_op_id->status, NA_SM_OP_COMPLETED);

    /* Set op ID release callbacks */
    na_sm_op_id->completion_data.plugin_callback = na_sm_release;
    na_sm_op_id->completion_data.plugin_callback_args = na_sm_op_id;

done:
    return (na_op_id_t) na_sm_op_id;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_op_destroy(na_class_t NA_UNUSED *na_class, na_op_id_t op_id)
{
    struct na_sm_op_id *na_sm_op_id = (struct na_sm_op_id *) op_id;
    na_return_t ret = NA_SUCCESS;

    if (hg_atomic_decr32(&na_sm_op_id->ref_count)) {
        /* Cannot free yet */
        goto done;
    }
    free(na_sm_op_id);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_lookup(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, const char *name, na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_addr *na_sm_addr = NULL;
    struct na_sm_copy_buf *na_sm_copy_buf = NULL;
    char filename[NA_SM_MAX_FILENAME];
    char pathname[NA_SM_MAX_FILENAME];
    int conn_sock;
    char *name_string = NULL, *short_name = NULL;
    na_return_t ret = NA_SUCCESS;

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(!(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED),
        done, ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_LOOKUP;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    na_sm_op_id->na_sm_addr = NULL;
    hg_atomic_set32(&na_sm_op_id->status, 0);

    /* Allocate addr */
    na_sm_addr = (struct na_sm_addr *) malloc(sizeof(struct na_sm_addr));
    NA_CHECK_ERROR(na_sm_addr == NULL, done, ret, NA_NOMEM,
        "Could not allocate NA SM addr");
    memset(na_sm_addr, 0, sizeof(struct na_sm_addr));
    hg_atomic_init32(&na_sm_addr->ref_count, 2); /* Extra refcount */

    na_sm_op_id->na_sm_addr = na_sm_addr;

    /**
     * Clean up name, strings can be of the format:
     *   <protocol>://<host string>
     */
    name_string = strdup(name);
    NA_CHECK_ERROR(name_string == NULL, done, ret, NA_NOMEM,
        "Could not duplicate string");

    if (strstr(name_string, ":") != NULL) {
         strtok_r(name_string, ":", &short_name);
         short_name += 2;
    } else
         short_name = name_string;

    /* Get PID / ID from name */
    sscanf(short_name, "%d/%u", &na_sm_addr->pid, &na_sm_addr->id);

    /* Open shared copy buf */
    NA_SM_GEN_SHM_NAME(filename, NA_SM_CLASS(na_class)->username, na_sm_addr);
    na_sm_copy_buf = (struct na_sm_copy_buf *) na_sm_open_shared_buf(
        filename, sizeof(struct na_sm_copy_buf), NA_FALSE);
    NA_CHECK_ERROR(na_sm_copy_buf == NULL, done, ret, NA_PROTOCOL_ERROR,
        "Could not open copy buffer");
    na_sm_addr->na_sm_copy_buf = na_sm_copy_buf;

    /* Open SHM sock */
    NA_SM_GEN_SOCK_PATH(pathname, NA_SM_CLASS(na_class)->username, na_sm_addr);
    ret = na_sm_create_sock(pathname, NA_FALSE, &conn_sock);
    NA_CHECK_NA_ERROR(done, ret, "Could not create sock");
    na_sm_addr->sock = conn_sock;
    /* We only need to receive conn ID in sock progress */
    na_sm_addr->sock_progress = NA_SM_CONN_ID;

    /* Push op ID to lookup op queue */
    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->lookup_op_queue_lock);
    HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->lookup_op_queue, na_sm_op_id,
        entry);
    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->lookup_op_queue_lock);

    /* Add conn_sock to poll set */
    ret = na_sm_poll_register(na_class, NA_SM_SOCK, na_sm_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not add conn_sock to poll set");

    /* Send addr info (PID / ID) */
    ret = na_sm_send_addr_info(na_class, na_sm_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not send addr info");

done:
    free(name_string);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_free(na_class_t *na_class, na_addr_t addr)
{
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;
    const char *copy_buf_name = NULL, *send_ring_buf_name = NULL,
        *recv_ring_buf_name = NULL, *pathname = NULL;
    char na_sm_copy_buf_name[NA_SM_MAX_FILENAME],
        na_sm_send_ring_buf_name[NA_SM_MAX_FILENAME],
        na_sm_recv_ring_buf_name[NA_SM_MAX_FILENAME],
        na_sock_name[NA_SM_MAX_FILENAME];
    na_return_t ret = NA_SUCCESS;
    int rc;

    NA_CHECK_ERROR(na_sm_addr == NULL, done, ret, NA_INVALID_ARG,
        "NULL SM addr");

    if (hg_atomic_decr32(&na_sm_addr->ref_count))
        /* Cannot free yet */
        goto done;

    if (na_sm_addr->accepted) { /* Created by accept */
        hg_thread_spin_lock(&NA_SM_CLASS(na_class)->accepted_addr_queue_lock);
        /* Remove the addr from accepted addr queue */
        HG_QUEUE_REMOVE(&NA_SM_CLASS(na_class)->accepted_addr_queue, na_sm_addr,
            na_sm_addr, entry);
        hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->accepted_addr_queue_lock);
    }

    /* Deregister event file descriptors from poll set */
    ret = na_sm_poll_deregister(na_class, NA_SM_NOTIFY, na_sm_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not delete notify from poll set");

    /* Destroy local event */
#ifdef HG_UTIL_HAS_SYSEVENTFD_H
    rc = hg_event_destroy(na_sm_addr->local_notify);
    NA_CHECK_ERROR(rc == HG_UTIL_FAIL, done, ret, NA_PROTOCOL_ERROR,
        "hg_event_destroy() failed");
#endif

    // TODO cleanup
    if (!na_sm_addr->self) { /* Created by lookup/connect or accept */
#ifndef HG_UTIL_HAS_SYSEVENTFD_H
        char na_sm_local_event_name[NA_SM_MAX_FILENAME],
            na_sm_remote_event_name[NA_SM_MAX_FILENAME];
        const char *local_event_name = NULL, *remote_event_name = NULL;
#endif

        /* Deregister sock file descriptor */
        ret = na_sm_poll_deregister(na_class, NA_SM_SOCK, na_sm_addr);
        NA_CHECK_NA_ERROR(done, ret, "Could not delete sock from poll set");

        /* Remove addr from poll addr queue */
        hg_thread_spin_lock(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);
        HG_QUEUE_REMOVE(&NA_SM_CLASS(na_class)->poll_addr_queue, na_sm_addr,
            na_sm_addr, poll_entry);
        hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);

        if (na_sm_addr->accepted) { /* Created by accept */
            /* Get file names from ring bufs / events to delete files */
            sprintf(na_sm_send_ring_buf_name, "%s_%s-%d-%d-%d-" NA_SM_SEND_NAME,
                NA_SM_SHM_PREFIX, NA_SM_CLASS(na_class)->username,
                NA_SM_CLASS(na_class)->self_addr->pid,
                NA_SM_CLASS(na_class)->self_addr->id,
                na_sm_addr->conn_id);
            sprintf(na_sm_recv_ring_buf_name, "%s_%s-%d-%d-%d-" NA_SM_RECV_NAME,
                NA_SM_SHM_PREFIX, NA_SM_CLASS(na_class)->username,
                NA_SM_CLASS(na_class)->self_addr->pid,
                NA_SM_CLASS(na_class)->self_addr->id,
                na_sm_addr->conn_id);
            send_ring_buf_name = na_sm_send_ring_buf_name;
            recv_ring_buf_name = na_sm_recv_ring_buf_name;

#ifndef HG_UTIL_HAS_SYSEVENTFD_H
            sprintf(na_sm_local_event_name, "%s/%s_%s/%d/%u/fifo-%u-%s",
                NA_SM_TMP_DIRECTORY, NA_SM_SHM_PREFIX,
                NA_SM_CLASS(na_class)->username,
                NA_SM_CLASS(na_class)->self_addr->pid,
                NA_SM_CLASS(na_class)->self_addr->id,
                na_sm_addr->conn_id, NA_SM_RECV_NAME);
            sprintf(na_sm_remote_event_name, "%s/%s_%s/%d/%u/fifo-%u-%s",
                NA_SM_TMP_DIRECTORY, NA_SM_SHM_PREFIX,
                NA_SM_CLASS(na_class)->username,
                NA_SM_CLASS(na_class)->self_addr->pid,
                NA_SM_CLASS(na_class)->self_addr->id,
                na_sm_addr->conn_id, NA_SM_SEND_NAME);
            local_event_name = na_sm_local_event_name;
            remote_event_name = na_sm_remote_event_name;
#endif
        }

        /* Destroy events */
#ifdef HG_UTIL_HAS_SYSEVENTFD_H
        rc = hg_event_destroy(na_sm_addr->remote_notify);
        NA_CHECK_ERROR(rc == HG_UTIL_FAIL, done, ret, NA_PROTOCOL_ERROR,
            "hg_event_destroy() failed");
#else
        ret = na_sm_event_destroy(local_event_name, na_sm_addr->local_notify);
        NA_CHECK_NA_ERROR(done, ret, "na_sm_event_destroy() failed");

        ret = na_sm_event_destroy(remote_event_name, na_sm_addr->remote_notify);
        NA_CHECK_NA_ERROR(done, ret, "na_sm_event_destroy() failed");
#endif
    } else {
#ifndef HG_UTIL_HAS_SYSEVENTFD_H
        /* Destroy local event */
        rc = hg_event_destroy(na_sm_addr->local_notify);
        NA_CHECK_ERROR(rc == HG_UTIL_FAIL, done, ret, NA_PROTOCOL_ERROR,
            "hg_event_destroy() failed");
#endif
        if (na_sm_addr->na_sm_copy_buf) { /* Self addr and listen */
            ret = na_sm_poll_deregister(na_class, NA_SM_ACCEPT, na_sm_addr);
            NA_CHECK_NA_ERROR(done, ret,
                "Could not delete listen from poll set");

            NA_SM_GEN_SHM_NAME(na_sm_copy_buf_name,
                NA_SM_CLASS(na_class)->username, na_sm_addr);
            copy_buf_name = na_sm_copy_buf_name;
            NA_SM_GEN_SOCK_PATH(na_sock_name,
                NA_SM_CLASS(na_class)->username, na_sm_addr);
            pathname = na_sock_name;
        }
    }

    /* Close sock (delete also tmp dir if pathname is set) */
    ret = na_sm_close_sock(na_sm_addr->sock, pathname);
    NA_CHECK_NA_ERROR(done, ret, "Could not close sock");

    /* Close ring buf (send) */
    ret = na_sm_close_shared_buf(send_ring_buf_name,
        na_sm_addr->na_sm_send_ring_buf, sizeof(struct na_sm_ring_buf));
    NA_CHECK_NA_ERROR(done, ret, "Could not close send ring buffer");

    /* Close ring buf (recv) */
    ret = na_sm_close_shared_buf(recv_ring_buf_name,
        na_sm_addr->na_sm_recv_ring_buf, sizeof(struct na_sm_ring_buf));
    NA_CHECK_NA_ERROR(done, ret, "Could not close recv ring buffer");

    /* Close copy buf */
    if (!na_sm_addr->accepted) { /* Created by accept */
        ret = na_sm_close_shared_buf(copy_buf_name, na_sm_addr->na_sm_copy_buf,
            sizeof(struct na_sm_copy_buf));
        NA_CHECK_NA_ERROR(done, ret, "Could not close copy buffer");
    }

    free(na_sm_addr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_self(na_class_t *na_class, na_addr_t *addr)
{
    struct na_sm_addr *na_sm_addr = NA_SM_CLASS(na_class)->self_addr;
    na_return_t ret = NA_SUCCESS;

    /* Increment refcount */
    hg_atomic_incr32(&na_sm_addr->ref_count);

    *addr = (na_addr_t) na_sm_addr;

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_dup(na_class_t NA_UNUSED *na_class, na_addr_t addr,
    na_addr_t *new_addr)
{
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;
    na_return_t ret = NA_SUCCESS;

    /* Increment refcount */
    hg_atomic_incr32(&na_sm_addr->ref_count);

    *new_addr = (na_addr_t) na_sm_addr;

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_bool_t
na_sm_addr_cmp(na_class_t NA_UNUSED *na_class, na_addr_t addr1, na_addr_t addr2)
{
    struct na_sm_addr *na_sm_addr1 = (struct na_sm_addr *) addr1;
    struct na_sm_addr *na_sm_addr2 = (struct na_sm_addr *) addr2;

    return (na_sm_addr1->pid == na_sm_addr2->pid)
        && (na_sm_addr1->id == na_sm_addr2->id);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_addr_is_self(na_class_t NA_UNUSED *na_class, na_addr_t addr)
{
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;

    return na_sm_addr->self;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_to_string(na_class_t NA_UNUSED *na_class, char *buf,
    na_size_t *buf_size, na_addr_t addr)
{
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;
    na_size_t string_len;
    char addr_string[NA_SM_MAX_FILENAME];
    na_return_t ret = NA_SUCCESS;

    sprintf(addr_string, "sm://%d/%u", na_sm_addr->pid, na_sm_addr->id);

    string_len = strlen(addr_string);
    if (buf) {
        NA_CHECK_ERROR(string_len >= *buf_size, done, ret, NA_OVERFLOW,
            "Buffer size too small to copy addr");
        strcpy(buf, addr_string);
    }
    *buf_size = string_len + 1;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_size_t
na_sm_msg_get_max_unexpected_size(const na_class_t NA_UNUSED *na_class)
{
    return NA_SM_UNEXPECTED_SIZE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_size_t
na_sm_msg_get_max_expected_size(const na_class_t NA_UNUSED *na_class)
{
    return NA_SM_EXPECTED_SIZE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_tag_t
na_sm_msg_get_max_tag(const na_class_t NA_UNUSED *na_class)
{
    return NA_SM_MAX_TAG;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_send_unexpected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, const void *buf, na_size_t buf_size,
    void NA_UNUSED *plugin_data, na_addr_t dest_addr,
    na_uint8_t NA_UNUSED dest_id, na_tag_t tag, na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) dest_addr;
    unsigned int idx_reserved;
    na_bool_t reserved = NA_FALSE;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size > NA_SM_UNEXPECTED_SIZE, done, ret, NA_OVERFLOW,
        "Exceeds unexpected size, %d", buf_size);

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(!(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED),
        done, ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_SEND_UNEXPECTED;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);
    /* TODO we assume that buf remains valid (safe because we pre-allocate buffers) */
    na_sm_op_id->info.msg.buf.const_ptr = buf;
    na_sm_op_id->info.msg.buf_size = buf_size;
    na_sm_op_id->info.msg.actual_buf_size = buf_size;
    na_sm_op_id->info.msg.tag = tag;

    /* Try to reserve buffer atomically */
    ret = na_sm_reserve_and_copy_buf(na_class, na_sm_addr->na_sm_copy_buf,
        buf, buf_size, &idx_reserved);
    if (unlikely(ret == NA_AGAIN)) {
        if (NA_SM_CLASS(na_class)->no_retry)
            /* Do not attempt to retry */
            NA_GOTO_DONE(error, ret, NA_AGAIN);
        else {
            NA_LOG_DEBUG("Pushing %p for retry", na_sm_op_id);

            /* Push op ID to retry queue */
            hg_thread_spin_lock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);
            HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->retry_op_queue,
                na_sm_op_id, entry);
            hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_QUEUED);
            hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);

            ret = NA_SUCCESS;
        }
    } else {
        /* Successfully reserved a buffer */
        reserved = NA_TRUE;

        /* Insert message into ring buffer (complete OP ID) */
        ret = na_sm_msg_insert(na_class, na_sm_op_id, idx_reserved);
        NA_CHECK_NA_ERROR(error, ret, "Could not insert message");
    }

done:
    return ret;

error:
    if (reserved)
        na_sm_release_buf(na_sm_op_id->na_sm_addr->na_sm_copy_buf,
            idx_reserved);
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_recv_unexpected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, void *buf, na_size_t buf_size,
    void NA_UNUSED *plugin_data, na_op_id_t *op_id)
{
    struct na_sm_unexpected_info *na_sm_unexpected_info;
    struct na_sm_op_id *na_sm_op_id = NULL;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size > NA_SM_UNEXPECTED_SIZE, done, ret, NA_OVERFLOW,
        "Exceeds unexpected size, %d", buf_size);

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(!(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED),
        done, ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_RECV_UNEXPECTED;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    na_sm_op_id->na_sm_addr = NULL;
    hg_atomic_set32(&na_sm_op_id->status, 0);
    na_sm_op_id->info.msg.buf.ptr = buf;
    na_sm_op_id->info.msg.buf_size = buf_size;

    /* Look for an unexpected message already received */
    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->unexpected_msg_queue_lock);
    na_sm_unexpected_info = HG_QUEUE_FIRST(
        &NA_SM_CLASS(na_class)->unexpected_msg_queue);
    HG_QUEUE_POP_HEAD(&NA_SM_CLASS(na_class)->unexpected_msg_queue, entry);
    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->unexpected_msg_queue_lock);
    if (unlikely(na_sm_unexpected_info)) {
        na_sm_op_id->na_sm_addr = na_sm_unexpected_info->na_sm_addr;
        hg_atomic_incr32(&na_sm_op_id->na_sm_addr->ref_count);
        na_sm_op_id->info.msg.actual_buf_size = na_sm_unexpected_info->buf_size;
        na_sm_op_id->info.msg.tag = na_sm_unexpected_info->tag;

        /* Copy buffers */
        memcpy(na_sm_op_id->info.msg.buf.ptr, na_sm_unexpected_info->buf,
            na_sm_unexpected_info->buf_size);

        free(na_sm_unexpected_info->buf);
        free(na_sm_unexpected_info);

        ret = na_sm_complete(na_sm_op_id);
        NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");
    } else {
        na_sm_op_id->info.msg.actual_buf_size = 0;
        na_sm_op_id->info.msg.tag = 0;

        /* Nothing has been received yet so add op_id to progress queue */
        hg_thread_spin_lock(&NA_SM_CLASS(na_class)->unexpected_op_queue_lock);
        HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->unexpected_op_queue,
            na_sm_op_id, entry);
        hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_QUEUED);
        hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->unexpected_op_queue_lock);
    }

done:
    return ret;

error:
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_send_expected(na_class_t NA_UNUSED *na_class, na_context_t *context,
    na_cb_t callback, void *arg, const void *buf, na_size_t buf_size,
    void NA_UNUSED *plugin_data, na_addr_t dest_addr,
    na_uint8_t NA_UNUSED dest_id, na_tag_t tag, na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) dest_addr;
    unsigned int idx_reserved;
    na_bool_t reserved = NA_FALSE;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size > NA_SM_EXPECTED_SIZE, done, ret, NA_OVERFLOW,
        "Exceeds expected size, %d", buf_size);

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(!(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED),
        done, ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_SEND_EXPECTED;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);
    /* TODO we assume that buf remains valid (safe because we pre-allocate buffers) */
    na_sm_op_id->info.msg.buf.const_ptr = buf;
    na_sm_op_id->info.msg.buf_size = buf_size;
    na_sm_op_id->info.msg.actual_buf_size = buf_size;
    na_sm_op_id->info.msg.tag = tag;

    /* Try to reserve buffer atomically */
    ret = na_sm_reserve_and_copy_buf(na_class, na_sm_addr->na_sm_copy_buf,
        buf, buf_size, &idx_reserved);
    if (unlikely(ret == NA_AGAIN)) {
        if (NA_SM_CLASS(na_class)->no_retry)
            /* Do not attempt to retry */
            NA_GOTO_DONE(error, ret, NA_AGAIN);
        else {
            NA_LOG_DEBUG("Pushing %p for retry", na_sm_op_id);

            /* Push op ID to retry queue */
            hg_thread_spin_lock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);
            HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->retry_op_queue,
                na_sm_op_id, entry);
            hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_QUEUED);
            hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);

            ret = NA_SUCCESS;
        }
    } else {
        /* Successfully reserved a buffer */
        reserved = NA_TRUE;

        /* Insert message into ring buffer (complete OP ID) */
        ret = na_sm_msg_insert(na_class, na_sm_op_id, idx_reserved);
        NA_CHECK_NA_ERROR(error, ret, "Could not insert message");
    }

done:
    return ret;

error:
    if (reserved)
        na_sm_release_buf(na_sm_op_id->na_sm_addr->na_sm_copy_buf,
            idx_reserved);
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_recv_expected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, void *buf, na_size_t buf_size,
    void NA_UNUSED *plugin_data, na_addr_t source_addr,
    na_uint8_t NA_UNUSED source_id, na_tag_t tag, na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) source_addr;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size > NA_SM_EXPECTED_SIZE, done, ret, NA_OVERFLOW,
        "Exceeds expected size, %d", buf_size);

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(!(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED),
        done, ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_RECV_EXPECTED;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);
    na_sm_op_id->info.msg.buf.ptr = buf;
    na_sm_op_id->info.msg.buf_size = buf_size;
    na_sm_op_id->info.msg.actual_buf_size = 0;
    na_sm_op_id->info.msg.tag = tag;

    /* Expected messages must always be pre-posted, therefore a message should
     * never arrive before that call returns (not completes), simply add
     * op_id to queue */
    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->expected_op_queue_lock);
    HG_QUEUE_PUSH_TAIL(&NA_SM_CLASS(na_class)->expected_op_queue, na_sm_op_id,
        entry);
    hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_QUEUED);
    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->expected_op_queue_lock);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_mem_handle_create(na_class_t NA_UNUSED *na_class, void *buf,
    na_size_t buf_size, unsigned long flags, na_mem_handle_t *mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle = NULL;
    na_return_t ret = NA_SUCCESS;

    na_sm_mem_handle = (struct na_sm_mem_handle *) malloc(
        sizeof(struct na_sm_mem_handle));
    NA_CHECK_ERROR(na_sm_mem_handle == NULL, error, ret, NA_NOMEM,
        "Could not allocate NA SM memory handle");

    na_sm_mem_handle->iov = (struct iovec *) malloc(sizeof(struct iovec));
    NA_CHECK_ERROR(na_sm_mem_handle->iov == NULL, error, ret, NA_NOMEM,
        "Could not allocate iovec");

    na_sm_mem_handle->iov->iov_base = buf;
    na_sm_mem_handle->iov->iov_len = buf_size;
    na_sm_mem_handle->iovcnt = 1;
    na_sm_mem_handle->flags = flags;
    na_sm_mem_handle->len = buf_size;

    *mem_handle = (na_mem_handle_t) na_sm_mem_handle;

    return ret;

error:
    if (na_sm_mem_handle) {
        free(na_sm_mem_handle->iov);
        free(na_sm_mem_handle);
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
#ifdef NA_SM_HAS_CMA
static na_return_t
na_sm_mem_handle_create_segments(na_class_t NA_UNUSED *na_class,
    struct na_segment *segments, na_size_t segment_count, unsigned long flags,
    na_mem_handle_t *mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle = NULL;
    na_return_t ret = NA_SUCCESS;
    na_size_t i, iov_max;

    /* Check that we do not exceed IOV_MAX */
    iov_max = (na_size_t) sysconf(_SC_IOV_MAX);
    NA_CHECK_ERROR(segment_count > iov_max, error, ret, NA_INVALID_ARG,
        "Segment count exceeds IOV_MAX limit");

    na_sm_mem_handle = (struct na_sm_mem_handle *) malloc(
        sizeof(struct na_sm_mem_handle));
    NA_CHECK_ERROR(na_sm_mem_handle == NULL, error, ret, NA_NOMEM,
        "Could not allocate NA SM memory handle");

    na_sm_mem_handle->iov = (struct iovec *) malloc(
        segment_count * sizeof(struct iovec));
    NA_CHECK_ERROR(na_sm_mem_handle->iov == NULL, error, ret, NA_NOMEM,
        "Could not allocate iovec");

    na_sm_mem_handle->len = 0;
    for (i = 0; i < segment_count; i++) {
        na_sm_mem_handle->iov[i].iov_base = (void *) segments[i].address;
        na_sm_mem_handle->iov[i].iov_len = segments[i].size;
        na_sm_mem_handle->len += na_sm_mem_handle->iov[i].iov_len;
    }
    na_sm_mem_handle->iovcnt = segment_count;
    na_sm_mem_handle->flags = flags;

    *mem_handle = (na_mem_handle_t) na_sm_mem_handle;

    return ret;

error:
    if (na_sm_mem_handle) {
        free(na_sm_mem_handle->iov);
        free(na_sm_mem_handle);
    }
    return ret;
}
#endif

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_mem_handle_free(na_class_t NA_UNUSED *na_class,
    na_mem_handle_t mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle =
        (struct na_sm_mem_handle *) mem_handle;
    na_return_t ret = NA_SUCCESS;

    free(na_sm_mem_handle->iov);
    free(na_sm_mem_handle);

    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_size_t
na_sm_mem_handle_get_serialize_size(na_class_t NA_UNUSED *na_class,
    na_mem_handle_t mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle =
        (struct na_sm_mem_handle *) mem_handle;
    unsigned long i;
    na_size_t ret = 2 * sizeof(unsigned long) + sizeof(size_t);

    for (i = 0; i < na_sm_mem_handle->iovcnt; i++)
        ret += sizeof(void *) + sizeof(size_t);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_mem_handle_serialize(na_class_t NA_UNUSED *na_class, void *buf,
    na_size_t NA_UNUSED buf_size, na_mem_handle_t mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle =
        (struct na_sm_mem_handle*) mem_handle;
    char *buf_ptr = (char *) buf;
    na_return_t ret = NA_SUCCESS;
    unsigned long i;

    /* Number of segments */
    memcpy(buf_ptr, &na_sm_mem_handle->iovcnt, sizeof(unsigned long));
    buf_ptr += sizeof(unsigned long);

    /* Flags */
    memcpy(buf_ptr, &na_sm_mem_handle->flags, sizeof(unsigned long));
    buf_ptr += sizeof(unsigned long);

    /* Length */
    memcpy(buf_ptr, &na_sm_mem_handle->len, sizeof(size_t));
    buf_ptr += sizeof(size_t);

    /* Segments */
    for (i = 0; i < na_sm_mem_handle->iovcnt; i++) {
        memcpy(buf_ptr, &na_sm_mem_handle->iov[i].iov_base, sizeof(void *));
        buf_ptr += sizeof(void *);
        memcpy(buf_ptr, &na_sm_mem_handle->iov[i].iov_len, sizeof(size_t));
        buf_ptr += sizeof(size_t);
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_mem_handle_deserialize(na_class_t NA_UNUSED *na_class,
    na_mem_handle_t *mem_handle, const void *buf, NA_UNUSED na_size_t buf_size)
{
    struct na_sm_mem_handle *na_sm_mem_handle = NULL;
    const char *buf_ptr = (const char *) buf;
    na_return_t ret = NA_SUCCESS;
    unsigned long i;

    na_sm_mem_handle = (struct na_sm_mem_handle *) malloc(
        sizeof(struct na_sm_mem_handle));
    NA_CHECK_ERROR(na_sm_mem_handle == NULL, error, ret, NA_NOMEM,
        "Could not allocate NA SM memory handle");
    na_sm_mem_handle->iov = NULL;

    /* Number of segments */
    memcpy(&na_sm_mem_handle->iovcnt, buf_ptr, sizeof(unsigned long));
    buf_ptr += sizeof(unsigned long);
    NA_CHECK_ERROR(na_sm_mem_handle->iovcnt == 0, error, ret, NA_FAULT,
        "NULL segment count");

    /* Flags */
    memcpy(&na_sm_mem_handle->flags, buf_ptr, sizeof(unsigned long));
    buf_ptr += sizeof(unsigned long);

    /* Length */
    memcpy(&na_sm_mem_handle->len, buf_ptr, sizeof(size_t));
    buf_ptr += sizeof(size_t);

    /* Segments */
    na_sm_mem_handle->iov = (struct iovec *) malloc(na_sm_mem_handle->iovcnt *
        sizeof(struct iovec));
    NA_CHECK_ERROR(na_sm_mem_handle->iov == NULL, error, ret, NA_NOMEM,
        "Could not allocate iovec");

    for (i = 0; i < na_sm_mem_handle->iovcnt; i++) {
        memcpy(&na_sm_mem_handle->iov[i].iov_base, buf_ptr, sizeof(void *));
        buf_ptr += sizeof(void *);
        memcpy(&na_sm_mem_handle->iov[i].iov_len, buf_ptr, sizeof(size_t));
        buf_ptr += sizeof(size_t);
    }

    *mem_handle = (na_mem_handle_t) na_sm_mem_handle;

    return ret;

error:
    if (na_sm_mem_handle) {
        free(na_sm_mem_handle->iov);
        free(na_sm_mem_handle);
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_put(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
    na_size_t length, na_addr_t remote_addr, na_uint8_t NA_UNUSED remote_id,
    na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_mem_handle *na_sm_mem_handle_local =
        (struct na_sm_mem_handle *) local_mem_handle;
    struct na_sm_mem_handle *na_sm_mem_handle_remote =
        (struct na_sm_mem_handle *) remote_mem_handle;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) remote_addr;
    struct iovec *local_iov, *remote_iov;
    struct iovec *local_iovs[IOV_MAX] = {NULL, 0};
    struct iovec *remote_iovs[IOV_MAX] = {NULL, 0};
    unsigned long liovcnt, riovcnt;
    na_return_t ret = NA_SUCCESS;
#if defined(NA_SM_HAS_CMA)
    ssize_t nwrite;
#elif defined(__APPLE__)
    kern_return_t kret;
    mach_port_name_t remote_task;
#endif

#if !defined(NA_SM_HAS_CMA) && !defined(__APPLE__)
    NA_GOTO_ERROR(done, ret, NA_PROTOCOL_ERROR,
        "Not implemented for this platform");
#endif

    switch (na_sm_mem_handle_remote->flags) {
        case NA_MEM_READ_ONLY:
            NA_GOTO_ERROR(done, ret, NA_PERMISSION,
                "Registered memory requires write permission");
            break;
        case NA_MEM_WRITE_ONLY:
        case NA_MEM_READWRITE:
            break;
        default:
            NA_GOTO_ERROR(done, ret, NA_INVALID_ARG,
                "Invalid memory access flag");
    }

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(!(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED),
        done, ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_PUT;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);

    /* Translate local offset, skip this step if not necessary */
    if (local_offset || length != na_sm_mem_handle_local->len) {
        local_iov = (struct iovec *) local_iovs;
        na_sm_offset_translate(na_sm_mem_handle_local, local_offset, length,
            local_iov, &liovcnt);
        NA_LOG_DEBUG("Translated local offsets into %lu segment(s)", liovcnt);
    } else {
        local_iov = na_sm_mem_handle_local->iov;
        liovcnt = na_sm_mem_handle_local->iovcnt;
    }

    /* Translate remote offset, skip this step if not necessary */
    if (remote_offset || length != na_sm_mem_handle_remote->len) {
        remote_iov = (struct iovec *) remote_iovs;
        na_sm_offset_translate(na_sm_mem_handle_remote, remote_offset, length,
            remote_iov, &riovcnt);
        NA_LOG_DEBUG("Translated remote offsets into %lu segment(s)", riovcnt);
    } else {
        remote_iov = na_sm_mem_handle_remote->iov;
        riovcnt = na_sm_mem_handle_remote->iovcnt;
    }

#if defined(NA_SM_HAS_CMA)
    nwrite = process_vm_writev(na_sm_addr->pid, local_iov, liovcnt, remote_iov,
        riovcnt, /* unused */0);
    NA_CHECK_ERROR(nwrite < 0, error, ret, NA_PROTOCOL_ERROR,
        "process_vm_writev() failed (%s)", strerror(errno));
    NA_CHECK_ERROR((na_size_t)nwrite != length, error, ret, NA_MSGSIZE,
        "Wrote %ld bytes, was expecting %lu bytes", nwrite, length);
#elif defined(__APPLE__)
    kret = task_for_pid(mach_task_self(), na_sm_addr->pid, &remote_task);
    NA_CHECK_ERROR(kret != KERN_SUCCESS, error, ret, NA_PROTOCOL_ERROR,
        "task_for_pid() failed (%s)\n"
        "Permission must be set to access remote memory, please refer to the "
        "documentation for instructions.", mach_error_string(kret));
    NA_CHECK_ERROR(liovcnt > 1 || riovcnt > 1, error, ret, NA_PROTOCOL_ERROR,
        "Non-contiguous transfers are not supported");

    kret = mach_vm_write(remote_task,
        (mach_vm_address_t) remote_iov->iov_base,
        (mach_vm_address_t) local_iov->iov_base,
        (mach_msg_type_number_t) length);
    NA_CHECK_ERROR(kret != KERN_SUCCESS, error, ret, NA_PROTOCOL_ERROR,
        "mach_vm_write() failed (%s)", mach_error_string(kret));
#endif

    /* Immediate completion */
    ret = na_sm_complete(na_sm_op_id);
    NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");

    /* Notify local completion */
    if (!NA_SM_CLASS(na_class)->no_wait) {
        int rc = hg_event_set(NA_SM_CLASS(na_class)->self_addr->local_notify);
        NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
            "Could not signal local completion");
    }

done:
    return ret;

error:
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_get(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
    na_size_t length, na_addr_t remote_addr, na_uint8_t NA_UNUSED remote_id,
    na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_mem_handle *na_sm_mem_handle_local =
        (struct na_sm_mem_handle *) local_mem_handle;
    struct na_sm_mem_handle *na_sm_mem_handle_remote =
        (struct na_sm_mem_handle *) remote_mem_handle;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) remote_addr;
    struct iovec *local_iov, *remote_iov;
    struct iovec *local_iovs[IOV_MAX] = {NULL, 0};
    struct iovec *remote_iovs[IOV_MAX] = {NULL, 0};
    unsigned long liovcnt, riovcnt;
    na_return_t ret = NA_SUCCESS;
#if defined(NA_SM_HAS_CMA)
    ssize_t nread;
#elif defined(__APPLE__)
    mach_vm_size_t nread;
    kern_return_t kret;
    mach_port_name_t remote_task;
#endif

#if !defined(NA_SM_HAS_CMA) && !defined(__APPLE__)
    NA_GOTO_ERROR(done, ret, NA_PROTOCOL_ERROR,
        "Not implemented for this platform");
#endif

    switch (na_sm_mem_handle_remote->flags) {
        case NA_MEM_WRITE_ONLY:
            NA_GOTO_ERROR(done, ret, NA_PERMISSION,
                "Registered memory requires write permission");
            break;
        case NA_MEM_READ_ONLY:
        case NA_MEM_READWRITE:
            break;
        default:
            NA_GOTO_ERROR(done, ret, NA_INVALID_ARG,
                "Invalid memory access flag");
    }

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(!(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED),
        done, ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_GET;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);

    /* Translate local offset, skip this step if not necessary */
    if (local_offset || length != na_sm_mem_handle_local->len) {
        local_iov = (struct iovec *) local_iovs;
        na_sm_offset_translate(na_sm_mem_handle_local, local_offset, length,
            local_iov, &liovcnt);
        NA_LOG_DEBUG("Translated local offsets into %lu segment(s)", liovcnt);
    } else {
        local_iov = na_sm_mem_handle_local->iov;
        liovcnt = na_sm_mem_handle_local->iovcnt;
    }

    /* Translate remote offset, skip this step if not necessary */
    if (remote_offset || length != na_sm_mem_handle_remote->len) {
        remote_iov = (struct iovec *) remote_iovs;
        na_sm_offset_translate(na_sm_mem_handle_remote, remote_offset, length,
            remote_iov, &riovcnt);
        NA_LOG_DEBUG("Translated remote offsets into %lu segment(s)", riovcnt);
    } else {
        remote_iov = na_sm_mem_handle_remote->iov;
        riovcnt = na_sm_mem_handle_remote->iovcnt;
    }

#if defined(NA_SM_HAS_CMA)
    nread = process_vm_readv(na_sm_addr->pid, local_iov, liovcnt, remote_iov,
        riovcnt, /* unused */0);
    NA_CHECK_ERROR(nread < 0, error, ret, NA_PROTOCOL_ERROR,
        "process_vm_readv() failed (%s)", strerror(errno));
#elif defined(__APPLE__)
    kret = task_for_pid(mach_task_self(), na_sm_addr->pid, &remote_task);
    NA_CHECK_ERROR(kret != KERN_SUCCESS, error, ret, NA_PROTOCOL_ERROR,
        "task_for_pid() failed (%s)\n"
        "Permission must be set to access remote memory, please refer to the "
        "documentation for instructions.", mach_error_string(kret));
    NA_CHECK_ERROR(liovcnt > 1 || riovcnt > 1, error, ret, NA_PROTOCOL_ERROR,
        "Non-contiguous transfers are not supported");

    kret = mach_vm_read_overwrite(remote_task,
        (mach_vm_address_t) remote_iov->iov_base, length,
        (mach_vm_address_t) local_iov->iov_base, &nread);
    NA_CHECK_ERROR(kret != KERN_SUCCESS, error, ret, NA_PROTOCOL_ERROR,
        "mach_vm_read_overwrite() failed (%s)", mach_error_string(kret));
#endif
#if defined(NA_SM_HAS_CMA) || defined(__APPLE__)
    NA_CHECK_ERROR((na_size_t)nread != length, error, ret, NA_MSGSIZE,
        "Read %ld bytes, was expecting %lu bytes", nread, length);
#endif

    /* Immediate completion */
    ret = na_sm_complete(na_sm_op_id);
    NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");

    /* Notify local completion */
    if (!NA_SM_CLASS(na_class)->no_wait) {
        int rc = hg_event_set(NA_SM_CLASS(na_class)->self_addr->local_notify);
        NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
            "Could not signal local completion");
    }

done:
    return ret;

error:
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE int
na_sm_poll_get_fd(na_class_t *na_class, na_context_t NA_UNUSED *context)
{
    int fd = -1;

    fd = hg_poll_get_fd(NA_SM_CLASS(na_class)->poll_set);
    NA_CHECK_ERROR_NORET(fd == -1, done, "Could not get poll fd from poll set");

 done:
    return fd;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_poll_try_wait(na_class_t *na_class, na_context_t NA_UNUSED *context)
{
    struct na_sm_addr *na_sm_addr;
    na_bool_t ret = NA_TRUE;

    /* Check whether something is in one of the ring buffers */
    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);
    HG_QUEUE_FOREACH(na_sm_addr, &NA_SM_CLASS(na_class)->poll_addr_queue,
        poll_entry) {
        if (!na_sm_ring_buf_is_empty(na_sm_addr->na_sm_recv_ring_buf)) {
            ret = NA_FALSE;
            break;
        }
    }
    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->poll_addr_queue_lock);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress(na_class_t *na_class, na_context_t NA_UNUSED *context,
    unsigned int timeout)
{
    double remaining = timeout / 1000.0; /* Convert timeout in ms into seconds */
    na_return_t ret = NA_TIMEOUT;

    do {
        hg_time_t t1, t2;
        hg_util_bool_t progressed;
        int rc;

        if (timeout)
            hg_time_get_current(&t1);

        rc = hg_poll_wait(NA_SM_CLASS(na_class)->poll_set,
            (unsigned int) (remaining * 1000.0), &progressed);
        NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, NA_PROTOCOL_ERROR,
            "hg_poll_wait() failed");

        /* We progressed, return success */
        if (progressed) {
            ret = NA_SUCCESS;
            break;
        }

        if (timeout) {
            hg_time_get_current(&t2);
            remaining -= hg_time_to_double(hg_time_subtract(t2, t1));
        }
    } while ((int)(remaining * 1000.0) > 0);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_cancel(na_class_t *na_class, na_context_t NA_UNUSED *context,
    na_op_id_t op_id)
{
    struct na_sm_op_id *na_sm_op_id = (struct na_sm_op_id *) op_id;
    na_return_t ret = NA_SUCCESS;
    na_bool_t canceled = NA_FALSE;

    /* Exit if op has already completed */
    if (hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_CANCELED)
        & NA_SM_OP_COMPLETED)
        goto done;

    NA_LOG_DEBUG("Canceling operation ID %p", na_sm_op_id);

    switch (na_sm_op_id->completion_data.callback_info.type) {
        case NA_CB_LOOKUP:
            /* Nothing */
            break;
        case NA_CB_RECV_UNEXPECTED:
            /* Must remove op_id from unexpected op_id queue */
            hg_thread_spin_lock(
                &NA_SM_CLASS(na_class)->unexpected_op_queue_lock);
            if (hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_QUEUED) {
                HG_QUEUE_REMOVE(&NA_SM_CLASS(na_class)->unexpected_op_queue,
                    na_sm_op_id, na_sm_op_id, entry);
                hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
                canceled = NA_TRUE;
            }
            hg_thread_spin_unlock(
                &NA_SM_CLASS(na_class)->unexpected_op_queue_lock);
            break;
        case NA_CB_RECV_EXPECTED:
            /* Must remove op_id from unexpected op_id queue */
            hg_thread_spin_lock(&NA_SM_CLASS(na_class)->expected_op_queue_lock);
            if (hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_QUEUED) {
                HG_QUEUE_REMOVE(&NA_SM_CLASS(na_class)->expected_op_queue,
                    na_sm_op_id, na_sm_op_id, entry);
                hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
                canceled = NA_TRUE;
            }
            hg_thread_spin_unlock(
                &NA_SM_CLASS(na_class)->expected_op_queue_lock);
            break;
        case NA_CB_SEND_UNEXPECTED:
        case NA_CB_SEND_EXPECTED:
            /* Must remove op_id from retry op_id queue */
            hg_thread_spin_lock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);
            if (hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_QUEUED) {
                HG_QUEUE_REMOVE(&NA_SM_CLASS(na_class)->retry_op_queue,
                    na_sm_op_id, na_sm_op_id, entry);
                hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
                canceled = NA_TRUE;
            }
            hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->retry_op_queue_lock);
            break;
        case NA_CB_PUT:
            /* Nothing */
            break;
        case NA_CB_GET:
            /* Nothing */
            break;
        default:
            NA_GOTO_ERROR(done, ret, NA_INVALID_ARG,
                "Operation type %d not supported",
                na_sm_op_id->completion_data.callback_info.type);
    }

    /* Cancel op id */
    if (canceled) {
        ret = na_sm_complete(na_sm_op_id);
        NA_CHECK_NA_ERROR(done, ret, "Could not complete operation");
    }

done:
    return ret;
}
