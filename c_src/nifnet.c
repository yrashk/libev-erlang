#include "erl_nif.h"
#include "ev.h"
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

static ErlNifResourceType* nifnet_resource_context;
static ErlNifResourceType* nifnet_resource_socket;

typedef enum _ipc_msg_type {
    STOP_CONTEXT,
    REGISTER_SOCKET,
    CLOSE_SOCKET,
    DEFERRED_SEND,
    DEFERRED_RECV,
    DEFERRED_ACCEPT,
} ipc_msg_type;

typedef enum _socket_state {
    LISTENING,
    CONNECTED
} socket_state;

#define BUFSIZE 131072

typedef struct _nifnet_context
{
    int ipc_push;
    int ipc_pull;
    struct ev_loop * loop_ctx;
    int running;
    ErlNifCond * cond;
    ErlNifMutex * mutex;
    ErlNifTid event_loop_tid;
} nifnet_context;

#define BUFINIT(b) do {\
        b.data = NULL; \
        b.size = b.pos = 0; } while (0)
#define BUFPTR(b) (void*)((char*)b.data+b.pos)
#define BUFCLEAR(b) while (b.data != NULL) { \
        free(b.data); \
        b.data = NULL; \
        b.size = b.pos = 0; }
#define BUFALLOC(b, s) do { \
        b.data = malloc(s); \
        b.size = s; \
        b.pos = 0; } while (0)
#define BUFEXTEND(b, s) do { \
        b.data = realloc(b.data, b.size+s); \
        b.size += s; } while (0)
#define BUFAVAIL(b) (b.size-b.pos)
#define BUFCONSUME(b, size) do { \
        memmove(b.data, (void*)((char*)b.data+size), b.pos-size); \
        b.pos = 0; } while(0)

#define MAX(a, b) (a > b ? a : b)
#define MIN(a, b) (a < b ? a : b)

typedef struct _nifnet_buffer
{
    void *data;
    int size;
    int pos;
} nifnet_buffer;

typedef struct _nifnet_socket
{
    int fd;
    ev_io io;
    nifnet_context * ctx;
    socket_state state;
    int to_recv;
    ErlNifPid recver_pid;
    ErlNifPid sender_pid;
    nifnet_buffer recv_buf;
    nifnet_buffer send_buf;
} nifnet_socket;

typedef struct _nifnet_msg
{
    ipc_msg_type type;
    nifnet_socket * socket;
} nifnet_msg;

// Helper macros
#define ERROR_TUPLE(err) \
    enif_make_tuple2(env, enif_make_atom(env, "error"),\
                          enif_make_int(env, err))
#define OK_TUPLE(r) enif_make_tuple2(env, enif_make_atom(env, "ok"), r)
#define ATOM(a) enif_make_atom(env, a)
#define IPC(ctx, mtype, handle) do { \
        nifnet_msg msg; \
        msg.type = mtype; \
        msg.socket = handle; \
        send(ctx->ipc_push, &msg, sizeof(msg), 0); \
    } while (0)

// Prototypes
#define NIF(name) ERL_NIF_TERM name(ErlNifEnv* env, int argc,\
                                    const ERL_NIF_TERM argv[])

NIF(nifnet_start);
NIF(nifnet_stop);
NIF(nifnet_connect);
NIF(nifnet_send);
NIF(nifnet_recv);
NIF(nifnet_listen);
NIF(nifnet_accept);
NIF(nifnet_close);

static ErlNifFunc nif_funcs[] =
{
    {"start", 0, nifnet_start},
    {"stop", 1, nifnet_stop},
    {"connect", 3, nifnet_connect},
    {"send", 2, nifnet_send},
    {"recv", 2, nifnet_recv},
    {"listen", 3, nifnet_listen},
    {"accept", 1, nifnet_accept},
    {"close", 1, nifnet_close},
};

// Helper functions

static void socket_set_blocking(int fd, int block)
{
    int flags = fcntl(fd, F_GETFL);
    if (block)
        flags &= (~O_NONBLOCK);
    else
        flags |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}

