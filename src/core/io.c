/**
 * @file      io.c
 * @brief     Hound I/O subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <hound-private/driver.h>
#include <hound-private/driver-ops.h>
#include <hound-private/error.h>
#include <hound-private/log.h>
#include <hound-private/queue.h>
#include <hound-private/refcount.h>
#include <hound-private/util.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <xlib/xvec.h>

#define PAUSE_SIGNAL SIGUSR1
#define POLL_BUF_SIZE (100*1024)
#define POLL_HAS_DATA (POLLIN|POLLPRI)

/**
 * Provides the relevant information that the I/O system need to know about a
 * given fd (besides the fd value itself). This is stored separately from the
 * struct pollfd structures because poll requires that the array of struct
 * pollfd's passed in be contiguous in memory.
 */
struct fdctx {
    const struct driver *drv;
    hound_seqno next_seqno;
    xvec_t(struct queue *) queues;
};

static struct s_ios {
    xvec_t(struct fdctx *) ctx;
    xvec_t(struct pollfd) fds;
} s_ios;

static pthread_t s_poll_thread;
static sigset_t s_origset;
static pthread_mutex_t s_poll_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_poll_cond = PTHREAD_COND_INITIALIZER;
static volatile bool s_poll_active_target = false;
static volatile bool s_poll_active_current = false;

static uint8_t s_read_buf[POLL_BUF_SIZE];
static struct hound_record s_records[HOUND_DRIVER_MAX_RECORDS];

static inline
size_t get_fd_index(int fd)
{
    size_t index;
    const struct pollfd *pfd;

    for (index = 0; index < xv_size(s_ios.fds); ++index) {
        pfd = &xv_A(s_ios.fds, index);
        if (pfd->fd == fd) {
            break;
        }
    }
    XASSERT_NEQ(index, xv_size(s_ios.fds));

    return index;
}

static inline
struct fdctx *get_fdctx(int fd)
{
    return xv_A(s_ios.ctx, get_fd_index(fd));
}

static
hound_err io_read(int fd, struct fdctx *ctx)
{
    ssize_t bytes_total;
    size_t bytes_left;
    const struct hound_record *end;
    hound_err err;
    size_t i;
    uint8_t *pos;
    struct hound_record *record;
    size_t record_count;
    struct record_info *rec_info;

    bytes_total = read(fd, s_read_buf, ARRAYLEN(s_read_buf));
    if (bytes_total == -1) {
        /* Someone wanted to pause polling; we can finish reading later. */
        if (errno == EINTR) {
            return HOUND_INTR;
        }

        if (errno == EIO) {
            hound_log_err(errno, "read returned EIO on fd %d", fd);
            return HOUND_IO_ERROR;
        }
        else {
            /* Other error codes are likely program bugs. */
            XASSERT_ERROR;
        }
    }

    /* Ask the driver to make records from the buffer. */
    pos = s_read_buf;
    bytes_left = bytes_total;
    while (bytes_left > 0) {
        bytes_total = bytes_left;
        record_count = 0;
        err = drv_op_parse(
            ctx->drv,
            pos,
            &bytes_left,
            s_records,
            &record_count);
        if (err != HOUND_OK) {
            hound_log_err(
                err,
                "Driver failed to parse records (size = %zu, drv = 0x%p)",
                bytes_total, ctx->drv);
            return err;
        }
        XASSERT_LTE(bytes_left, (size_t) bytes_total);

        if (bytes_left == (size_t) bytes_total) {
            /* Driver can't make more records from this buffer. We're done. */
            break;
        }

        XASSERT_GT(record_count, 0);
        pos += bytes_total - bytes_left;

        /* Add to all user queues. */
        end = s_records + record_count;
        for (record = s_records; record < end; ++record) {
            rec_info = drv_alloc(sizeof(*rec_info));
            if (rec_info == NULL) {
                hound_log_err_nofmt(
                    HOUND_OOM,
                    "Failed to malloc a rec_info; can't add record to user queue");
                continue;
            }
            record->seqno = ctx->next_seqno;
            ++ctx->next_seqno;
            record->dev_id = ctx->drv->id;
            memcpy(&rec_info->record, record, sizeof(*record));
            atomic_ref_init(&rec_info->refcount, xv_size(ctx->queues));

            for (i = 0; i < xv_size(ctx->queues); ++i) {
                queue_push(xv_A(ctx->queues, i), rec_info);
            }
        }
    }

    return HOUND_OK;
}

