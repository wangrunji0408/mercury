/*
 * Copyright (C) 2013-2019 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#include "na_plugin.h"

#include "mercury_time.h"
#include "mercury_mem.h"

#include <stdlib.h>
#include <string.h>

/****************/
/* Local Macros */
/****************/

#define NA_CLASS_DELIMITER "+" /* e.g. "class+protocol" */

#ifdef _WIN32
#  define strtok_r strtok_s
#  undef strdup
#  define strdup _strdup
#endif

#define NA_ATOMIC_QUEUE_SIZE 1024   /* TODO make it configurable */

#define NA_PROGRESS_LOCK 0x80000000 /* 32-bit lock value for serial progress */

/************************************/
/* Local Type and Struct Definition */
/************************************/

struct na_private_class {
    struct na_class na_class;                   /* Must remain as first field */
};

/* Private context / do not expose private members to plugins */
struct na_private_context {
    struct na_context context;                  /* Must remain as first field */
    hg_thread_cond_t  completion_queue_cond;    /* Completion queue cond */
#ifdef NA_HAS_MULTI_PROGRESS
    hg_thread_cond_t  progress_cond;            /* Progress cond */
#endif
    hg_thread_mutex_t completion_queue_mutex;   /* Completion queue mutex */
#ifdef NA_HAS_MULTI_PROGRESS
    hg_thread_mutex_t progress_mutex;           /* Progress mutex */
#endif
    HG_QUEUE_HEAD(na_cb_completion_data) backfill_queue; /* Backfill completion queue */
    struct hg_atomic_queue *completion_queue;   /* Default completion queue */
    na_class_t *na_class;                       /* Pointer to NA class */
    hg_atomic_int32_t backfill_queue_count;     /* Number of entries in backfill queue */
    hg_atomic_int32_t trigger_waiting;          /* Polling/waiting in trigger */
#ifdef NA_HAS_MULTI_PROGRESS
    hg_atomic_int32_t progressing;              /* Progressing count */
#endif
};

/********************/
/* Local Prototypes */
/********************/

/* Parse host string and fill info */
static na_return_t
na_info_parse(
    const char *host_string,
    struct na_info **na_info_ptr
    );

/* Free host info */
static void
na_info_free(
    struct na_info *na_info
    );

/*******************/
/* Local Variables */
/*******************/

/* NA plugin class table */
static const struct na_class_ops *const na_class_table[] = {
#ifdef NA_HAS_SM
    &NA_PLUGIN_OPS(sm), /* Keep NA SM first for protocol selection */
#endif
#ifdef NA_HAS_OFI
    &NA_PLUGIN_OPS(ofi),
#endif
#ifdef NA_HAS_BMI
    &NA_PLUGIN_OPS(bmi),
#endif
#ifdef NA_HAS_MPI
    &NA_PLUGIN_OPS(mpi),
#endif
#ifdef NA_HAS_CCI
    &NA_PLUGIN_OPS(cci),
#endif
    NULL
};

/* Return code string table */
#define X(a) #a,
static const char *const na_return_name[] = { NA_RETURN_VALUES };
#undef X

/* Default error log mask */
#ifdef NA_HAS_VERBOSE_ERROR
unsigned int NA_LOG_MASK = HG_LOG_TYPE_ERROR | HG_LOG_TYPE_WARNING;
#endif

/*---------------------------------------------------------------------------*/
static na_return_t
na_info_parse(const char *info_string, struct na_info **na_info_ptr)
{
    struct na_info *na_info = NULL;
    char *input_string = NULL, *token = NULL, *locator = NULL;
    na_return_t ret = NA_SUCCESS;

    na_info = (struct na_info *) malloc(sizeof(struct na_info));
    NA_CHECK_ERROR(na_info == NULL, error, ret, NA_NOMEM,
        "Could not allocate NA info struct");

    /* Initialize NA info */
    na_info->class_name = NULL;
    na_info->protocol_name = NULL;
    na_info->host_name = NULL;

    /* Copy info string and work from that */
    input_string = strdup(info_string);
    NA_CHECK_ERROR(input_string == NULL, error, ret, NA_NOMEM,
        "Could not duplicate host string");

    /**
     * Strings can be of the format:
     *   [<class>+]<protocol>[://[<host string>]]
     */

    /* Get first part of string (i.e., class_name+protocol) */
    token = strtok_r(input_string, ":", &locator);

    /* Is class name specified */
    if (strstr(token, NA_CLASS_DELIMITER) != NULL) {
        char *_locator = NULL;

        token = strtok_r(token, NA_CLASS_DELIMITER, &_locator);

        /* Get NA class name */
        na_info->class_name = strdup(token);
        NA_CHECK_ERROR(na_info->class_name == NULL, error, ret, NA_NOMEM,
            "Could not duplicate NA info class name");

        /* Get protocol name */
        na_info->protocol_name = strdup(_locator);
        NA_CHECK_ERROR(na_info->protocol_name == NULL, error, ret,
            NA_NOMEM, "Could not duplicate NA info protocol name");
    } else {
        /* Get protocol name */
        na_info->protocol_name = strdup(token);
        NA_CHECK_ERROR(na_info->protocol_name == NULL, error, ret,
            NA_NOMEM, "Could not duplicate NA info protocol name");
    }

    /* Is the host string empty? */
    if (!locator || locator[0] == '\0')
        goto done;

    /* Format sanity check ("://") */
    NA_CHECK_ERROR(strncmp(locator, "//", 2) != 0, error, ret,
        NA_PROTONOSUPPORT, "Bad address string format");

    /* :// followed by empty hostname is allowed, explicitly check here */
    if (locator[2] == '\0')
        goto done;

    na_info->host_name = strdup(locator + 2);
    NA_CHECK_ERROR(na_info->host_name == NULL, error, ret, NA_NOMEM,
        "Could not duplicate NA info host name");

done:
    *na_info_ptr = na_info;
    free(input_string);

    return ret;

error:
    na_info_free(na_info);
    free(input_string);

    return ret;
}