void * event_loop(void * handle);
NIF(nifnet_start)
{
    int fd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0) {
        return ERROR_TUPLE(errno);
    }

    nifnet_context* handle =
            enif_alloc_resource(nifnet_resource_context,
                                sizeof(nifnet_context));
    handle->running = 0;
    handle->ipc_push = fd[0];
    handle->ipc_pull = fd[1];
    socket_set_blocking(handle->ipc_pull, 0);

    handle->mutex = enif_mutex_create("nifnet_mutex");
    handle->cond = enif_cond_create("nifnet_cond");

    enif_mutex_lock(handle->mutex);

    int err;
    if ((err = enif_thread_create("nifnet_event_loop",
                                  &handle->event_loop_tid,
                                  event_loop, handle, NULL))) {
        enif_mutex_unlock(handle->mutex);
        enif_mutex_destroy(handle->mutex);
        enif_cond_destroy(handle->cond);
        enif_release_resource(handle);
        return ERROR_TUPLE(err);
    }
    while (handle->running == 0) {
        enif_cond_wait(handle->cond, handle->mutex);
    }
    enif_mutex_unlock(handle->mutex);

    ERL_NIF_TERM result = enif_make_resource(env, handle);

    return OK_TUPLE(result);
}

NIF(nifnet_send) {
    nifnet_socket * socket;
    ErlNifBinary bin;

    if (!enif_get_resource(env, argv[0], nifnet_resource_socket,
                                (void **)&socket)) {
        return enif_make_badarg(env);
    }
    if (!enif_inspect_iolist_as_binary(env, argv[1], &bin)) {
        return enif_make_badarg(env);
    }

    int s = send(socket->fd, bin.data, bin.size, 0);
    if (s < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            goto deferred;
        }
        return ERROR_TUPLE(errno);
    }
    if (s == bin.size) {
        return enif_make_atom(env, "ok");
    }
deferred:
    enif_self(env, &socket->sender_pid);
    BUFALLOC(socket->send_buf, bin.size-s);
    memcpy(socket->send_buf.data, (void*)((char*)bin.data+s), bin.size-s);
    IPC(socket->ctx, DEFERRED_SEND, socket);
    return enif_make_atom(env, "deferred");
}

NIF(nifnet_recv) {
    nifnet_socket * socket;

    if (!enif_get_resource(env, argv[0], nifnet_resource_socket,
                                (void **)&socket)) {
        return enif_make_badarg(env);
    }

    unsigned int wanted;
    if (!enif_get_uint(env, argv[1], &wanted)) {
        enif_make_badarg(env);
    }

    ERL_NIF_TERM result;
    int to_recv = wanted ? wanted : BUFSIZE;
    to_recv = MIN(to_recv, BUFSIZE);

    void *data = enif_make_new_binary(env, to_recv, &result);

    int r = recv(socket->fd, data, to_recv, 0);
    if ((r > 0 && wanted == 0) || (r == wanted)) {
        return enif_make_tuple2(env, enif_make_atom(env, "ok"), result);
    }
    if (r == 0) {
        return enif_make_tuple2(env, enif_make_atom(env, "error"),
                                     enif_make_atom(env, "disconnected"));
    }
    if (r > 0) {
        int avail = BUFAVAIL(socket->recv_buf);
        if (avail < r)
            BUFEXTEND(socket->recv_buf, r-avail);
        memcpy(BUFPTR(socket->recv_buf), data, r);
        socket->recv_buf.pos += r;
        goto deferred;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        goto deferred;
    }
    return enif_make_tuple2(env, enif_make_atom(env, "error"),
                                 enif_make_int(env, errno));
deferred:
    socket->to_recv = wanted;
    enif_self(env, &socket->recver_pid);
    IPC(socket->ctx, DEFERRED_RECV, socket);
    return enif_make_atom(env, "deferred");
}

NIF(nifnet_connect)
{
    nifnet_context * ctx;

    if (!enif_get_resource(env, argv[0], nifnet_resource_context,
                                (void **)&ctx)) {
        return enif_make_badarg(env);
    }
    if (!ctx->running) {
        return enif_make_badarg(env);
    }

    int arity = 0;
    const ERL_NIF_TERM *tuple;
    if (!enif_get_tuple(env, argv[1], &arity, &tuple)) {
        return enif_make_badarg(env);
    }
    if (arity != 4) {
        return enif_make_badarg(env);
    }
    in_addr_t addr = 0;
    int i;
    int shift = 24;
    for (i = 0; i < 4; i++, shift -=8) {
        unsigned int x;
        if (!enif_get_uint(env, tuple[i], &x)) {
            return enif_make_badarg(env);
        }
        addr |= ((x&0xFF)<<shift);
    }

    unsigned int port;
    if (!enif_get_uint(env, argv[2], &port)) {
        return enif_make_badarg(env);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return ERROR_TUPLE(errno);
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(addr);
    saddr.sin_port = htons(port);
    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        close(sock);
        return ERROR_TUPLE(errno);
    }
    socket_set_blocking(sock, 0);

    nifnet_socket * handle = enif_alloc_resource(nifnet_resource_socket,
                                                   sizeof(nifnet_socket));

    handle->fd = sock;
    handle->ctx = ctx;
    handle->state = CONNECTED;
    BUFINIT(handle->recv_buf);
    BUFALLOC(handle->recv_buf, BUFSIZE);
    BUFINIT(handle->send_buf);

    IPC(ctx, REGISTER_SOCKET, handle);

    ERL_NIF_TERM result = enif_make_resource(env, handle);

    return enif_make_tuple2(env, enif_make_atom(env, "ok"), result);
}