/**
 * Wait for the ready signal for the event loop to continue. Return when it is
 * safe to proceed.
 */
static
void io_wait_for_ready(void) {
    pthread_mutex_lock(&s_poll_mutex);
    while (!s_poll_active_target || xv_size(s_ios.fds) == 0) {
        s_poll_active_current = false;
        pthread_cond_signal(&s_poll_cond);
        pthread_cond_wait(&s_poll_cond, &s_poll_mutex);
    }
    s_poll_active_current = true;
    pthread_mutex_unlock(&s_poll_mutex);
}

static
void io_sighandler(UNUSED int signum)
{
    /* Nothing to do. */
}

static
void *io_poll(UNUSED void *data)
{
    struct sigaction action;
    struct fdctx *ctx;
    hound_err err;
    size_t i;
    int fds;
    struct pollfd *pfd;

    /*
     * Set a dummy action for PAUSE_SIGNAL just so that we can interrupt system
     * calls by raising it. Note that we do *not* set SA_RESTART, as we actually
     * want to be interrupted so that we can pause the thread when needed.
     */
    action.sa_handler = io_sighandler;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);
    err = sigaction(PAUSE_SIGNAL, &action, NULL);
    XASSERT_EQ(err, 0);

    while (true) {
        io_wait_for_ready();

        /* Wait for I/O. */
        fds = ppoll(xv_data(s_ios.fds), xv_size(s_ios.fds), NULL, &s_origset);
        if (fds == -1) {
            if (errno == EINTR) {
                /* We got a signal; probably need to pause the loop. */
                continue;
            }
            else if (errno == ENOMEM) {
                hound_log_err_nofmt(errno, "poll failed with ENOMEM");
            }
            else if (errno == EIO) {
                hound_log_err_nofmt(errno, "poll failed with EIO");
            }
            else {
                /* Other error codes are likely program bugs. */
                XASSERT_ERROR;
            }
        }
        XASSERT_GT (fds, 0);

        /* Read all fds that have data. */
        for (i = 0; fds > 0 && i < xv_size(s_ios.fds); ++i) {
            pfd = &xv_A(s_ios.fds, i);
            if (pfd->revents == 0) {
                continue;
            }
            XASSERT(pfd->revents & POLL_HAS_DATA);

            ctx = xv_A(s_ios.ctx, i);
            err = io_read(pfd->fd, ctx);
            if (err == HOUND_INTR) {
                /* Someone wants to pause polling; finish reading later. */
                break;
            }
            --fds;
            if (err != HOUND_OK) {
                hound_log_err(err, "Failed to grab record from fd %d", pfd->fd);
                continue;
            }
        }
    }

    return NULL;
}

static
void io_pause_poll(void)
{
    hound_err err;

    /*
     * Wait until the poll has actually canceled. io_wait_for_ready will signal
     * on the condition variable when it is run.
     */
    pthread_mutex_lock(&s_poll_mutex);
    err = pthread_kill(s_poll_thread, PAUSE_SIGNAL);
    XASSERT_EQ(err, 0);
    while (s_poll_active_current) {
        s_poll_active_target = false;
        pthread_cond_signal(&s_poll_cond);
        pthread_cond_wait(&s_poll_cond, &s_poll_mutex);
    }
    pthread_mutex_unlock(&s_poll_mutex);
}

static
void io_resume_poll(void)
{
    pthread_mutex_lock(&s_poll_mutex);
    s_poll_active_target = true;
    pthread_cond_signal(&s_poll_cond);
    pthread_mutex_unlock(&s_poll_mutex);
}

static
hound_err io_start_poll(void)
{
    pthread_attr_t attr;
    hound_err err;

    err = pthread_attr_init(&attr);
    if (err != 0) {
        return err;
    }
    err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    XASSERT_EQ(err, 0);

    err = pthread_create(&s_poll_thread, NULL, io_poll, NULL);
    if (err != 0) {
        return err;
    }

    return HOUND_OK;
}

