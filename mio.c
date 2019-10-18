/*
 * Copyright 2000-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright 2007-2012 Niels Provos, Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "event2/event-config.h"
#include "evconfig-private.h"

#ifdef EVENT__HAVE_MIO

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef EVENT__HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef EVENT__HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef EVENT__HAVE_SYS_TIMERFD_H
#include <sys/timerfd.h>
#endif

#include "event-internal.h"
#include "evsignal-internal.h"
#include "event2/thread.h"
#include "evthread-internal.h"
#include "log-internal.h"
#include "evmap-internal.h"
#include "changelist-internal.h"
#include "time-internal.h"

//#include "epolltable-internal.h"


struct mioop {
//	struct mio_event *events;
//	int nevents;
//	int miofd;
  void *poll_handle;
};

static void *epoll_init(struct event_base *);
static int epoll_dispatch(struct event_base *, struct timeval *);
static void epoll_dealloc(struct event_base *);

const struct eventop mioops = {
	"mio",
	mio_init,
	mio_nochangelist_add,
	mio_nochangelist_del,
	mio_dispatch,
	mio_dealloc,
	1, /* need reinit */
	EV_FEATURE_ET|EV_FEATURE_O1|EV_FEATURE_EARLY_CLOSE,
	0
};


static void *
epoll_init(struct event_base *base)
{
//	int miofd = -1;
	struct mioop *mioop;


	if (!(mioop = mm_calloc(1, sizeof(struct mioop)))) {
//		close(miofd);
		return (NULL);
	}

//	mioop->miofd = miofd;

#if 0
	/* Initialize fields */
	epollop->events = mm_calloc(INITIAL_NEVENT, sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		mm_free(epollop);
		close(epfd);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENT;
#endif

	if ((base->flags & EVENT_BASE_FLAG_EPOLL_USE_CHANGELIST) != 0 ||
	    ((base->flags & EVENT_BASE_FLAG_IGNORE_ENV) == 0 &&
		evutil_getenv_("EVENT_EPOLL_USE_CHANGELIST") != NULL)) {

		base->evsel = &epollops_changelist;
	}

	evsig_init_(base);

	return (epollop);
}

static const char *
change_to_string(int change)
{
	change &= (EV_CHANGE_ADD|EV_CHANGE_DEL);
	if (change == EV_CHANGE_ADD) {
		return "add";
	} else if (change == EV_CHANGE_DEL) {
		return "del";
	} else if (change == 0) {
		return "none";
	} else {
		return "???";
	}
}

static const char *
epoll_op_to_string(int op)
{
	return op == EPOLL_CTL_ADD?"ADD":
	    op == EPOLL_CTL_DEL?"DEL":
	    op == EPOLL_CTL_MOD?"MOD":
	    "???";
}

#define PRINT_CHANGES(op, events, ch, status)  \
	"Epoll %s(%d) on fd %d " status ". "       \
	"Old events were %d; "                     \
	"read change was %d (%s); "                \
	"write change was %d (%s); "               \
	"close change was %d (%s)",                \
	epoll_op_to_string(op),                    \
	events,                                    \
	ch->fd,                                    \
	ch->old_events,                            \
	ch->read_change,                           \
	change_to_string(ch->read_change),         \
	ch->write_change,                          \
	change_to_string(ch->write_change),        \
	ch->close_change,                          \
	change_to_string(ch->close_change)

static int
epoll_apply_one_change(struct event_base *base,
    struct epollop *epollop,
    const struct event_change *ch)
{
	struct epoll_event epev;
	int op, events = 0;
	int idx;

	idx = EPOLL_OP_TABLE_INDEX(ch);
	op = epoll_op_table[idx].op;
	events = epoll_op_table[idx].events;

	if (!events) {
		EVUTIL_ASSERT(op == 0);
		return 0;
	}

	if ((ch->read_change|ch->write_change) & EV_CHANGE_ET)
		events |= EPOLLET;

	memset(&epev, 0, sizeof(epev));
	epev.data.fd = ch->fd;
	epev.events = events;
	if (epoll_ctl(epollop->epfd, op, ch->fd, &epev) == 0) {
		event_debug((PRINT_CHANGES(op, epev.events, ch, "okay")));
		return 0;
	}

	switch (op) {
	case EPOLL_CTL_MOD:
		if (errno == ENOENT) {
			/* If a MOD operation fails with ENOENT, the
			 * fd was probably closed and re-opened.  We
			 * should retry the operation as an ADD.
			 */
			if (epoll_ctl(epollop->epfd, EPOLL_CTL_ADD, ch->fd, &epev) == -1) {
				event_warn("Epoll MOD(%d) on %d retried as ADD; that failed too",
				    (int)epev.events, ch->fd);
				return -1;
			} else {
				event_debug(("Epoll MOD(%d) on %d retried as ADD; succeeded.",
					(int)epev.events,
					ch->fd));
				return 0;
			}
		}
		break;
	case EPOLL_CTL_ADD:
		if (errno == EEXIST) {
			/* If an ADD operation fails with EEXIST,
			 * either the operation was redundant (as with a
			 * precautionary add), or we ran into a fun
			 * kernel bug where using dup*() to duplicate the
			 * same file into the same fd gives you the same epitem
			 * rather than a fresh one.  For the second case,
			 * we must retry with MOD. */
			if (epoll_ctl(epollop->epfd, EPOLL_CTL_MOD, ch->fd, &epev) == -1) {
				event_warn("Epoll ADD(%d) on %d retried as MOD; that failed too",
				    (int)epev.events, ch->fd);
				return -1;
			} else {
				event_debug(("Epoll ADD(%d) on %d retried as MOD; succeeded.",
					(int)epev.events,
					ch->fd));
				return 0;
			}
		}
		break;
	case EPOLL_CTL_DEL:
		if (errno == ENOENT || errno == EBADF || errno == EPERM) {
			/* If a delete fails with one of these errors,
			 * that's fine too: we closed the fd before we
			 * got around to calling epoll_dispatch. */
			event_debug(("Epoll DEL(%d) on fd %d gave %s: DEL was unnecessary.",
				(int)epev.events,
				ch->fd,
				strerror(errno)));
			return 0;
		}
		break;
	default:
		break;
	}

	event_warn(PRINT_CHANGES(op, epev.events, ch, "failed"));
	return -1;
}

static int
epoll_apply_changes(struct event_base *base)
{
	struct event_changelist *changelist = &base->changelist;
	struct epollop *epollop = base->evbase;
	struct event_change *ch;

	int r = 0;
	int i;

	for (i = 0; i < changelist->n_changes; ++i) {
		ch = &changelist->changes[i];
		if (epoll_apply_one_change(base, epollop, ch) < 0)
			r = -1;
	}

	return (r);
}

static int
epoll_nochangelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *p)
{
	struct event_change ch;
	ch.fd = fd;
	ch.old_events = old;
	ch.read_change = ch.write_change = ch.close_change = 0;
	if (events & EV_WRITE)
		ch.write_change = EV_CHANGE_ADD |
		    (events & EV_ET);
	if (events & EV_READ)
		ch.read_change = EV_CHANGE_ADD |
		    (events & EV_ET);
	if (events & EV_CLOSED)
		ch.close_change = EV_CHANGE_ADD |
		    (events & EV_ET);

	return epoll_apply_one_change(base, base->evbase, &ch);
}