/*---------------------------------------------------------------------------*/
static void
na_info_free(struct na_info *na_info)
{
    if (!na_info) return;

    free(na_info->class_name);
    free(na_info->protocol_name);
    free(na_info->host_name);
    free(na_info);
}

/*---------------------------------------------------------------------------*/
na_class_t *
NA_Initialize(const char *info_string, na_bool_t listen)
{
    return NA_Initialize_opt(info_string, listen, NULL);
}

/*---------------------------------------------------------------------------*/
na_class_t *
NA_Initialize_opt(const char *info_string, na_bool_t listen,
    const struct na_init_info *na_init_info)
{
    struct na_private_class *na_private_class = NULL;
    struct na_info *na_info = NULL;
    unsigned int plugin_index;
    const unsigned int plugin_count =
        sizeof(na_class_table) / sizeof(na_class_table[0]) - 1;
    na_bool_t plugin_found = NA_FALSE;
#ifdef NA_HAS_VERBOSE_ERROR
    const char *log_level = NULL;
#endif
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(info_string == NULL, error, ret, NA_INVALID_ARG,
        "NULL info string");

#ifdef NA_HAS_VERBOSE_ERROR
    /* Set log level */
    log_level = getenv("HG_NA_LOG_LEVEL");
    if (log_level && (strcmp(log_level, "debug") == 0))
        NA_LOG_MASK |= HG_LOG_TYPE_DEBUG;
#endif

    na_private_class = (struct na_private_class *) malloc(
        sizeof(struct na_private_class));
    NA_CHECK_ERROR(na_private_class == NULL, error, ret, NA_NOMEM,
        "Could not allocate class");
    memset(na_private_class, 0, sizeof(struct na_private_class));

    ret = na_info_parse(info_string, &na_info);
    NA_CHECK_NA_ERROR(error, ret, "Could not parse host string");

    na_info->na_init_info = na_init_info;
    if (na_init_info)
        na_private_class->na_class.progress_mode = na_init_info->progress_mode;

    /* Print debug info */
    NA_LOG_DEBUG("Class: %s, Protocol: %s, Hostname: %s" , na_info->class_name,
        na_info->protocol_name, na_info->host_name);

    for (plugin_index = 0; plugin_index < plugin_count; plugin_index++) {
        na_bool_t verified = NA_FALSE;

        NA_CHECK_ERROR(na_class_table[plugin_index]->class_name == NULL, error,
            ret, NA_PROTONOSUPPORT, "class name is not defined");

        NA_CHECK_ERROR(na_class_table[plugin_index]->check_protocol == NULL,
            error, ret, NA_OPNOTSUPPORTED,
            "check_protocol plugin callback is not defined");

        /* Skip check protocol if class name does not match */
        if (na_info->class_name) {
            if (strcmp(na_class_table[plugin_index]->class_name,
                na_info->class_name) != 0)
                continue;
        }

        /* Check that protocol is supported */
        verified = na_class_table[plugin_index]->check_protocol(
            na_info->protocol_name);
        if (!verified) {
            NA_CHECK_ERROR(na_info->class_name, error, ret,
                NA_PROTONOSUPPORT,
                "Specified class name does not support requested protocol");
            continue;
        }

        /* If no class name specified, take the first plugin that supports
         * the protocol */
        if (!na_info->class_name) {
            /* While we're here, dup the class_name */
            na_info->class_name = strdup(
                na_class_table[plugin_index]->class_name);
            NA_CHECK_ERROR(na_info->class_name == NULL, error, ret,
                NA_NOMEM, "Unable to dup class name string");
        }

        /* All checks have passed */
        plugin_found = NA_TRUE;
        break;
    }

    NA_CHECK_ERROR(!plugin_found, error, ret, NA_PROTONOSUPPORT,
        "No suitable plugin found that matches %s", info_string);

    na_private_class->na_class.ops = na_class_table[plugin_index];
    NA_CHECK_ERROR(na_private_class->na_class.ops == NULL, error, ret,
        NA_INVALID_ARG, "NULL NA class ops");

    NA_CHECK_ERROR(na_private_class->na_class.ops->initialize == NULL, error,
        ret, NA_OPNOTSUPPORTED, "initialize plugin callback is not defined");

    ret = na_private_class->na_class.ops->initialize(
        &na_private_class->na_class, na_info, listen);
    NA_CHECK_NA_ERROR(error, ret, "Could not initialize plugin");

    na_private_class->na_class.protocol_name = strdup(na_info->protocol_name);
    NA_CHECK_ERROR(na_private_class->na_class.protocol_name == NULL, error, ret,
        NA_NOMEM, "Could not duplicate protocol name");

    na_private_class->na_class.listen = listen;

    na_info_free(na_info);

    return (na_class_t *) na_private_class;

error:
    na_info_free(na_info);
    if (na_private_class) {
        free(na_private_class->na_class.protocol_name);
        free(na_private_class);
    }
    return NULL;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Finalize(na_class_t *na_class)
{
    struct na_private_class *na_private_class =
        (struct na_private_class *) na_class;
    na_return_t ret = NA_SUCCESS;

    if (!na_private_class)
        goto done;

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->finalize == NULL, done, ret,
        NA_OPNOTSUPPORTED, "finalize plugin callback is not defined");

    ret = na_class->ops->finalize(&na_private_class->na_class);

    free(na_private_class->na_class.protocol_name);
    free(na_private_class);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
void
NA_Cleanup(void)
{
    unsigned int plugin_count =
        sizeof(na_class_table) / sizeof(na_class_table[0]) - 1;
    unsigned int i;

    for (i = 0; i < plugin_count; i++) {
        if (!na_class_table[i]->cleanup)
            continue;

        na_class_table[i]->cleanup();
    }
}

/*---------------------------------------------------------------------------*/
na_context_t *
NA_Context_create(na_class_t *na_class)
{
    return NA_Context_create_id(na_class, 0);
}

/*---------------------------------------------------------------------------*/
na_context_t *
NA_Context_create_id(na_class_t *na_class, na_uint8_t id)
{
    na_return_t ret = NA_SUCCESS;
    struct na_private_context *na_private_context = NULL;

    NA_CHECK_ERROR(na_class == NULL, error, ret, NA_INVALID_ARG,
        "NULL NA class");

    na_private_context = (struct na_private_context *) malloc(
        sizeof(struct na_private_context));
    NA_CHECK_ERROR(na_private_context == NULL, error, ret, NA_NOMEM,
        "Could not allocate context");
    na_private_context->na_class = na_class;

    NA_CHECK_ERROR(na_class->ops == NULL, error, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->context_create) {
        ret = na_class->ops->context_create(na_class,
            &na_private_context->context.plugin_context, id);
        NA_CHECK_NA_ERROR(error, ret, "Could not create plugin context");
    }

    /* Initialize completion queue */
    na_private_context->completion_queue =
        hg_atomic_queue_alloc(NA_ATOMIC_QUEUE_SIZE);
    NA_CHECK_ERROR(na_private_context->completion_queue == NULL, error, ret,
        NA_NOMEM, "Could not allocate queue");
    HG_QUEUE_INIT(&na_private_context->backfill_queue);
    hg_atomic_init32(&na_private_context->backfill_queue_count, 0);

    /* Initialize completion queue mutex/cond */
    hg_thread_mutex_init(&na_private_context->completion_queue_mutex);
    hg_thread_cond_init(&na_private_context->completion_queue_cond);
    hg_atomic_init32(&na_private_context->trigger_waiting, 0);

#ifdef NA_HAS_MULTI_PROGRESS
    /* Initialize progress mutex/cond */
    hg_thread_mutex_init(&na_private_context->progress_mutex);
    hg_thread_cond_init(&na_private_context->progress_cond);
    hg_atomic_init32(&na_private_context->progressing, 0);
#endif

    return (na_context_t *) na_private_context;

error:
    free(na_private_context);
    return NULL;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Context_destroy(na_class_t *na_class, na_context_t *context)
{
    struct na_private_context *na_private_context =
        (struct na_private_context *) context;
    na_bool_t empty;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    if (!context)
        goto done;

    /* Check that completion queue is empty now */
    empty = hg_atomic_queue_is_empty(na_private_context->completion_queue);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_BUSY,
        "Completion queue should be empty");
    hg_atomic_queue_free(na_private_context->completion_queue);

    /* Check that backfill completion queue is empty now */
    hg_thread_mutex_lock(&na_private_context->completion_queue_mutex);
    empty = HG_QUEUE_IS_EMPTY(&na_private_context->backfill_queue);
    hg_thread_mutex_unlock(&na_private_context->completion_queue_mutex);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_BUSY,
        "Completion queue should be empty");

    /* Destroy completion queue mutex/cond */
    hg_thread_mutex_destroy(&na_private_context->completion_queue_mutex);
    hg_thread_cond_destroy(&na_private_context->completion_queue_cond);

    /* Destroy NA plugin context */
    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->context_destroy) {
        ret = na_class->ops->context_destroy(na_class,
            na_private_context->context.plugin_context);
        NA_CHECK_NA_ERROR(done, ret, "Could not destroy plugin context");
    }