NIF(nifnet_listen)
{
    nifnet_context * ctx;

    if (!enif_get_resource(env, argv[0], nifnet_resource_context,
                                (void **)&ctx)) {
        return enif_make_badarg(env);
    }
    if (!ctx->running) {
        return enif_make_badarg(env);
    }

    unsigned int port;
    if (!enif_get_uint(env, argv[1], &port)) {
        return enif_make_badarg(env);
    }

    unsigned int backlog;
    if (!enif_get_uint(env, argv[2], &backlog)) {
        return enif_make_badarg(env);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return ERROR_TUPLE(errno);
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    saddr.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        close(sock);
        return ERROR_TUPLE(errno);
    }
    if (listen(sock, backlog) < 0) {
        close(sock);
        return ERROR_TUPLE(errno);
    }
    socket_set_blocking(sock, 0);

    nifnet_socket * handle = enif_alloc_resource(nifnet_resource_socket,
                                                   sizeof(nifnet_socket));

    handle->fd = sock;
    handle->ctx = ctx;
    handle->state = LISTENING;

    IPC(ctx, REGISTER_SOCKET, handle);

    ERL_NIF_TERM result = enif_make_resource(env, handle);

    return enif_make_tuple2(env, enif_make_atom(env, "ok"), result);
}

NIF(nifnet_accept)
{
    nifnet_socket * socket;

    if (!enif_get_resource(env, argv[0], nifnet_resource_socket,
                                (void **)&socket)) {
        return enif_make_badarg(env);
    }

    if (!socket->ctx->running || socket->fd == -1
                              || socket->state != LISTENING) {
        return enif_make_badarg(env);
    }

    int conn;
    if ((conn = accept(socket->fd, NULL, NULL)) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            enif_self(env, &socket->recver_pid);
            IPC(socket->ctx, DEFERRED_ACCEPT, socket);
            return enif_make_atom(env, "deferred");
        }
        return ERROR_TUPLE(errno);
    }
    nifnet_socket * handle = enif_alloc_resource(nifnet_resource_socket,
                                                   sizeof(nifnet_socket));

    handle->fd = conn;
    handle->ctx = socket->ctx;
    handle->state = CONNECTED;
    BUFINIT(handle->recv_buf);
    BUFALLOC(handle->recv_buf, BUFSIZE);
    BUFINIT(handle->send_buf);

    socket_set_blocking(handle->fd, 0);

    IPC(socket->ctx, REGISTER_SOCKET, handle);

    ERL_NIF_TERM result = enif_make_resource(env, handle);

    return enif_make_tuple2(env, enif_make_atom(env, "ok"), result);
}

NIF(nifnet_close)
{
    nifnet_socket * socket;

    if (!enif_get_resource(env, argv[0], nifnet_resource_socket,
                                (void **)&socket)) {
        return enif_make_badarg(env);
    }


    if (!socket->ctx->running) {
        return enif_make_badarg(env);
    }

    IPC(socket->ctx, CLOSE_SOCKET, socket);
    return enif_make_atom(env, "ok");
}

NIF(nifnet_stop)
{
    nifnet_context * ctx;

    if (!enif_get_resource(env, argv[0], nifnet_resource_context,
                                (void **)&ctx)) {
        return enif_make_badarg(env);
    }

    IPC(ctx, STOP_CONTEXT, NULL);
    enif_thread_join(ctx->event_loop_tid, NULL);
    enif_release_resource(ctx);

    return enif_make_atom(env, "ok");
}