static
void io_stop_poll(void)
{
    hound_err err;
    void *ret;

    /* First let the event loop gracefully exit. */
    io_pause_poll();

    /* Now shoot it in the head. */
    err = pthread_cancel(s_poll_thread);
    XASSERT_EQ(err, 0);

    /*
     * Remove our signal handler. Note that this *must* be done after pausing,
     * since we rely on the signal to trigger the pause.
     */
    err = sigaction(PAUSE_SIGNAL, NULL, NULL);
    XASSERT_EQ(err, 0);

    /* Wait until the thread is finally dead. */
    err = pthread_join(s_poll_thread, &ret);
    XASSERT_EQ(err, 0);
    XASSERT_EQ(ret, PTHREAD_CANCELED);
}

hound_err io_add_fd(int fd, struct driver *drv)
{
    struct fdctx *ctx;
    hound_err err;
    int flags;
    struct pollfd *pfd;

    XASSERT_NOT_NULL(drv);
    XASSERT_NEQ(fd, 0);

    /* Our fds must be non-blocking for the poll loop to work. */
    flags = fcntl(fd, F_GETFL, 0);
    XASSERT_NEQ(flags, -1);
    err = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    XASSERT_NEQ(err, -1);

    io_pause_poll();

    pfd = xv_pushp(struct pollfd, s_ios.fds);
    if (pfd == NULL) {
        err = HOUND_OOM;
        goto error_xv_push;
    }
    pfd->fd = fd;
    pfd->events = POLL_HAS_DATA;

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto error_ctx_alloc;
    }
    ctx->drv = drv;
    ctx->next_seqno = 0;
    xv_init(ctx->queues);

    xv_push(struct fdctx *, s_ios.ctx, ctx);
    if (xv_data(s_ios.ctx) == NULL) {
        err = HOUND_OOM;
        goto error_ctx_push;
    }

    io_resume_poll();

    return HOUND_OK;

error_ctx_push:
    free(ctx);
error_ctx_alloc:
    (void) xv_pop(s_ios.fds);
error_xv_push:
    io_resume_poll();
    return err;
}

void io_remove_fd(int fd)
{
    struct fdctx *ctx;
    size_t index;

    index = get_fd_index(fd);
    ctx = xv_A(s_ios.ctx, index);

    io_pause_poll();

    /* Remove fd and ctx. */
    RM_VEC_INDEX(s_ios.fds, index);
    RM_VEC_INDEX(s_ios.ctx, index);

    io_resume_poll();

    xv_destroy(ctx->queues);
    free(ctx);
}

hound_err io_add_queue(int fd, struct queue *queue)
{
    struct fdctx *ctx;

    ctx = get_fdctx(fd);
    XASSERT_NOT_NULL(ctx);

    io_pause_poll();

    xv_push(struct queue *, ctx->queues, queue);
    if (xv_data(ctx->queues) == NULL) {
        /* Push failed to reallocate the queue. */
        return HOUND_OOM;
    }

    io_resume_poll();

    return HOUND_OK;
}

void io_remove_queue(int fd, struct queue *queue)
{
    struct fdctx *ctx;
    size_t index;

    ctx = get_fdctx(fd);
    XASSERT_NOT_NULL(ctx);

    io_pause_poll();

    /* Find our queue. */
    for (index = 0; index < xv_size(ctx->queues); ++index) {
        if (xv_A(ctx->queues, index) == queue) {
            break;
        }
    }
    XASSERT_NEQ(index, xv_size(ctx->queues));

    /* Remove the queue. */
    RM_VEC_INDEX(ctx->queues, index);

    io_resume_poll();
}

void io_init(void)
{
    sigset_t blockset;
    hound_err err;

    xv_init(s_ios.fds);
    xv_init(s_ios.ctx);

    /*
     * Block our pause signal so it will get queued up if someone sends it.
     * If the signal is sent after io_wait_for_ready() but before ppoll(),
     * ppoll() will still see the signal because it gets queued up while
     * blocked. If we didn't block the signal here, such a signal would be
     * dropped and ppoll() would never act on the signal, leading to deadlock.
     *
     * See pselect(2) for more information.
     */
    sigemptyset(&blockset);
    sigaddset(&blockset, PAUSE_SIGNAL);
    err = pthread_sigmask(SIG_BLOCK, &blockset, &s_origset);
    XASSERT_EQ(err, 0);

    err = io_start_poll();
    if (err != HOUND_OK) {
        hound_log_err_nofmt(err, "Failed io_start_poll");
    }
}

void io_destroy(void)
{
    io_stop_poll();
    xv_destroy(s_ios.ctx);
    xv_destroy(s_ios.fds);
}
