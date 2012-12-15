#include <v8.h>
#include <node.h>
#include <node_version.h>
#include <stdlib.h>
#include <list>

#if !(NODE_MAJOR_VERSION == 0 && NODE_MINOR_VERSION < 8)

#include "context.hpp"

using namespace node;
using namespace v8;

static struct gcontext g_context;
static uv_prepare_t prepare_handle;
static uv_check_t check_handle;

GContext::GContext() {}

void GContext::Init()
{
	GMainContext *gc = g_main_context_default();

	struct gcontext *ctx = &g_context;

	if (!g_thread_supported())
		g_thread_init(NULL);

	g_main_context_acquire(gc);
	ctx->gc = g_main_context_ref(gc);
	ctx->fds = NULL;

	/* Prepare */
	uv_prepare_init(uv_default_loop(), &prepare_handle);
	uv_prepare_start(&prepare_handle, prepare_cb);

	/* Check */
	uv_check_init(uv_default_loop(), &check_handle);
	uv_check_start(&check_handle, check_cb);
}

void GContext::Uninit()
{
	struct gcontext *ctx = &g_context;

	/* Remove all handlers */
	std::list<poll_handler>::iterator phandler = ctx->poll_handlers.begin();
	while(phandler != ctx->poll_handlers.end()) {

		/* Stop polling handler */
		uv_unref((uv_handle_t *)phandler->pt);
		uv_poll_stop(phandler->pt);
		uv_close((uv_handle_t *)phandler->pt, (uv_close_cb)free);

		delete phandler->pollfd;

		phandler = ctx->poll_handlers.erase(phandler);
	}

	uv_unref((uv_handle_t *) &check_handle);
	uv_check_stop(&check_handle);
	uv_close((uv_handle_t *)&check_handle, NULL);

	uv_unref((uv_handle_t *) &prepare_handle);
	uv_prepare_stop(&prepare_handle);
	uv_close((uv_handle_t *)&prepare_handle, NULL);

	g_free(ctx->fds);

	/* Release GMainContext loop */
	g_main_context_unref(ctx->gc);
}

void GContext::poll_cb(uv_poll_t *handle, int status, int events)
{
	struct gcontext *ctx = &g_context;

	struct gcontext_pollfd *_pfd = (struct gcontext_pollfd *)handle->data;

	GPollFD *pfd = _pfd->pfd;

	pfd->revents |= pfd->events & ((events & UV_READABLE ? G_IO_IN : 0) | (events & UV_WRITABLE ? G_IO_OUT : 0));
}

void GContext::prepare_cb(uv_prepare_t *handle, int status)
{
	gint i;
	gint timeout;
	struct gcontext *ctx = &g_context;

	g_main_context_prepare(ctx->gc, &ctx->max_priority);

	/* Getting all sources from GLib main context */
	while(ctx->allocated_nfds < (ctx->nfds = g_main_context_query(ctx->gc,
			ctx->max_priority,
			&timeout,
			ctx->fds,
			ctx->allocated_nfds))) { 

		g_free(ctx->fds);

		ctx->allocated_nfds = ctx->nfds;

		ctx->fds = g_new(GPollFD, ctx->allocated_nfds);
	}

	/* Poll */
	if (ctx->nfds || timeout != 0) {
		/* Reduce reference count of handler */
		for (std::list<poll_handler>::iterator phandler = ctx->poll_handlers.begin(); phandler != ctx->poll_handlers.end(); ++phandler) {
			phandler->ref--;
		}

		/* Process current file descriptors from GContext */
		for (i = 0; i < ctx->nfds; ++i) {
			GPollFD *pfd = ctx->fds + i;

			pfd->revents = 0;

			/* Finding this file descriptor in list */
			bool exists = false;
			for (std::list<poll_handler>::iterator phandler = ctx->poll_handlers.begin(); phandler != ctx->poll_handlers.end(); ++phandler) {
				if (phandler->fd == pfd->fd) {
					/* Update GPollFD */
					phandler->pollfd->pfd = pfd;
					phandler->ref++;
					exists = true;
					break;
				}
			}

			if (exists)
				continue;

			/* Preparing poll handler */
			struct poll_handler *phandler = new poll_handler;
			struct gcontext_pollfd *pollfd = new gcontext_pollfd;
			pollfd->pfd = pfd;
			phandler->fd = pfd->fd;
			phandler->pollfd = pollfd;
			phandler->ref = 1;

			/* Create uv poll handler, then append own poll handler on it */
			uv_poll_t *pt = new uv_poll_t;
			pt->data = pollfd;
			phandler->pt = pt;

			uv_poll_init(uv_default_loop(), pt, pfd->fd);
			uv_poll_start(pt, UV_READABLE | UV_WRITABLE, poll_cb);

			ctx->poll_handlers.push_back(*phandler);
		}

		/* Remove handlers which aren't required */
		std::list<poll_handler>::iterator phandler = ctx->poll_handlers.begin();
		while(phandler != ctx->poll_handlers.end()) {
			if (phandler->ref == 0) {
				printf("Remove %d\n", phandler->fd);

				uv_unref((uv_handle_t *)phandler->pt);
				uv_poll_stop(phandler->pt);
				uv_close((uv_handle_t *)phandler->pt, (uv_close_cb)free);

				delete phandler->pollfd;

				phandler = ctx->poll_handlers.erase(phandler);

				continue;
			}

			++phandler;
		}
	}
}

void GContext::check_cb(uv_check_t *handle, int status)
{
	struct gcontext *ctx = &g_context;

	g_main_context_check(ctx->gc, ctx->max_priority, ctx->fds, ctx->nfds);
	g_main_context_dispatch(ctx->gc);
}

#endif