static void socket_event_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    nifnet_socket * socket = (nifnet_socket *)w->data;
    if (revents & EV_READ) {
        ERL_NIF_TERM result;
        ErlNifEnv * env;
        while (1) {
            int avail = BUFAVAIL(socket->recv_buf);
            if (avail < BUFSIZE)
                BUFEXTEND(socket->recv_buf, BUFSIZE-avail);
            int r = recv(socket->fd, BUFPTR(socket->recv_buf), BUFSIZE, 0);
            if (r == 0) {
                // disconnected
                ev_io_stop(loop, w);
                close(socket->fd);
                socket->fd = -1;
                if (socket->recv_buf.pos > 0) {
                    goto return_buffer;
                }
                ErlNifEnv * env = enif_alloc_env();
                enif_send(NULL, &socket->recver_pid, env,
                          enif_make_tuple2(env,
                                   enif_make_atom(env, "deferred_error"),
                                   enif_make_atom(env, "disconnected")));
                enif_free_env(env);
                goto out;
            } else if (r > 0) {
                socket->recv_buf.pos += r;
                if (socket->to_recv > 0 &&
                    socket->recv_buf.pos >= socket->to_recv) {
                    goto return_buffer;
                }
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    goto return_buffer;
                }
                ErlNifEnv * env = enif_alloc_env();
                enif_send(NULL, &socket->recver_pid, env,
                          enif_make_tuple2(env,
                                       enif_make_atom(env, "deferred_error"),
                                       enif_make_int(env, errno)));
                enif_free_env(env);
                goto out;
            }
        }
return_buffer:
        env = enif_alloc_env();
        memcpy(enif_make_new_binary(env, socket->recv_buf.pos, &result),
               socket->recv_buf.data, socket->recv_buf.pos);
        enif_send(NULL, &socket->recver_pid, env,
                  enif_make_tuple2(env, enif_make_atom(env, "deferred_ok"),
                                        result));
        enif_free_env(env);
        socket->recv_buf.pos = 0;
        ev_io_stop(loop, w);
        ev_io_set(w, socket->fd, w->events & (~EV_READ));
        ev_io_start(loop, w);
    }
out:
    if (revents & EV_WRITE) {
        if (socket->send_buf.size > 0) {
            int n, to_send;
            int sent;
            do {
                to_send = MIN(BUFAVAIL(socket->send_buf), BUFSIZE);
                if (to_send == 0) {
                    ev_io_stop(loop, w);
                    ev_io_set(&socket->io, socket->fd,
                              socket->io.events & (~EV_WRITE));
                    ev_io_start(loop, w);
                    BUFCLEAR(socket->send_buf);
                    ErlNifEnv* env = enif_alloc_env();
                    enif_send(NULL, &socket->sender_pid, env,
                              enif_make_atom(env, "deferred_ok"));
                    enif_free_env(env);
                    break;
                }
                n = send(socket->fd, BUFPTR(socket->send_buf), to_send, 0);
                if (n > 0) {
                    socket->send_buf.pos += n;
                    sent += n;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // finish later
                    break;
                } else {
                    ev_io_stop(loop, w);
                    ev_io_set(&socket->io, socket->fd,
                              socket->io.events & (~EV_WRITE));
                    ev_io_start(loop, w);
                    ErlNifEnv * env = enif_alloc_env();
                    enif_send(NULL, &socket->sender_pid, env,
                              enif_make_tuple2(env,
                                        enif_make_atom(env, "deferred_error"),
                                        enif_make_int(env, errno)));
                    enif_free_env(env);
                    break;
                }
            } while (1);
            // } while (sent < SOME_LIMIT); to avoid loop starvation
        }
    }
}

static void accept_event_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    ERL_NIF_TERM result;
    ErlNifEnv * env = enif_alloc_env();
    nifnet_socket * socket = (nifnet_socket *)w->data;
    int conn;
    if ((conn = accept(socket->fd, NULL, NULL)) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            enif_free_env(env);
            return;
        }
        result = enif_make_tuple2(env, enif_make_atom(env, "deferred_error"),
                                       enif_make_int(env, errno));
    } else {
        nifnet_socket * handle =
                    enif_alloc_resource(nifnet_resource_socket,
                                        sizeof(nifnet_socket));

        handle->fd = conn;
        handle->ctx = socket->ctx;
        handle->state = CONNECTED;
        BUFINIT(handle->recv_buf);
        BUFALLOC(handle->recv_buf, BUFSIZE);
        BUFINIT(handle->send_buf);

        socket_set_blocking(handle->fd, 0);

        ev_io_init(&handle->io, socket_event_cb, handle->fd, EV_NONE);
        handle->io.data = handle;
        ev_io_start(loop, &handle->io);

        result = enif_make_tuple2(env, enif_make_atom(env, "deferred_ok"),
                                       enif_make_resource(env, handle));

    }
    enif_send(NULL, &socket->recver_pid, env, result);
    enif_free_env(env);
    ev_io_stop(loop, w);
    ev_io_set(w, socket->fd, w->events & (~EV_READ));
}

