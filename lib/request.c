/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stdio.h>
#include <stdlib.h>
#include "h2o.h"

#define INITIAL_INBUFSZ 8192

static void deferred_proceed_cb(h2o_timeout_entry_t *entry)
{
    h2o_req_t *req = H2O_STRUCT_FROM_MEMBER(h2o_req_t, _timeout_entry, entry);
    h2o_proceed_response(req);
}

void h2o_init_request(h2o_req_t *req, h2o_conn_t *conn, h2o_req_t *src)
{
    /* clear all memory (expect memory pool, since it is large) */
    memset(req, 0, offsetof(h2o_req_t, pool));

    /* init memory pool (before others, since it may be used) */
    h2o_mempool_init(&req->pool);

    /* init properties that should be initialized to non-zero */
    req->conn = conn;
    req->_timeout_entry.cb = deferred_proceed_cb;
    req->res.content_length = SIZE_MAX;

    if (src != NULL) {
#define COPY(buf) do { \
    req->buf.base = h2o_mempool_alloc(&req->pool, src->buf.len); \
    memcpy(req->buf.base, src->buf.base, src->buf.len); \
    req->buf.len = src->buf.len; \
} while (0)
        COPY(authority);
        COPY(method);
        COPY(path);
        COPY(scheme);
        req->version = src->version;
        h2o_vector_reserve(&req->pool, (h2o_vector_t*)&req->headers, sizeof(h2o_header_t), src->headers.size);
        memcpy(req->headers.entries, src->headers.entries, sizeof(req->headers.entries[0]) * src->headers.size);
        req->headers.size = src->headers.size;
        req->entity = src->entity;
        req->http1_is_persistent = src->http1_is_persistent;
        if (src->upgrade.base != NULL) {
            COPY(upgrade);
        } else {
            req->upgrade.base = NULL;
            req->upgrade.len = 0;
        }
#undef COPY
    }
}

void h2o_dispose_request(h2o_req_t *req)
{
    /* close the generator if it is still open */
    if (req->_generator != NULL) {
        /* close generator */
        if (req->_generator->stop != NULL)
            req->_generator->stop(req->_generator, req);
        req->_generator = NULL;
    }
    /* close the ostreams still open */
    while (req->_ostr_top->next != NULL) {
        if (req->_ostr_top->stop != NULL)
            req->_ostr_top->stop(req->_ostr_top, req);
        req->_ostr_top = req->_ostr_top->next;
    }

    h2o_timeout_unlink(&req->_timeout_entry);

    if (req->version != 0 && req->host_config != NULL) {
        h2o_logger_t **logger = req->host_config->loggers.entries, **end = logger + req->host_config->loggers.size;
        for (; logger != end; ++logger) {
            (*logger)->log_access((*logger), req);
        }
    }

    h2o_mempool_clear(&req->pool);
}

void h2o_process_request(h2o_req_t *req)
{
    h2o_context_t *ctx = req->conn->ctx;

    h2o_get_timestamp(ctx, &req->pool, &req->processed_at);

    /* setup host context */
    req->host_config = ctx->global_config->hosts.entries;
    if (req->authority.base != NULL) {
        if (ctx->global_config->hosts.size != 1) {
            h2o_hostconf_t *hostconf = ctx->global_config->hosts.entries, *end = hostconf + ctx->global_config->hosts.size;
            for (; hostconf != end; ++hostconf) {
                if (h2o_memis(req->authority.base, req->authority.len, hostconf->hostname.base, hostconf->hostname.len)) {
                    req->host_config = hostconf;
                    break;
                }
            }
        }
    } else {
        /* set the authority name to the default one */
        req->authority = req->host_config->hostname;
    }

    { /* call any of the handlers */
        h2o_handler_t **handler = req->host_config->handlers.entries, **end = handler + req->host_config->handlers.size;
        for (; handler != end; ++handler) {
            if ((*handler)->on_req(*handler, req) == 0)
                return;
        }
    }

    h2o_send_error(req, 404, "File Not Found", "not found");
}

void h2o_start_response(h2o_req_t *req, h2o_generator_t *generator)
{
    /* set generator */
    assert(req->_generator == NULL);
    req->_generator = generator;

    /* setup response filters */
    if (req->host_config->filters.size != 0) {
        h2o_filter_t *filter = req->host_config->filters.entries[0];
        filter->on_setup_ostream(filter, req, &req->_ostr_top);
    }
}

void h2o_send(h2o_req_t *req, h2o_iovec_t *bufs, size_t bufcnt, int is_final)
{
    size_t i;

    assert(req->_generator != NULL);

    if (is_final)
        req->_generator = NULL;

    for (i = 0; i != bufcnt; ++i)
        req->bytes_sent += bufs[i].len;

    req->_ostr_top->do_send(req->_ostr_top, req, bufs, bufcnt, is_final);
}


h2o_ostream_t *h2o_add_ostream(h2o_req_t *req, size_t sz, h2o_ostream_t **slot)
{
    h2o_ostream_t *ostr = h2o_mempool_alloc(&req->pool, sz);
    ostr->next = *slot;
    ostr->do_send = NULL;
    ostr->stop = NULL;
    ostr->start_pull = NULL;

    *slot = ostr;

    return ostr;
}

void h2o_ostream_send_next(h2o_ostream_t *ostream, h2o_req_t *req, h2o_iovec_t *bufs, size_t bufcnt, int is_final)
{
    if (is_final) {
        assert(req->_ostr_top == ostream);
        req->_ostr_top = ostream->next;
    } else if (bufcnt == 0) {
        h2o_timeout_link(req->conn->ctx->loop, &req->conn->ctx->zero_timeout, &req->_timeout_entry);
        return;
    }
    ostream->next->do_send(ostream->next, req, bufs, bufcnt, is_final);
}
