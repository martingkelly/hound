/**
 * @file      file.c
 * @brief     Unit test for the file driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <hound/hound.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

struct text {
    char *data;
    size_t index;
};

void data_cb(
    const struct hound_record *record,
    UNUSED hound_seqno seqno,
    void *data)
{
    struct text *text;
    int ret;

    XASSERT_NOT_NULL(record);
    XASSERT_NOT_NULL(record->data);
    XASSERT_GT(record->size, 0);
    XASSERT_NOT_NULL(data);

    text = data;
    ret = memcmp(text->data + text->index, record->data, record->size);
    XASSERT_EQ(ret, 0);
    text->index += record->size;
}

char *slurp_file(const char *filepath, size_t *count)
{
    ssize_t bytes;
    char *data;
    int fd;
    int ret;
    struct stat st;

    fd = open(filepath, 0, O_RDONLY);
    XASSERT_NEQ(fd, -1);

    ret = fstat(fd, &st);
    XASSERT_EQ(ret, 0);
    data = malloc(st.st_size);
    XASSERT_NOT_NULL(data);

    bytes = 0;
    do {
        ret = read(fd, data + bytes, st.st_size - bytes);
        XASSERT_GT(ret, 0);
        bytes += ret;
    } while (bytes < st.st_size);

    *count = st.st_size;

    close(fd);

    return data;
}

int main(int argc, const char **argv)
{
    struct hound_ctx *ctx;
    hound_err err;
    const char *filepath;
    const char *schema_base;
    struct text text;
    size_t total_count;
    struct hound_data_rq data_rq = { .id = HOUND_DATA_FILE, .period_ns = 0 };
    struct hound_rq rq = {
        .queue_len = 100,
        .cb = data_cb,
        .cb_ctx = &text,
        .rq_list.len = 1,
        .rq_list.data = &data_rq
    };

    if (argc != 3) {
        fprintf(stderr, "Usage: file SCHEMA-BASE-PATH TESTFILE\n");
        exit(EXIT_FAILURE);
    }

    if (strnlen(argv[1], PATH_MAX) == PATH_MAX) {
        fprintf(stderr, "Schema base path is longer than PATH_MAX\n");
        exit(EXIT_FAILURE);
    }
    schema_base = argv[1];

    if (strnlen(argv[2], PATH_MAX) == PATH_MAX) {
        fprintf(stderr, "File argument is longer than PATH_MAX\n");
        exit(EXIT_FAILURE);
    }
    filepath = argv[2];

    err = hound_init_driver(
        "file",
        filepath,
        schema_base,
        "file.yaml",
        0,
        NULL);
    XASSERT_OK(err);

    err = hound_alloc_ctx(&rq, &ctx);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_OK(err);

    text.data = slurp_file(filepath, &total_count);
    text.index = 0;
    while (text.index < total_count) {
        err = hound_next(ctx, 1);
        XASSERT_OK(err);
        err = hound_read(ctx, 1, NULL);
        XASSERT_OK(err);
    }
    XASSERT_EQ(text.index, total_count);
    free(text.data);

    err = hound_stop(ctx);
    XASSERT_OK(err);

    err = hound_free_ctx(ctx);
    XASSERT_OK(err);

    err = hound_destroy_driver(filepath);
    XASSERT_OK(err);

    return 0;
}