#ifdef NA_HAS_MULTI_PROGRESS
    /* Destroy progress mutex/cond */
    hg_thread_mutex_destroy(&na_private_context->progress_mutex);
    hg_thread_cond_destroy(&na_private_context->progress_cond);
#endif

    free(na_private_context);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_op_id_t
NA_Op_create(na_class_t *na_class)
{
    na_op_id_t ret = NA_OP_ID_NULL;

    NA_CHECK_ERROR_NORET(na_class == NULL, done, "NULL NA class");
    NA_CHECK_ERROR_NORET(na_class->ops == NULL, done, "NULL NA class ops");
    NA_CHECK_ERROR_NORET(na_class->ops->op_create == NULL, done,
        "op_create plugin callback is not defined");

    ret = na_class->ops->op_create(na_class);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Op_destroy(na_class_t *na_class, na_op_id_t op_id)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");

    if (op_id == NA_OP_ID_NULL)
        /* Nothing to do */
        goto done;

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->op_destroy == NULL, done, ret,
        NA_OPNOTSUPPORTED, "op_destroy plugin callback is not defined");

    ret = na_class->ops->op_destroy(na_class, op_id);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_lookup(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, const char *name, na_op_id_t *op_id)
{
    char *name_string = NULL;
    char *short_name = NULL;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(context == NULL, done, ret, NA_INVALID_ARG,
        "NULL context");
    NA_CHECK_ERROR(name == NULL, done, ret, NA_INVALID_ARG,
        "Lookup name is NULL");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->addr_lookup == NULL, done, ret,
        NA_OPNOTSUPPORTED, "addr_lookup plugin callback is not defined");

    /* Copy name and work from that */
    name_string = strdup(name);
    NA_CHECK_ERROR(name_string == NULL, done, ret, NA_NOMEM,
        "Could not duplicate string");

    /* If NA class name was specified, we can remove the name here:
     * ie. bmi+tcp://hostname:port -> tcp://hostname:port */
    if (strstr(name_string, NA_CLASS_DELIMITER) != NULL)
        strtok_r(name_string, NA_CLASS_DELIMITER, &short_name);
    else
        short_name = name_string;

    ret = na_class->ops->addr_lookup(na_class, context, callback, arg,
        short_name, op_id);

