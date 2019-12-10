/**
 * @file      obd.c
 * @brief     Unit test for the OBD-II driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <hound/driver/obd.h>
#include <hound/hound.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <linux/can.h>
#include <linux/limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <valgrind.h>

struct test_ctx {
    size_t seqno;
    size_t count;
    struct modepid *obd_rqs;
    char iface[HOUND_DEVICE_NAME_MAX];
};

struct modepid {
    yobd_mode mode;
    yobd_pid pid;
    size_t count;
};

static struct modepid s_obd_rqs[] = {
    {
        .mode = 0x01,
        .pid = 0x000c,
        .count = 0
    },
    {
        .mode = 0x01,
        .pid = 0x000d,
        .count = 0
    }
};

static struct test_ctx s_ctx = {
    .seqno = 0,
    .count = ARRAYLEN(s_obd_rqs),
    .obd_rqs = s_obd_rqs
};

void data_cb(const struct hound_record *record, void *data)
{
    struct test_ctx *ctx;
    const char *dev_name;
    hound_err err;
    size_t i;
    yobd_mode mode;
    struct modepid *modepid;
    yobd_pid pid;

    XASSERT_NOT_NULL(record);
    XASSERT_NOT_NULL(record->data);
    XASSERT_EQ(record->size, sizeof(float));
    XASSERT_NOT_NULL(data);

    ctx = data;

    XASSERT_EQ(ctx->seqno, record->seqno);

    hound_obd_get_mode_pid(record->data_id, &mode, &pid);

    for (i = 0; i < ctx->count; ++i) {
        modepid = &ctx->obd_rqs[i];
        if (modepid->mode == mode && modepid->pid == pid) {
            ++modepid->count;
        }
    }

    err = hound_get_dev_name(record->dev_id, &dev_name);
    XASSERT_OK(err);
    XASSERT_STREQ(dev_name, ctx->iface);

    ++ctx->seqno;
}

bool can_iface_exists(const char *iface)
{
    struct ifreq ifr;
    int fd;
    int ret;
    bool success;

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    XASSERT_NEQ(fd, -1);

    strcpy(ifr.ifr_name, iface); /* NOLINT, string size already checked */
    ret = ioctl(fd, SIOCGIFINDEX, &ifr);
    success = (ret != -1);

    close(fd);

    return success;
}

static
void test_read(hound_data_period period_ns, bool enforce_counts)
{
    struct hound_ctx *ctx;
    hound_err err;
    size_t i;
    struct modepid *modepid;
    size_t n;
    struct hound_data_rq *data_rq;
    struct hound_data_rq data_rqs[s_ctx.count];
    struct hound_rq rq = {
        .queue_len = 10000,
        .cb = data_cb,
        .cb_ctx = &s_ctx,
        .rq_list.len = ARRAYLEN(data_rqs),
        .rq_list.data = data_rqs
    };

    for (i = 0; i < ARRAYLEN(data_rqs); ++i) {
        data_rq = &data_rqs[i];
        modepid = &s_ctx.obd_rqs[i];
        modepid->count = 0;
        hound_obd_get_data_id(modepid->mode, modepid->pid, &data_rq->id);
        data_rq->period_ns = period_ns;
    }

    s_ctx.seqno = 0;
    err = hound_alloc_ctx(&ctx, &rq);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_OK(err);

    if (RUNNING_ON_VALGRIND) {
        n = 2;
    }
    else {
        n = 100;
    }
    for (i = 0; i < n; ++i) {
        err = hound_read(ctx, 1);
        XASSERT_OK(err);
    }

    if (enforce_counts) {
        for (i = 1; i < s_ctx.count; ++i) {
            XASSERT_EQ(s_ctx.obd_rqs[0].count, s_ctx.obd_rqs[i].count);
        }
    }
    else {
        fprintf(stderr, "counts: %lu %lu\n", s_ctx.obd_rqs[0].count, s_ctx.obd_rqs[1].count);
    }

    err = hound_stop(ctx);
    XASSERT_OK(err);

    err = hound_free_ctx(ctx);
    XASSERT_OK(err);
}

int main(int argc, const char **argv)
{
    hound_err err;
    struct hound_obd_driver_init init;
    const char *schema_base;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s CAN-IFACE SCHEMA-BASE-PATH\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (strnlen(argv[1], IFNAMSIZ) == IFNAMSIZ) {
        fprintf(stderr, "Device argument is longer than IFNAMSIZ\n");
        exit(EXIT_FAILURE);
    }
    strcpy(s_ctx.iface, argv[1]); /* NOLINT, string size already checked */
    strcpy(init.iface, argv[1]); /* NOLINT, string size already checked */

    if (strnlen(argv[2], PATH_MAX) == PATH_MAX) {
        fprintf(stderr, "Schema base path is longer than PATH_MAX\n");
        exit(EXIT_FAILURE);
    }
    schema_base = argv[2];

    if (!can_iface_exists(init.iface)) {
        fprintf(
            stderr,
            "Failed to open CAN interface %s\n"
            "Run this command to create a CAN interface:\n"
            "sudo meson/vcan setup\n",
            init.iface);
        exit(EXIT_FAILURE);
    }

    init.yobd_schema = "standard-pids.yaml";

    err = hound_register_obd_driver(schema_base, &init);
    XASSERT_OK(err);

    /* On-demand data. */
    test_read(0, true);

    /* Periodic data. */
    test_read(1e9/1000, false);

    err = hound_unregister_driver(init.iface);
    XASSERT_OK(err);

    return EXIT_SUCCESS;
}
