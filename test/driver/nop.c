/**
 * @file      nop.c
 * @brief     No-op driver implementation. This driver implements all the
 *            required driver functions but does not actually produce data, and
 *            is used for unit-testing the driver core.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound_private/driver.h>
#include <hound_test/assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define NS_PER_SEC (1e9)
#define FD_INVALID (-1)
#define UNUSED __attribute__((unused))

static const char *s_device_ids[] = {"dummy", "fake"};
static const hound_data_period s_accel_period[] = {
    0,
    NS_PER_SEC,
    NS_PER_SEC/10,
    NS_PER_SEC/500,
    NS_PER_SEC/1000,
    NS_PER_SEC/2000
};
static const hound_data_period s_gyro_period[] = { 0 };
static const struct hound_drv_datadesc s_datadesc[] = {
    {
        .id = HOUND_DEVICE_ACCELEROMETER,
        .name = "super-extra-accelerometer",
        .period_count = ARRAYLEN(s_accel_period),
        .avail_periods = s_accel_period
    },

    {
        .id = HOUND_DEVICE_GYROSCOPE,
        .name = "oneshot-gyroscope",
        .period_count = ARRAYLEN(s_gyro_period),
        .avail_periods = s_gyro_period
    }
};
static int s_fd = FD_INVALID;

hound_err nop_init(UNUSED hound_alloc alloc, UNUSED void *data)
{
    return HOUND_OK;
}

hound_err nop_destroy(void)
{
    return HOUND_OK;
}

hound_err nop_reset(UNUSED hound_alloc alloc, UNUSED void *data)
{
    return HOUND_OK;
}

hound_err nop_device_ids(
        const char ***device_ids,
        hound_device_id_count *count)
{
    *count = ARRAYLEN(s_device_ids);
    *device_ids = s_device_ids;

    return HOUND_OK;
}

hound_err nop_datadesc(
        const struct hound_drv_datadesc **desc,
        hound_data_count *count)
{
    *count = ARRAYLEN(s_datadesc);
    *desc = s_datadesc;

    return HOUND_OK;
}

hound_err nop_setdata(UNUSED const struct hound_drv_data_list *data)
{
    return HOUND_OK;
}

hound_err nop_parse(
    const uint8_t *buf,
    size_t *bytes,
    struct hound_record *record)
{
    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);
    XASSERT_NOT_NULL(record);

    return HOUND_OK;
}

hound_err nop_start(int *fd)
{
    XASSERT_EQ(s_fd, FD_INVALID);
    s_fd = open("/dev/null", 0);
    XASSERT_NEQ(s_fd, -1);
    *fd = s_fd;

    return HOUND_OK;
}

hound_err nop_next(UNUSED hound_data_id id)
{
	return HOUND_OK;
}

hound_err nop_stop(void)
{
    hound_err err;

    XASSERT_NEQ(s_fd, FD_INVALID);
    err = close(s_fd);
    XASSERT_NEQ(err, -1);

    return HOUND_OK;
}

struct driver_ops nop_driver = {
    .init = nop_init,
    .destroy = nop_destroy,
    .reset = nop_reset,
    .device_ids = nop_device_ids,
    .datadesc = nop_datadesc,
    .setdata = nop_setdata,
    .parse = nop_parse,
    .start = nop_start,
    .next = nop_next,
    .stop = nop_stop
};

hound_err register_nop_driver(void)
{
    return driver_register("/dev/nop", &nop_driver, NULL);
}