done:
    free(name_string);
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_lookup2(na_class_t *na_class, const char *name, na_addr_t *addr)
{
    char *name_string = NULL;
    char *short_name = NULL;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(name == NULL, done, ret, NA_INVALID_ARG,
        "Lookup name is NULL");
    NA_CHECK_ERROR(addr == NULL, done, ret, NA_INVALID_ARG,
        "NULL pointer to na_addr_t");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (!na_class->ops->addr_lookup2)
        /* Until we switch to new lookup, exit if no callback */
        goto done;
//    NA_CHECK_ERROR(na_class->ops->addr_lookup2 == NULL, done, ret,
//        NA_PROTOCOL_ERROR, "addr_lookup2 plugin callback is not defined");

    /* Copy name and work from that */
    name_string = strdup(name);
    NA_CHECK_ERROR(name_string == NULL, done, ret, NA_NOMEM,
        "Could not duplicate string");

    /* If NA class name was specified, we can remove the name here:
     * ie. bmi+tcp://hostname:port -> tcp://hostname:port */
    if (strstr(name_string, NA_CLASS_DELIMITER) != NULL)
        strtok_r(name_string, NA_CLASS_DELIMITER, &short_name);
    else
        short_name = name_string;

    ret = na_class->ops->addr_lookup2(na_class, short_name, addr);

done:
    free(name_string);
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_free(na_class_t *na_class, na_addr_t addr)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    if (addr == NA_ADDR_NULL)
        /* Nothing to do */
        goto done;

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->addr_free == NULL, done, ret,
        NA_OPNOTSUPPORTED, "addr_free plugin callback is not defined");

    ret = na_class->ops->addr_free(na_class, addr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_set_remove(na_class_t *na_class, na_addr_t addr)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    if (addr == NA_ADDR_NULL)
        /* Nothing to do */
        goto done;

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->addr_set_remove)
        ret = na_class->ops->addr_set_remove(na_class, addr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_self(na_class_t *na_class, na_addr_t *addr)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(addr == NULL, done, ret, NA_INVALID_ARG,
        "NULL pointer to na_addr_t");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->addr_self == NULL, done, ret,
        NA_OPNOTSUPPORTED, "addr_self plugin callback is not defined");

    ret = na_class->ops->addr_self(na_class, addr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_dup(na_class_t *na_class, na_addr_t addr, na_addr_t *new_addr)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(addr == NA_ADDR_NULL, done, ret, NA_INVALID_ARG,
        "NULL addr");
    NA_CHECK_ERROR(new_addr == NULL, done, ret, NA_INVALID_ARG,
        "NULL pointer to NA addr");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->addr_dup == NULL, done, ret,
        NA_OPNOTSUPPORTED, "addr_dup plugin callback is not defined");

    ret = na_class->ops->addr_dup(na_class, addr, new_addr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_bool_t
NA_Addr_cmp(na_class_t *na_class, na_addr_t addr1, na_addr_t addr2)
{
    na_bool_t ret = NA_FALSE;

    NA_CHECK_ERROR_NORET(na_class == NULL, done, "NULL NA class");

    if (addr1 == NA_ADDR_NULL && addr2 == NA_ADDR_NULL)
        NA_GOTO_DONE(done, ret, NA_TRUE);

    if (addr1 == NA_ADDR_NULL || addr2 == NA_ADDR_NULL)
        NA_GOTO_DONE(done, ret, NA_FALSE);

    NA_CHECK_ERROR_NORET(na_class->ops == NULL, done, "NULL NA class ops");
    NA_CHECK_ERROR_NORET(na_class->ops->addr_cmp == NULL, done,
        "addr_cmp plugin callback is not defined");

    ret = na_class->ops->addr_cmp(na_class, addr1, addr2);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_to_string(na_class_t *na_class, char *buf, na_size_t *buf_size,
    na_addr_t addr)
{
    char *buf_ptr = buf;
    na_size_t buf_size_used = 0, plugin_buf_size = 0;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    /* buf can be NULL */
    NA_CHECK_ERROR(buf_size == 0, done, ret, NA_INVALID_ARG,
        "NULL buffer size");
    NA_CHECK_ERROR(addr == NA_ADDR_NULL, done, ret, NA_INVALID_ARG,
        "NULL addr");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->addr_to_string == NULL, done, ret,
        NA_OPNOTSUPPORTED, "addr_to_string plugin callback is not defined");

    /* Automatically prepend string by plugin name with class delimiter,
     * except for MPI plugin (special case, because of generated string) */
    if (strcmp(na_class->ops->class_name, "mpi") == 0) {
        buf_size_used = 0;
        plugin_buf_size = *buf_size;
    } else {
        buf_size_used = strlen(na_class->ops->class_name)
            + strlen(NA_CLASS_DELIMITER);
        if (buf_ptr) {
            NA_CHECK_ERROR(buf_size_used >= *buf_size, done, ret, NA_OVERFLOW,
                "Buffer size too small to copy addr");
            strcpy(buf_ptr, na_class->ops->class_name);
            strcat(buf_ptr, NA_CLASS_DELIMITER);
            buf_ptr += buf_size_used;
            plugin_buf_size = *buf_size - buf_size_used;
        } else
            plugin_buf_size = 0;
    }

    ret = na_class->ops->addr_to_string(na_class, buf_ptr, &plugin_buf_size,
        addr);

    *buf_size = buf_size_used + plugin_buf_size;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_serialize(na_class_t *na_class, void *buf, na_size_t buf_size,
    na_addr_t addr)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(buf == NULL, done, ret, NA_INVALID_ARG,
        "NULL buffer");
    NA_CHECK_ERROR(buf_size == 0, done, ret, NA_INVALID_ARG,
        "NULL buffer size");
    NA_CHECK_ERROR(addr == NA_ADDR_NULL, done, ret, NA_INVALID_ARG,
        "NULL addr");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->addr_serialize == NULL, done, ret,
        NA_OPNOTSUPPORTED, "addr_serialize plugin callback is not defined");

    ret = na_class->ops->addr_serialize(na_class, buf, buf_size, addr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Addr_deserialize(na_class_t *na_class, na_addr_t *addr, const void *buf,
    na_size_t buf_size)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(addr == NULL, done, ret, NA_INVALID_ARG,
        "NULL pointer to addr");
    NA_CHECK_ERROR(buf == NULL, done, ret, NA_INVALID_ARG,
        "NULL buffer");
    NA_CHECK_ERROR(buf_size == 0, done, ret, NA_INVALID_ARG,
        "NULL buffer size");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->addr_deserialize == NULL, done, ret,
        NA_OPNOTSUPPORTED, "addr_deserialize plugin callback is not defined");

    ret = na_class->ops->addr_deserialize(na_class, addr, buf, buf_size);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
void *
NA_Msg_buf_alloc(na_class_t *na_class, na_size_t buf_size, void **plugin_data)
{
    void *ret = NULL;

    NA_CHECK_ERROR_NORET(na_class == NULL, done, "NULL NA class");
    NA_CHECK_ERROR_NORET(buf_size == 0, done, "NULL buffer size");
    NA_CHECK_ERROR_NORET(plugin_data == NULL, done,
        "NULL pointer to plugin data");

    NA_CHECK_ERROR_NORET(na_class->ops == NULL, done, "NULL NA class ops");
    if (na_class->ops->msg_buf_alloc)
        ret = na_class->ops->msg_buf_alloc(na_class, buf_size, plugin_data);
    else {
        na_size_t page_size = (na_size_t) hg_mem_get_page_size();

        ret = hg_mem_aligned_alloc(page_size, buf_size);
        NA_CHECK_ERROR_NORET(ret == NULL, done,
            "Could not allocate %d bytes", (int) buf_size);
        memset(ret, 0, buf_size);
        *plugin_data = (void *)1; /* Sanity check on free */
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Msg_buf_free(na_class_t *na_class, void *buf, void *plugin_data)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(buf == NULL, done, ret, NA_INVALID_ARG,
        "NULL buffer");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->msg_buf_free)
        ret = na_class->ops->msg_buf_free(na_class, buf, plugin_data);
    else {
        NA_CHECK_ERROR(plugin_data != (void *)1, done, ret, NA_FAULT,
            "Invalid plugin data value");
        hg_mem_aligned_free(buf);
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Msg_init_unexpected(na_class_t *na_class, void *buf, na_size_t buf_size)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(buf == NULL, done, ret, NA_INVALID_ARG,
        "NULL buffer");
    NA_CHECK_ERROR(buf_size == 0, done, ret, NA_INVALID_ARG,
        "NULL buffer size");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->msg_init_unexpected)
        ret = na_class->ops->msg_init_unexpected(na_class, buf, buf_size);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Msg_init_expected(na_class_t *na_class, void *buf, na_size_t buf_size)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(buf == NULL, done, ret, NA_INVALID_ARG,
        "NULL buffer");
    NA_CHECK_ERROR(buf_size == 0, done, ret, NA_INVALID_ARG,
        "NULL buffer size");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->msg_init_expected)
        ret = na_class->ops->msg_init_expected(na_class, buf, buf_size);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_handle_create(na_class_t *na_class, void *buf, na_size_t buf_size,
    unsigned long flags, na_mem_handle_t *mem_handle)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(buf == NULL, done, ret, NA_INVALID_ARG,
        "NULL buffer");
    NA_CHECK_ERROR(buf_size == 0, done, ret, NA_INVALID_ARG,
        "NULL buffer size");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->mem_handle_create == NULL, done, ret,
        NA_OPNOTSUPPORTED, "mem_handle_create plugin callback is not defined");

    ret = na_class->ops->mem_handle_create(na_class, buf, buf_size, flags,
        mem_handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_handle_create_segments(na_class_t *na_class, struct na_segment *segments,
    na_size_t segment_count, unsigned long flags,
    na_mem_handle_t *mem_handle)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(segments == NULL, done, ret, NA_INVALID_ARG,
        "NULL pointer to segments");
    NA_CHECK_ERROR(segment_count == 0, done, ret, NA_INVALID_ARG,
        "NULL segment count");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->mem_handle_create_segments == NULL, done, ret,
        NA_OPNOTSUPPORTED,
        "mem_handle_create_segments plugin callback is not defined");

    ret = na_class->ops->mem_handle_create_segments(na_class, segments,
        segment_count, flags, mem_handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_handle_free(na_class_t *na_class, na_mem_handle_t mem_handle)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(mem_handle == NA_MEM_HANDLE_NULL, done, ret,
        NA_INVALID_ARG, "NULL memory handle");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->mem_handle_free == NULL, done, ret,
        NA_OPNOTSUPPORTED, "mem_handle_free plugin callback is not defined");

    ret = na_class->ops->mem_handle_free(na_class, mem_handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_register(na_class_t *na_class, na_mem_handle_t mem_handle)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(mem_handle == NA_MEM_HANDLE_NULL, done, ret,
        NA_INVALID_ARG, "NULL memory handle");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->mem_register)
        /* Optional */
        ret = na_class->ops->mem_register(na_class, mem_handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_deregister(na_class_t *na_class, na_mem_handle_t mem_handle)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(mem_handle == NA_MEM_HANDLE_NULL, done, ret,
        NA_INVALID_ARG, "NULL memory handle");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->mem_deregister)
        /* Optional */
        ret = na_class->ops->mem_deregister(na_class, mem_handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_publish(na_class_t *na_class, na_mem_handle_t mem_handle)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(mem_handle == NA_MEM_HANDLE_NULL, done, ret,
        NA_INVALID_ARG, "NULL memory handle");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->mem_publish)
        /* Optional */
        ret = na_class->ops->mem_publish(na_class, mem_handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_unpublish(na_class_t *na_class, na_mem_handle_t mem_handle)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(mem_handle == NA_MEM_HANDLE_NULL, done, ret,
        NA_INVALID_ARG, "NULL memory handle");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    if (na_class->ops->mem_unpublish)
        /* Optional */
        ret = na_class->ops->mem_unpublish(na_class, mem_handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_handle_serialize(na_class_t *na_class, void *buf, na_size_t buf_size,
    na_mem_handle_t mem_handle)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(buf == NULL, done, ret, NA_INVALID_ARG,
        "NULL buffer");
    NA_CHECK_ERROR(buf_size == 0, done, ret, NA_INVALID_ARG,
        "NULL buffer size");
    NA_CHECK_ERROR(mem_handle == NA_MEM_HANDLE_NULL, done, ret,
        NA_INVALID_ARG, "NULL memory handle");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->mem_handle_serialize == NULL, done, ret,
        NA_OPNOTSUPPORTED,
        "mem_handle_serialize plugin callback is not defined");

    ret = na_class->ops->mem_handle_serialize(na_class, buf, buf_size,
        mem_handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Mem_handle_deserialize(na_class_t *na_class, na_mem_handle_t *mem_handle,
    const void *buf, na_size_t buf_size)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(mem_handle == NULL, done, ret,
        NA_INVALID_ARG, "NULL pointer to memory handle");
    NA_CHECK_ERROR(buf == NULL, done, ret, NA_INVALID_ARG,
        "NULL buffer");
    NA_CHECK_ERROR(buf_size == 0, done, ret, NA_INVALID_ARG,
        "NULL buffer size");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->mem_handle_deserialize == NULL, done, ret,
        NA_OPNOTSUPPORTED,
        "mem_handle_deserialize plugin callback is not defined");

    ret = na_class->ops->mem_handle_deserialize(na_class, mem_handle, buf,
        buf_size);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_bool_t
NA_Poll_try_wait(na_class_t *na_class, na_context_t *context)
{
    struct na_private_context *na_private_context =
        (struct na_private_context *) context;
    na_bool_t ret = NA_FALSE;

    NA_CHECK_ERROR_NORET(na_class == NULL, error, "NULL NA class");
    NA_CHECK_ERROR_NORET(context == NULL, error, "NULL context");

    /* Do not try to wait if NA_NO_BLOCK is set */
    if (na_class->progress_mode & NA_NO_BLOCK)
        return NA_FALSE;

    /* Something is in one of the completion queues */
    if (!hg_atomic_queue_is_empty(na_private_context->completion_queue) ||
        hg_atomic_get32(&na_private_context->backfill_queue_count))
        return NA_FALSE;

    /* Check plugin try wait */
    NA_CHECK_ERROR_NORET(na_class->ops == NULL, error, "NULL NA class ops");
    if (na_class->ops->na_poll_try_wait)
        return na_class->ops->na_poll_try_wait(na_class, context);

    return NA_TRUE;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Progress(na_class_t *na_class, na_context_t *context, unsigned int timeout)
{
    struct na_private_context *na_private_context =
        (struct na_private_context *) context;
    double remaining = timeout / 1000.0; /* Convert timeout in ms into seconds */
#ifdef NA_HAS_MULTI_PROGRESS
    hg_util_int32_t old, num;
#endif
    na_return_t ret = NA_TIMEOUT;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(na_private_context == NULL, done, ret, NA_INVALID_ARG,
        "NULL context");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->progress == NULL, done, ret,
        NA_OPNOTSUPPORTED, "progress plugin callback is not defined");

#ifdef NA_HAS_MULTI_PROGRESS
    hg_atomic_incr32(&na_private_context->progressing);
    for (;;) {
        hg_time_t t1, t2;

        old = hg_atomic_get32(&na_private_context->progressing)
            & (hg_util_int32_t) ~NA_PROGRESS_LOCK;
        num = old | (hg_util_int32_t) NA_PROGRESS_LOCK;
        if (hg_atomic_cas32(&na_private_context->progressing, old, num))
            break; /* No other thread is progressing */

        /* Timeout is 0 so leave */
        if (remaining <= 0) {
            hg_atomic_decr32(&na_private_context->progressing);
            goto done;
        }

        hg_time_get_current(&t1);

        /* Prevent multiple threads from concurrently calling progress on
         * the same context */
        hg_thread_mutex_lock(&na_private_context->progress_mutex);

        num = hg_atomic_get32(&na_private_context->progressing);
        /* Do not need to enter condition if lock is already released */
        if (((num & (hg_util_int32_t) NA_PROGRESS_LOCK) != 0)
            && (hg_thread_cond_timedwait(&na_private_context->progress_cond,
                &na_private_context->progress_mutex,
                (unsigned int) (remaining * 1000.0)) != HG_UTIL_SUCCESS)) {
            /* Timeout occurred so leave */
            hg_atomic_decr32(&na_private_context->progressing);
            hg_thread_mutex_unlock(&na_private_context->progress_mutex);
            goto done;
        }

        hg_thread_mutex_unlock(&na_private_context->progress_mutex);

        hg_time_get_current(&t2);
        remaining -= hg_time_to_double(hg_time_subtract(t2, t1));
        /* Give a chance to call progress with timeout of 0 */
        if (remaining < 0)
            remaining = 0;
    }
#endif

    /* Something is in one of the completion queues */
    if (!hg_atomic_queue_is_empty(na_private_context->completion_queue)
        || hg_atomic_get32(&na_private_context->backfill_queue_count)) {
        ret = NA_SUCCESS; /* Progressed */
#ifdef NA_HAS_MULTI_PROGRESS
        goto unlock;
#else
        goto done;
#endif
    }

    /* Try to make progress for remaining time */
    ret = na_class->ops->progress(na_class, context,
        (unsigned int) (remaining * 1000.0));

#ifdef NA_HAS_MULTI_PROGRESS
unlock:
    do {
        old = hg_atomic_get32(&na_private_context->progressing);
        num = (old - 1) ^ (hg_util_int32_t) NA_PROGRESS_LOCK;
    } while (!hg_atomic_cas32(&na_private_context->progressing, old, num));

    if (num > 0) {
        /* If there is another processes entered in progress, signal it */
        hg_thread_mutex_lock(&na_private_context->progress_mutex);
        hg_thread_cond_signal(&na_private_context->progress_cond);
        hg_thread_mutex_unlock(&na_private_context->progress_mutex);
    }
#endif

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Trigger(na_context_t *context, unsigned int timeout, unsigned int max_count,
    int callback_ret[], unsigned int *actual_count)
{
    struct na_private_context *na_private_context =
        (struct na_private_context *) context;
    double remaining = timeout / 1000.0; /* Convert timeout in ms into seconds */
    na_return_t ret = NA_SUCCESS;
    unsigned int count = 0;

    NA_CHECK_ERROR(context == NULL, done, ret, NA_INVALID_ARG,
        "NULL context");

    while (count < max_count) {
        struct na_cb_completion_data *completion_data = NULL;

        completion_data = hg_atomic_queue_pop_mc(
            na_private_context->completion_queue);
        if (!completion_data) {
            /* Check backfill queue */
            if (hg_atomic_get32(&na_private_context->backfill_queue_count)) {
                hg_thread_mutex_lock(
                    &na_private_context->completion_queue_mutex);
                completion_data = HG_QUEUE_FIRST(
                    &na_private_context->backfill_queue);
                HG_QUEUE_POP_HEAD(&na_private_context->backfill_queue,
                    entry);
                hg_atomic_decr32(&na_private_context->backfill_queue_count);
                hg_thread_mutex_unlock(
                    &na_private_context->completion_queue_mutex);
                if (!completion_data)
                    continue; /* Give another chance to grab it */
            } else {
                hg_time_t t1, t2;

                /* If something was already processed leave */
                if (count)
                    break;

                /* Timeout is 0 so leave */
                if ((int)(remaining * 1000.0) <= 0) {
                    ret = NA_TIMEOUT;
                    break;
                }

                hg_time_get_current(&t1);

                hg_atomic_incr32(&na_private_context->trigger_waiting);
                hg_thread_mutex_lock(
                    &na_private_context->completion_queue_mutex);
                /* Otherwise wait timeout ms */
                while (hg_atomic_queue_is_empty(
                    na_private_context->completion_queue)
                    && !hg_atomic_get32(
                        &na_private_context->backfill_queue_count)) {
                    if (hg_thread_cond_timedwait(
                        &na_private_context->completion_queue_cond,
                        &na_private_context->completion_queue_mutex,
                        timeout) != HG_UTIL_SUCCESS) {
                        /* Timeout occurred so leave */
                        ret = NA_TIMEOUT;
                        break;
                    }
                }
                hg_thread_mutex_unlock(
                    &na_private_context->completion_queue_mutex);
                hg_atomic_decr32(&na_private_context->trigger_waiting);
                if (ret == NA_TIMEOUT)
                    break;

                hg_time_get_current(&t2);
                remaining -= hg_time_to_double(hg_time_subtract(t2, t1));
                continue; /* Give another chance to grab it */
            }
        }

        /* Completion data should be valid */
        NA_CHECK_ERROR(completion_data == NULL, done, ret, NA_INVALID_ARG,
            "NULL completion data");

        /* Execute callback */
        if (completion_data->callback) {
            int cb_ret =
                completion_data->callback(&completion_data->callback_info);
            if (callback_ret)
                callback_ret[count] = cb_ret;
        }

        /* Execute plugin callback (free resources etc)
         * NB. If the NA operation ID is reused by the plugin for another
         * operation we must be careful that resources are released BEFORE
         * that operation ID gets re-used. This is currently not protected
         * and left upon the plugin implementation.
         */
        if (completion_data->plugin_callback)
            completion_data->plugin_callback(
                completion_data->plugin_callback_args);

        count++;
    }

    if (actual_count)
        *actual_count = count;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_Cancel(na_class_t *na_class, na_context_t *context, na_op_id_t op_id)
{
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(na_class == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class");
    NA_CHECK_ERROR(context == NULL, done, ret, NA_INVALID_ARG,
        "NULL context");
    NA_CHECK_ERROR(op_id == NA_OP_ID_NULL, done, ret, NA_INVALID_ARG,
        "NULL operation ID");

    NA_CHECK_ERROR(na_class->ops == NULL, done, ret, NA_INVALID_ARG,
        "NULL NA class ops");
    NA_CHECK_ERROR(na_class->ops->cancel == NULL, done, ret, NA_OPNOTSUPPORTED,
        "cancel plugin callback is not defined");

    ret = na_class->ops->cancel(na_class, context, op_id);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
const char *
NA_Error_to_string(na_return_t errnum)
{
    return na_return_name[errnum];
}

/*---------------------------------------------------------------------------*/
na_return_t
na_cb_completion_add(na_context_t *context,
    struct na_cb_completion_data *na_cb_completion_data)
{
    struct na_private_context *na_private_context =
        (struct na_private_context *) context;
    na_return_t ret = NA_SUCCESS;

    if (hg_atomic_queue_push(na_private_context->completion_queue,
        na_cb_completion_data) != HG_UTIL_SUCCESS) {
        /* Queue is full */
        hg_thread_mutex_lock(&na_private_context->completion_queue_mutex);
        HG_QUEUE_PUSH_TAIL(&na_private_context->backfill_queue,
            na_cb_completion_data, entry);
        hg_atomic_incr32(&na_private_context->backfill_queue_count);
        hg_thread_mutex_unlock(&na_private_context->completion_queue_mutex);
    }

    if (hg_atomic_get32(&na_private_context->trigger_waiting)) {
        hg_thread_mutex_lock(&na_private_context->completion_queue_mutex);
        /* Callback is pushed to the completion queue when something completes
         * so wake up anyone waiting in the trigger */
        hg_thread_cond_signal(&na_private_context->completion_queue_cond);
        hg_thread_mutex_unlock(&na_private_context->completion_queue_mutex);
    }

    return ret;
}