static void ipc_event_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    nifnet_context * ctx = (nifnet_context *)w->data;
    nifnet_msg msg;
    recv(ctx->ipc_pull, &msg, sizeof(msg), 0);
    switch (msg.type) {
        case STOP_CONTEXT:
            ctx->running = 0;
            ev_break(loop, EVBREAK_ALL);
            break;
        case REGISTER_SOCKET:
            switch (msg.socket->state) {
                case LISTENING:
                    ev_io_init(&msg.socket->io, accept_event_cb,
                               msg.socket->fd, EV_NONE);
                    msg.socket->io.data = msg.socket;
                    ev_io_start(loop, &msg.socket->io);
                    break;
                case CONNECTED:
                    ev_io_init(&msg.socket->io, socket_event_cb,
                               msg.socket->fd, EV_NONE);
                    msg.socket->io.data = msg.socket;
                    ev_io_start(loop, &msg.socket->io);
                    break;
            }
            break;
        case CLOSE_SOCKET:
            ev_io_stop(loop, &msg.socket->io);
            if (msg.socket->fd != -1) {
                close(msg.socket->fd);
                msg.socket->fd = -1;
            }
            BUFCLEAR(msg.socket->recv_buf);
            BUFCLEAR(msg.socket->send_buf);
            enif_release_resource(&msg.socket);
            break;
        case DEFERRED_ACCEPT:
            ev_io_stop(loop, &msg.socket->io);
            ev_io_set(&msg.socket->io, msg.socket->fd,
                      msg.socket->io.events | EV_READ);
            ev_io_start(loop, &msg.socket->io);
            break;
        case DEFERRED_SEND:
            ev_io_stop(loop, &msg.socket->io);
            ev_io_set(&msg.socket->io, msg.socket->fd,
                      msg.socket->io.events | EV_WRITE);
            ev_io_start(loop, &msg.socket->io);
            break;
        case DEFERRED_RECV:
            ev_io_stop(loop, &msg.socket->io);
            ev_io_set(&msg.socket->io, msg.socket->fd,
                      msg.socket->io.events | EV_READ);
            ev_io_start(loop, &msg.socket->io);
            break;
    }
}

void * event_loop(void * handle)
{
    nifnet_context * ctx = (nifnet_context *)handle;

    ctx->loop_ctx = ev_loop_new(EVFLAG_AUTO);

    ev_io ipc_watcher;
    ev_io_init(&ipc_watcher, ipc_event_cb, ctx->ipc_pull, EV_READ);
    ev_io_start(ctx->loop_ctx, &ipc_watcher);
    ipc_watcher.data = (void *)ctx;

    enif_mutex_lock(ctx->mutex);
    ctx->running = 1;
    enif_cond_signal(ctx->cond);
    enif_mutex_unlock(ctx->mutex);

    while (ctx->running) {
        ev_run(ctx->loop_ctx, 0);
    }
    ev_loop_destroy(ctx->loop_ctx);
    enif_mutex_destroy(ctx->mutex);
    enif_cond_destroy(ctx->cond);
    return NULL;
}

static int on_load(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info)
{
    nifnet_resource_context =
        enif_open_resource_type(env, "nifnet_nif",
                                     "nifnet_resource_context",
                                     NULL,
                                     ERL_NIF_RT_CREATE|ERL_NIF_RT_TAKEOVER,
                                     0);
    nifnet_resource_socket =
        enif_open_resource_type(env, "nifnet_nif",
                                     "nifnet_resource_socket",
                                     NULL,
                                     ERL_NIF_RT_CREATE|ERL_NIF_RT_TAKEOVER,
                                     0);
    return 0;
}

ERL_NIF_INIT(nifnet_nif, nif_funcs, &on_load, NULL, NULL, NULL);

