/*
 * Copyright (c) 2015, iNuron NV. All rights reserved.
 *
 * Author: Chrysostomos Nanakos <cnanakos@openvstorage.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <fcntl.h>
#include <assert.h>

#include <openvstorage/volumedriver.h>

#include "list.h"
#include "tapdisk.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-server.h"

#ifdef HACE_CONFIG_H
#include "config.h"
#endif

#define MAX_OPENVSTORAGE_REQS        TAPDISK_DATA_REQUESTS
#define MAX_OPENVSTORAGE_MERGED_REQS 32
#define MAX_MERGE_SIZE               131072

struct tdopenvstorage_request
{
    td_request_t treq[MAX_OPENVSTORAGE_MERGED_REQS];
    int treq_count;
    int op;
    uint64_t offset;
    uint64_t size;
    void *buf;
    ssize_t ret;
    struct list_head queue;
    struct ovs_aiocb *aiocbp;
    ovs_buffer_t *ovs_buffer;
};

struct tdopenvstorage_data
{
    ovs_ctx_t* ctx;
    uint64_t size;

    struct list_head reqs_inflight;
    struct list_head reqs_free;
    struct tdopenvstorage_request *req_deferred;
    struct tdopenvstorage_request reqs[MAX_OPENVSTORAGE_REQS];
    int reqs_free_count;

    int timeout_event_id;
    int pipe_fds[2];
    int pipe_event_id;

    struct {
        int req_total;
        int req_issued;
        int req_issued_no_merge;
        int req_issued_forced;
        int req_issued_direct;
        int req_issued_timeout;

        int req_miss;
        int req_miss_op;
        int req_miss_ofs;
        int req_miss_buf;
    } stat;
};

static int tdopenvstorage_close(td_driver_t* driver);
static void tdopenvstorage_pipe_read_cb(event_id_t eb, char mode, void* data);

static int get_volume_info(struct tdopenvstorage_data *td)
{
    struct stat st;
    int r = ovs_stat(td->ctx, &st);
    if (r < 0)
    {
        return -EIO;
    }
    td->size = st.st_size;
    return 0;
}

static void openvstorage_finish_aiocb(ovs_completion_t *completion, void *arg)
{
    int r;
    struct tdopenvstorage_request *tdreq = (struct tdopenvstorage_request*)arg;
    struct ovs_aiocb *aiocbp = tdreq->aiocbp;
    struct tdopenvstorage_data *prv = tdreq->treq[0].image->driver->data;

    tdreq->ret = ovs_aio_return(prv->ctx, aiocbp);
    ovs_aio_finish(prv->ctx, aiocbp);
    ovs_aio_release_completion(completion);

    if (tdreq->op == TD_OP_READ && tdreq->ret != -1)
    {
        memcpy(tdreq->buf,
               aiocbp->aio_buf,
               tdreq->ret);
        if (tdreq->ret < tdreq->size)
        {
            memset(tdreq->buf + tdreq->ret,
                   0,
                   tdreq->size - tdreq->ret);
        }
    }
    ovs_deallocate(prv->ctx, tdreq->ovs_buffer);

    while (1)
    {
        r = write(prv->pipe_fds[1], (void*)&tdreq, sizeof(tdreq));
        if (r >= 0)
        {
            break;
        }
        if ((errno != EAGAIN) && (errno != EINTR))
        {
            break;
        }
    }
    if (r <= 0)
    {
        DPRINTF("%s: failed to write to completion pipe\n", __func__);
    }
    free(aiocbp);
}

static int tdopenvstorage_open(td_driver_t *driver,
                               const char* filename,
                               td_flag_t flags)
{
    int r = 0, i;
    struct tdopenvstorage_data *prv = driver->data;
    memset(prv, 0x00, sizeof(struct tdopenvstorage_data));

    prv->ctx = ovs_ctx_init(filename, O_RDWR);
    if (prv->ctx == NULL)
    {
        r = -errno;
        DPRINTF("%s: failed to create Open vStorage context\n", __func__);
        return r;
    }

    INIT_LIST_HEAD(&prv->reqs_inflight);
    INIT_LIST_HEAD(&prv->reqs_free);

    for (i = 0; i < MAX_OPENVSTORAGE_REQS; i++)
    {
        INIT_LIST_HEAD(&prv->reqs[i].queue);
        list_add(&prv->reqs[i].queue, &prv->reqs_free);
    }

    prv->reqs_free_count = MAX_OPENVSTORAGE_REQS;

    prv->pipe_fds[0] = prv->pipe_fds[1] = prv->pipe_event_id = -1;
    prv->timeout_event_id = -1;

    r = pipe(prv->pipe_fds);
    if (r)
    {
        r = -errno;
        DPRINTF("%s: failed to create inter-thread pipe (%d)\n", __func__, r);
        goto err_exit;
    }

    prv->pipe_event_id = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
                                                       prv->pipe_fds[0],
                                                       0,
                                                       tdopenvstorage_pipe_read_cb,
                                                       prv);

    r = get_volume_info(prv);
    if (r < 0)
    {
        goto err_exit_with_td;
    }

    driver->info.sector_size = DEFAULT_SECTOR_SIZE;
    driver->info.size = prv->size >> SECTOR_SHIFT;
    driver->info.info = 0;
    return 0;

err_exit_with_td:
    if(prv->pipe_event_id >= 0)
    {
        tapdisk_server_unregister_event(prv->pipe_event_id);
    }
err_exit:
    ovs_ctx_destroy(prv->ctx);
    return r;
}

static int tdopenvstorage_close(td_driver_t *driver)
{
    struct tdopenvstorage_data *prv = driver->data;
    int r;

    if (prv->pipe_fds[0] >= 0)
    {
        close(prv->pipe_fds[0]);
        close(prv->pipe_fds[1]);
    }

    if (prv->pipe_event_id >=0)
    {
        tapdisk_server_unregister_event(prv->pipe_event_id);
    }

    r = ovs_ctx_destroy(prv->ctx);
    if (r < 0)
    {
        DPRINTF("%s: cannot destroy Open vStorage context\n", __func__);
    }
    return r;
}

static void tdopenvstorage_pipe_read_cb(event_id_t eb,
                                        char mode,
                                        void *data)
{
    struct tdopenvstorage_data *prv = data;
    struct tdopenvstorage_request *req;
    char *p = (void *)&req;
    int ret, tr, i;

    for (tr = 0; tr < sizeof(req);)
    {
        ret = read(prv->pipe_fds[0], p + tr, sizeof(req) - tr);
        if (ret == 0) {
            DPRINTF("%s: short read on completion pipe\n", __func__);
            break;
        }
        if (ret < 0) {
            if ((errno == EAGAIN) || (errno == EINTR))
            {
                continue;
            }
            break;
        }
        tr += ret;
    }

    if (tr != sizeof(req)) {
        DPRINTF("%s: read aborted on completion pipe\n", __func__);
        return;
    }

    for (i = 0; i < req->treq_count; i++)
    {
        int err = req->ret < 0 ? -EIO : 0;
        if (err < 0)
        {
            DPRINTF("%s: error in req->ret: %d\n", __func__, err);
        }
        td_complete_request(req->treq[i], err);
    }
    list_move(&req->queue, &prv->reqs_free);
    prv->reqs_free_count++;
}

static int openvstorage_submit_aio_request(struct tdopenvstorage_data *prv,
                                           struct tdopenvstorage_request *tdreq)
{
    int r;
    ovs_buffer_t *ovs_buf = NULL;
    void *buf = NULL;

    ovs_buf = ovs_allocate(prv->ctx, tdreq->size);
    if (ovs_buf == NULL)
    {
        DPRINTF("%s: cannot allocate shm buffer\n", __func__);
        return -EIO;
    }
    buf = ovs_buffer_data(ovs_buf);

    if (tdreq->op == TD_OP_WRITE)
    {
        memcpy(buf, tdreq->buf, tdreq->size);
    }

    struct ovs_aiocb *aiocbp = malloc(sizeof(struct ovs_aiocb));
    if (aiocbp == NULL)
    {
        ovs_deallocate(prv->ctx, ovs_buf);
        return -ENOMEM;
    }
    aiocbp->aio_buf = buf;
    aiocbp->aio_nbytes = tdreq->size;
    aiocbp->aio_offset = tdreq->offset;

    tdreq->aiocbp = aiocbp;
    tdreq->ovs_buffer = ovs_buf;

    ovs_completion_t *completion =
        ovs_aio_create_completion((ovs_callback_t) openvstorage_finish_aiocb,
                                  (void*)tdreq);

    if (completion == NULL)
    {
        free(aiocbp);
        ovs_deallocate(prv->ctx, ovs_buf);
        return -ENOMEM;
    }

    switch (tdreq->op)
    {
    case TD_OP_READ:
        r = ovs_aio_readcb(prv->ctx, aiocbp, completion);
        break;
    case TD_OP_WRITE:
        r = ovs_aio_writecb(prv->ctx, aiocbp, completion);
        break;
    default:
        r = -EINVAL;
    }

    if (r < 0)
    {
        free(aiocbp);
        ovs_aio_release_completion(completion);
        ovs_deallocate(prv->ctx, ovs_buf);
        return r;
    }
    return 0;
}

static int tdopenvstorage_submit_aio_request(struct tdopenvstorage_data *prv,
                                             struct tdopenvstorage_request *req)
{
    int r, i;
    prv->stat.req_issued++;
    list_add_tail(&req->queue, &prv->reqs_inflight);

    switch (req->op)
    {
    case TD_OP_READ:
    case TD_OP_WRITE:
        r = openvstorage_submit_aio_request(prv, req);
        break;
    default:
        r = -EINVAL;
    }

    if (r < 0)
    {
        goto err_exit;
    }
    return 0;
err_exit:
    for (i = 0; req->treq_count; i++)
    {
        td_complete_request(req->treq[i], r);
    }
    return r;
}

static void tdopenvstorage_timeout_cb(event_id_t eb, char mode, void* data)
{
    struct tdopenvstorage_data* prv = data;

    if (prv->req_deferred)
    {
        tdopenvstorage_submit_aio_request(prv,
                                          prv->req_deferred);
        prv->req_deferred = NULL;
        prv->stat.req_issued_timeout++;
    }
    tapdisk_server_unregister_event(eb);
    prv->timeout_event_id = -1;
}

static void tdopenvstorage_queue_request(td_driver_t *driver,
                                         td_request_t treq)
{
    struct tdopenvstorage_data *prv = driver->data;
    size_t size = treq.secs * driver->info.sector_size;
    off_t offset = treq.sec * driver->info.sector_size;
    struct tdopenvstorage_request *req;
    int merged = 0;

    prv->stat.req_total++;

    if (prv->req_deferred)
    {
        struct tdopenvstorage_request *dr = prv->req_deferred;
        if ((dr->op == treq.op) && ((dr->offset + dr->size) == offset) &&
                (((unsigned long)dr->buf + dr->size) == (unsigned long)treq.buf))
        {
            dr->treq[dr->treq_count++] = treq;
            dr->size += size;
            merged = 1;
        }
        else
        {
            prv->stat.req_miss++;
            if (dr->op != treq.op)
            {
                prv->stat.req_miss_op++;
            }
            if ((dr->offset + dr->size) != offset)
            {
                prv->stat.req_miss_ofs++;
            }
            if (((unsigned long)dr->buf + dr->size) != (unsigned long)treq.buf)
            {
                prv->stat.req_miss_buf++;
            }
        }

        if (!merged || (size != (11 * 4096)) ||
                (dr->size >= MAX_MERGE_SIZE) ||
                (dr->treq_count == MAX_OPENVSTORAGE_MERGED_REQS))
        {
            tdopenvstorage_submit_aio_request(prv, dr);
            prv->req_deferred = NULL;

            if (!merged)
            {
                prv->stat.req_issued_no_merge++;
            }
            else
            {
                prv->stat.req_issued_forced++;
            }
        }
    }

    if (!merged)
    {
        if (prv->reqs_free_count == 0)
        {
            td_complete_request(treq, -EBUSY);
            goto no_free_reqs;
        }

        req = list_entry(prv->reqs_free.next,
                         struct tdopenvstorage_request,
                         queue);
        list_del(&req->queue);
        prv->reqs_free_count--;

        req->treq_count = 1;
        req->treq[0] = treq;
        req->op = treq.op;
        req->offset = offset;
        req->size = size;
        req->buf = treq.buf;

        if ((size == (11 * 4096)) && (size < MAX_MERGE_SIZE))
        {
            prv->req_deferred = req;
        }
        else
        {
            tdopenvstorage_submit_aio_request(prv, req);
            prv->stat.req_issued_direct++;
        }
    }
no_free_reqs:
    if (prv->req_deferred && (prv->timeout_event_id == -1))
    {
        prv->timeout_event_id = tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT,
                                                              -1,
                                                              0,
                                                              tdopenvstorage_timeout_cb,
                                                              prv);
    }
    else if (!prv->req_deferred && (prv->timeout_event_id != -1))
    {
        tapdisk_server_unregister_event(prv->timeout_event_id);
        prv->timeout_event_id = -1;
    }
}

static int tdopenvstorage_get_parent_id(td_driver_t *driver,
                                        td_disk_id_t *id)
{
    return TD_NO_PARENT;
}

static int tdopenvstorage_validate_parent(td_driver_t *driver,
                                          td_driver_t *parent,
                                          td_flag_t flags)
{
    return -EINVAL;
}

static void tdopenvstorage_stats(td_driver_t *driver, td_stats_t *st)
{
    struct tdopenvstorage_data *prv = driver->data;
    tapdisk_stats_field(st, "req_free_count", "d", prv->reqs_free_count);
    tapdisk_stats_field(st, "req_total", "d", prv->stat.req_total);
    tapdisk_stats_field(st, "req_issued", "d", prv->stat.req_issued);
    tapdisk_stats_field(st, "req_issued_no_merge", "d", prv->stat.req_issued_no_merge);
    tapdisk_stats_field(st, "req_issued_forced", "d", prv->stat.req_issued_forced);
    tapdisk_stats_field(st, "req_issued_direct", "d", prv->stat.req_issued_direct);
    tapdisk_stats_field(st, "req_issued_timeout", "d", prv->stat.req_issued_timeout);
    tapdisk_stats_field(st, "req_miss", "d", prv->stat.req_miss);
    tapdisk_stats_field(st, "req_miss_op", "d", prv->stat.req_miss_op);
    tapdisk_stats_field(st, "req_miss_ofs", "d", prv->stat.req_miss_ofs);
    tapdisk_stats_field(st, "req_miss_buf", "d", prv->stat.req_miss_buf);
    tapdisk_stats_field(st, "max_merge_size", "d", MAX_MERGE_SIZE);
}

struct tap_disk tapdisk_openvstorage = {
    .disk_type = "tapdisk_openvstorage",
    .private_data_size = sizeof(struct tdopenvstorage_data),
    .flags = 0,
    .td_open = tdopenvstorage_open,
    .td_close= tdopenvstorage_close,
    .td_queue_read = tdopenvstorage_queue_request,
    .td_queue_write = tdopenvstorage_queue_request,
    .td_get_parent_id = tdopenvstorage_get_parent_id,
    .td_validate_parent = tdopenvstorage_validate_parent,
    .td_debug = NULL,
    .td_stats = tdopenvstorage_stats,
};
