/**
 * @file      driver.h
 * @brief     Private driver functionality shared by multiple compilation units.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_DRIVER_H_
#define HOUND_PRIVATE_DRIVER_H_

#include <hound/hound.h>
#include <hound-private/io.h>
#include <hound-private/queue.h>
#include <stdbool.h>

#define HOUND_DRIVER_REGISTER_PRIO 102
#define HOUND_DRIVER_REGISTER_FUNC __attribute__((constructor(HOUND_DRIVER_REGISTER_PRIO)))

/** Maximum number of records a driver can produce from a single parse call. */
#define HOUND_DRIVER_MAX_RECORDS 1000

struct schema_desc {
    hound_data_id data_id;
    const char *name;
    size_t fmt_count;
    struct hound_data_fmt *fmts;
};

struct drv_datadesc {
    /** True if this descriptor is enabled; false otherwise. */
    bool enabled;

    /** For enabled data, the number of periods in the avail_periods array. */
    hound_period_count period_count;

    /** For enabled data, the available data periods for this descriptor. */
    hound_data_period *avail_periods;

    /* The schema for this descriptor. */
    const struct schema_desc *schema_desc;
};

struct driver_ops {
    hound_err (*init)(
        const char *path,
        size_t arg_count,
        const struct hound_init_arg *args);

    hound_err (*destroy)(void);

    /**
     * Get the device ID for the backing device.
     *
     * @param device_name a pointer to a string with length HOUND_DEVICE_NAME_MAX,
     * including the '\0' character. The driver must fill this in with a device
     * ID and include the '\0' character. If the driver does not have or cannot
     * find a device ID, it should fill in an empty string.
     *
     * @return an error code
     */
    hound_err (*device_name)(char *device_name);

    /**
     * Get the data descriptors supported by this driver.
     *
     * @param desc_count the length of the descriptors array
     * @param descs a pointer to an array of data descriptors as parsed by the
     *              driver's schema. The driver should set the "enabled" member
     *              of the struct to true if the descriptor is available and
     *              false otherwise, and fill in the frequnecies at which
     *              enabled data is available.
     *
     * @return an error code
     */
    hound_err (*datadesc)(size_t desc_count, struct drv_datadesc *descs);

    hound_err (*setdata)(const struct hound_data_rq_list *data);

    /**
     * Called when the driver's fd is ready to read or write data. A driver must
     * implement either poll or parse, but not both. If it implements poll, then
     * it is responsible for reading and writing to its polled fd and creating
     * records.
     *
     * @param events the events returned by the most recent call to poll()
     * @param next_events to be filled in with the next events that should be
     *                    monitored. For more details, see the manpage for
     *                    poll(). If this is not set, the value will be
     *                    unchanged from the last time poll() was called.
     * @param records a pointer to a block of records that the driver may use,
     *                up to a maximum of HOUND_DRIVER_MAX_RECORDS. Each record
     *                data should be allocated via drv_alloc. driver via
     *                drv_alloc.  All record fields -- except the sequence
     *                number -- shall be filled in by the driver. Each record
     *                data should be allocated via drv_alloc, and the memory for
     *                it shall be owned by the driver core.
     * @param record_count the driver shall set this to the number of records
     *                     produced.
     * @param timeout_enabled set to true if poll should be called again after a
     *                        timeout even if no events have occurred. This will
     *                        directly become an argument into the poll
     *                        syscall in the Hound I/O core.
     * @param timeout if timeout_enabled is set to true, the number of
     *                nanoseconds to wait until calling poll again.
     */
    hound_err (*poll)(
        short events,
        short *next_events,
        struct hound_record *records,
        size_t *record_count,
        bool *timeout_enabled,
        hound_data_period *timeout);