static int
epoll_nochangelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *p)
{
	struct event_change ch;
	ch.fd = fd;
	ch.old_events = old;
	ch.read_change = ch.write_change = ch.close_change = 0;
	if (events & EV_WRITE)
		ch.write_change = EV_CHANGE_DEL;
	if (events & EV_READ)
		ch.read_change = EV_CHANGE_DEL;
	if (events & EV_CLOSED)
		ch.close_change = EV_CHANGE_DEL;

	return epoll_apply_one_change(base, base->evbase, &ch);
}

static int
epoll_dispatch(struct event_base *base, struct timeval *tv)
{
	struct epollop *epollop = base->evbase;
	struct epoll_event *events = epollop->events;
	int i, res;
	long timeout = -1;

	if (tv != NULL) {
		timeout = evutil_tv_to_msec_(tv);
		if (timeout < 0 || timeout > MAX_EPOLL_TIMEOUT_MSEC) {
			/* Linux kernels can wait forever if the timeout is
			 * too big; see comment on MAX_EPOLL_TIMEOUT_MSEC. */
			timeout = MAX_EPOLL_TIMEOUT_MSEC;
		}
	}

	epoll_apply_changes(base);
	event_changelist_remove_all_(&base->changelist, base);

	EVBASE_RELEASE_LOCK(base, th_base_lock);

	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}

		return (0);
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));
	EVUTIL_ASSERT(res <= epollop->nevents);

	for (i = 0; i < res; i++) {
		int what = events[i].events;
		short ev = 0;
#ifdef USING_TIMERFD
		if (events[i].data.fd == epollop->timerfd)
			continue;
#endif

		if (what & (EPOLLHUP|EPOLLERR)) {
			ev = EV_READ | EV_WRITE;
		} else {
			if (what & EPOLLIN)
				ev |= EV_READ;
			if (what & EPOLLOUT)
				ev |= EV_WRITE;
			if (what & EPOLLRDHUP)
				ev |= EV_CLOSED;
		}

		if (!ev)
			continue;

		evmap_io_active_(base, events[i].data.fd, ev | EV_ET);
	}

	if (res == epollop->nevents && epollop->nevents < MAX_NEVENT) {
		/* We used all of the event space this time.  We should
		   be ready for more events next time. */
		int new_nevents = epollop->nevents * 2;
		struct epoll_event *new_events;

		new_events = mm_realloc(epollop->events,
		    new_nevents * sizeof(struct epoll_event));
		if (new_events) {
			epollop->events = new_events;
			epollop->nevents = new_nevents;
		}
	}

	return (0);
}


static void
epoll_dealloc(struct event_base *base)
{
	struct mioop *mioop = base->evbase;

#if 0
	if (epollop->events)
		mm_free(epollop->events);
	if (epollop->epfd >= 0)
		close(epollop->epfd);
#endif

	memset(mioop, 0, sizeof(struct mioop));
	mm_free(mioop);
}

#endif /* EVENT__HAVE_EPOLL */