    /**
     * Parse the raw data from the I/O layer and produce one or more records. A
     * driver must implement either poll or parse, but not both. If it
     * implements parse, then the I/O core will read any data available on the
     * driver's fd and pass it into parse, as a buffer.
     *
     * @param buf the raw data coming from the I/O layer
     * @param bytes a pointer to the number of bytes in the buffer. The pointer
     *              should be filled in to indicate how many bytes are still
     *              left unconsumed by the driver. For example, if *bytes is 10
     *              and the driver consumes 8 bytes, the driver should set
     *              *bytes to 2 before returning. If the driver does not change
     *              the value of bytes (no bytes were consumed), then it is
     *              assumed that the driver has no more records to produce at
     *              this time, so parse will not be called again until new data
     *              is available. Note that the next time parse is called, the
     *              unconsumed data will *not* be in the buffer, so if the
     *              driver needs to reference the unconsumed bytes, it must
     *              store them itself.
     * @param records a pointer to a block of records that the driver may use,
     *                up to a maximum of HOUND_DRIVER_MAX_RECORDS. All record
     *                fields -- except the sequence number -- shall be filled in
     *                by the driver. Each record data should be allocated via
     *                drv_alloc, and the memory for it shall be owned by the
     *                driver core.
     * @param record_count the driver shall set this to the number of records
     *                     produced.
     *
     * @return an error code
     */
    hound_err (*parse)(
        unsigned char *buf,
        size_t *bytes,
        struct hound_record *records,
        size_t *record_count);

    hound_err (*start)(int *fd);
    hound_err (*next)(hound_data_id id);
    hound_err (*next_bytes)(hound_data_id id, size_t bytes);
    hound_err (*stop)(void);
};

size_t get_type_size(hound_type type);

/**
 * A function that drivers should use for any allocations they need to do.
 *
 * @param bytes the number of bytes to allocate to the pointer
 *
 * @return a pointer to the allocated memory, or NULL if the allocation failed.
 */
void *drv_alloc(size_t bytes);

/**
 * A function that drivers should use for any reallocations they need to do.
 *
 * @param p a pointer to realloc
 * @param bytes the number of bytes to reallocate to the pointer
 *
 * @return a pointer to the allocated memory, or NULL if the allocation failed.
 */
void *drv_realloc(void *p, size_t bytes);

/**
 * Driver version of strdup.
 *
 * @param s a string to duplicate
 *
 * @return a new, duplicated string, or NULL if the duplication failed.
 */
char *drv_strdup(const char *s);

/**
 * Free a pointer allocated by drv_alloc.
 *
 * @param p a pointer to free
 */
void drv_free(void *p);

/**
 * Gets the currently set driver context.
 *
 * @return the currently set driver context pointer, or NULL if no context has
 * been set.
 */
void *drv_ctx(void);

/**
 * Set a driver context void * for private driver context.
 *
 * @return a driver context pointer
 */
void drv_set_ctx(void *ctx);

/**
 * Get the driver's associated file descriptor.
 *
 * @return the driver's file descriptor
 */
int drv_fd(void);

void driver_init_statics(void);
void driver_destroy_statics(void);

/** Forward declaration for use as opaque pointer. */
struct driver;

hound_err driver_get_dev_name(hound_dev_id id, const char **name);
bool driver_is_pull_mode(const struct driver *drv);
bool driver_is_push_mode(const struct driver *drv);

hound_err driver_get_datadescs(struct hound_datadesc **descs, size_t *len);
void driver_free_datadescs(struct hound_datadesc *descs);

void driver_register(const char *name, struct driver_ops *ops);

hound_err driver_init(
    const char *name,
    const char *path,
    const char *schema_base,
    const char *schema,
    size_t arg_count,
    const struct hound_init_arg *args);

hound_err driver_destroy(const char *path);
hound_err driver_destroy_all(void);

hound_err driver_next(struct driver *drv, hound_data_id id, size_t n);

hound_err driver_ref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *data_rq_list);
hound_err driver_unref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *data_rq_list);

hound_err driver_get(hound_data_id id, struct driver **drv);

bool driver_period_supported(
    struct driver *drv,
    hound_data_id id,
    hound_data_period period);

#define drv_default_pull io_default_pull
#define drv_default_push io_default_push

#endif /* HOUND_PRIVATE_DRIVER_H_ */
