/*
    Copyright (c) 2007-2009 FastMQ Inc.

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../bindings/c/zmq.h"

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <new>

#include "socket_base.hpp"
#include "app_thread.hpp"
#include "dispatcher.hpp"
#include "msg_content.hpp"
#include "platform.hpp"
#include "stdint.hpp"
#include "err.hpp"

#if defined ZMQ_HAVE_LINUX
#include <poll.h>
#endif

#if !defined ZMQ_HAVE_WINDOWS
#include <unistd.h>
#include <sys/time.h>
#endif

const char *zmq_strerror (int errnum_)
{
    switch (errnum_) {
#if defined ZMQ_HAVE_WINDOWS
    case ENOTSUP:
        return "Not supported";
    case EPROTONOSUPPORT:
        return "Protocol not supported";
    case ENOBUFS:
        return "No buffer space available";
    case ENETDOWN:
        return "Network is down";
    case EADDRINUSE:
        return "Address in use";
    case EADDRNOTAVAIL:
        return "Address not available";
#endif
    case EMTHREAD:
        return "Number of preallocated application threads exceeded";
    case EFSM:
        return "Operation cannot be accomplished in current state";
    case ENOCOMPATPROTO:
        return "The protocol is not compatible with the socket type";
    default:
#if defined _MSC_VER
#pragma warning (push)
#pragma warning (disable:4996)
#endif
        return strerror (errnum_);
#if defined _MSC_VER
#pragma warning (pop)
#endif
    }
}

int zmq_msg_init (zmq_msg_t *msg_)
{
    msg_->content = (zmq::msg_content_t*) ZMQ_VSM;
    msg_->vsm_size = 0;
    return 0;
}

int zmq_msg_init_size (zmq_msg_t *msg_, size_t size_)
{
    if (size_ <= ZMQ_MAX_VSM_SIZE) {
        msg_->content = (zmq::msg_content_t*) ZMQ_VSM;
        msg_->vsm_size = (uint8_t) size_;
    }
    else {
        msg_->content =
            (zmq::msg_content_t*) malloc (sizeof (zmq::msg_content_t) + size_);
        if (!msg_->content) {
            errno = ENOMEM;
            return -1;
        }
        msg_->shared = 0;

        zmq::msg_content_t *content = (zmq::msg_content_t*) msg_->content;
        content->data = (void*) (content + 1);
        content->size = size_;
        content->ffn = NULL;
        new (&content->refcnt) zmq::atomic_counter_t ();
    }
    return 0;
}

int zmq_msg_init_data (zmq_msg_t *msg_, void *data_, size_t size_,
    zmq_free_fn *ffn_)
{
    msg_->shared = 0;
    msg_->content = (zmq::msg_content_t*) malloc (sizeof (zmq::msg_content_t));
    zmq_assert (msg_->content);
    zmq::msg_content_t *content = (zmq::msg_content_t*) msg_->content;
    content->data = data_;
    content->size = size_;
    content->ffn = ffn_;
    new (&content->refcnt) zmq::atomic_counter_t ();
    return 0;
}

int zmq_msg_close (zmq_msg_t *msg_)
{
    //  For VSMs and delimiters there are no resources to free.
    if (msg_->content == (zmq::msg_content_t*) ZMQ_DELIMITER ||
          msg_->content == (zmq::msg_content_t*) ZMQ_VSM)
        return 0;

    //  If the content is not shared, or if it is shared and the reference.
    //  count has dropped to zero, deallocate it.
    zmq::msg_content_t *content = (zmq::msg_content_t*) msg_->content;
    if (!msg_->shared || !content->refcnt.sub (1)) {

        //  We used "placement new" operator to initialize the reference.
        //  counter so we call its destructor now.
        content->refcnt.~atomic_counter_t ();

        if (content->ffn)
            content->ffn (content->data);
        free (content);
    }

    return 0;
}

int zmq_msg_move (zmq_msg_t *dest_, zmq_msg_t *src_)
{
    zmq_msg_close (dest_);
    *dest_ = *src_;
    zmq_msg_init (src_);
    return 0;
}

int zmq_msg_copy (zmq_msg_t *dest_, zmq_msg_t *src_)
{
    zmq_msg_close (dest_);

    //  VSMs and delimiters require no special handling.
    if (src_->content != (zmq::msg_content_t*) ZMQ_DELIMITER &&
          src_->content != (zmq::msg_content_t*) ZMQ_VSM) {

        //  One reference is added to shared messages. Non-shared messages
        //  are turned into shared messages and reference count is set to 2.
        zmq::msg_content_t *content = (zmq::msg_content_t*) src_->content;
        if (src_->shared)
            content->refcnt.add (1);
        else {
            src_->shared = true;
            content->refcnt.set (2);
        }
    }

    *dest_ = *src_;
    return 0;
}

void *zmq_msg_data (zmq_msg_t *msg_)
{
    if (msg_->content == (zmq::msg_content_t*) ZMQ_VSM)
        return msg_->vsm_data;
    if (msg_->content == (zmq::msg_content_t*) ZMQ_DELIMITER)
        return NULL;

    return ((zmq::msg_content_t*) msg_->content)->data;
}

size_t zmq_msg_size (zmq_msg_t *msg_)
{
    if (msg_->content == (zmq::msg_content_t*) ZMQ_VSM)
        return msg_->vsm_size;
    if (msg_->content == (zmq::msg_content_t*) ZMQ_DELIMITER)
        return 0;

    return ((zmq::msg_content_t*) msg_->content)->size;
}

void *zmq_init (int app_threads_, int io_threads_, int flags_)
{
    //  There should be at least a single thread managed by the dispatcher.
    if (app_threads_ <= 0 || io_threads_ <= 0 ||
          app_threads_ > 63 || io_threads_ > 63) {
        errno = EINVAL;
        return NULL;
    }

    zmq::dispatcher_t *dispatcher = new zmq::dispatcher_t (app_threads_,
        io_threads_, flags_);
    zmq_assert (dispatcher);
    return (void*) dispatcher;
}

int zmq_term (void *dispatcher_)
{
    return ((zmq::dispatcher_t*) dispatcher_)->term ();
}

void *zmq_socket (void *dispatcher_, int type_)
{
    return (void*) (((zmq::dispatcher_t*) dispatcher_)->create_socket (type_));
}

int zmq_close (void *s_)
{
    ((zmq::socket_base_t*) s_)->close ();
    return 0;
}

int zmq_setsockopt (void *s_, int option_, const void *optval_,
    size_t optvallen_)
{
    return (((zmq::socket_base_t*) s_)->setsockopt (option_, optval_,
        optvallen_));
}

int zmq_bind (void *s_, const char *addr_)
{
    return (((zmq::socket_base_t*) s_)->bind (addr_));
}

int zmq_connect (void *s_, const char *addr_)
{
    return (((zmq::socket_base_t*) s_)->connect (addr_));
}

int zmq_send (void *s_, zmq_msg_t *msg_, int flags_)
{
    return (((zmq::socket_base_t*) s_)->send (msg_, flags_));
}

int zmq_flush (void *s_)
{
    return (((zmq::socket_base_t*) s_)->flush ());
}

int zmq_recv (void *s_, zmq_msg_t *msg_, int flags_)
{
    return (((zmq::socket_base_t*) s_)->recv (msg_, flags_));
}

int zmq_poll (zmq_pollitem_t *items_, int nitems_)
{
    //  TODO: Replace the polling mechanism by the virtualised framework
    //  used in 0MQ I/O threads. That'll make the thing work on all platforms.
#if !defined ZMQ_HAVE_LINUX
    errno = ENOTSUP;
    return -1;
#else

    pollfd *pollfds = (pollfd*) malloc (nitems_ * sizeof (pollfd));
    zmq_assert (pollfds);
    int npollfds = 0;
    int nsockets = 0;

    zmq::app_thread_t *app_thread = NULL;

    for (int i = 0; i != nitems_; i++) {

        //  0MQ sockets.
        if (items_ [i].socket) {

            //  Get the app_thread the socket is living in. If there are two
            //  sockets in the same pollset with different app threads, fail.
            zmq::socket_base_t *s = (zmq::socket_base_t*) items_ [i].socket;
            if (app_thread) {
                if (app_thread != s->get_thread ()) {
                    free (pollfds);
                    errno = EFAULT;
                    return -1;
                }
            }
            else
                app_thread = s->get_thread ();

            nsockets++;
            continue;
        }

        //  Raw file descriptors.
        pollfds [npollfds].fd = items_ [i].fd;
        pollfds [npollfds].events =
            (items_ [i].events & ZMQ_POLLIN ? POLLIN : 0) |
            (items_ [i].events & ZMQ_POLLOUT ? POLLOUT : 0);
        npollfds++;
    }

    //  If there's at least one 0MQ socket in the pollset we have to poll
    //  for 0MQ commands. If ZMQ_POLL was not set, fail.
    if (nsockets) {
        pollfds [npollfds].fd = app_thread->get_signaler ()->get_fd ();
        if (pollfds [npollfds].fd == zmq::retired_fd) {
            free (pollfds);
            errno = ENOTSUP;
            return -1;
        }
        pollfds [npollfds].events = POLLIN;
        npollfds++;
    }

    int nevents = 0;
    bool initial = true;
    while (!nevents) {

        //  Wait for activity. In the first iteration just check for events,
        //  don't wait. Waiting would prevent exiting on any events that may
        //  already be signaled on 0MQ sockets.
        int rc = poll (pollfds, npollfds, initial ? 0 : -1);
        if (rc == -1 && errno == EINTR)
            continue;
        errno_assert (rc >= 0);
        initial = false;

        //  Process 0MQ commands if needed.
        if (nsockets && pollfds [npollfds -1].revents & POLLIN)
            app_thread->process_commands (false, false);

        //  Check for the events.
        int pollfd_pos = 0;
        for (int i = 0; i != nitems_; i++) {

            //  If the poll item is a raw file descriptor, simply convert
            //  the events to zmq_pollitem_t-style format.
            if (!items_ [i].socket) {
                items_ [i].revents =
                    (pollfds [pollfd_pos].revents & POLLIN ? ZMQ_POLLIN : 0) |
                    (pollfds [pollfd_pos].revents & POLLOUT ? ZMQ_POLLOUT : 0);
                if (items_ [i].revents)
                    nevents++;
                pollfd_pos++;
                continue;
            }

            //  The poll item is a 0MQ socket.
            zmq::socket_base_t *s = (zmq::socket_base_t*) items_ [i].socket;
            items_ [i].revents = 0;
            if ((items_ [i].events & ZMQ_POLLOUT) && s->has_out ())
                items_ [i].revents |= ZMQ_POLLOUT;
            if ((items_ [i].events & ZMQ_POLLIN) && s->has_in ())
                items_ [i].revents |= ZMQ_POLLIN;
            if (items_ [i].revents)
                nevents++;
        }
    }

    free (pollfds);
    return nevents;

#endif
}

#if defined ZMQ_HAVE_WINDOWS

static uint64_t now ()
{    
    //  Get the high resolution counter's accuracy.
    LARGE_INTEGER ticksPerSecond;
    QueryPerformanceFrequency (&ticksPerSecond);

    //  What time is it?
    LARGE_INTEGER tick;
    QueryPerformanceCounter (&tick);

    //  Convert the tick number into the number of seconds
    //  since the system was started.
    double ticks_div = (double) (ticksPerSecond.QuadPart / 1000000);     
    return (uint64_t) (tick.QuadPart / ticks_div);
}

void zmq_sleep (int seconds_)
{
    Sleep (seconds_ * 1000);
}

#else

static uint64_t now ()
{
    struct timeval tv;
    int rc;

    rc = gettimeofday (&tv, NULL);
    assert (rc == 0);
    return (tv.tv_sec * (uint64_t) 1000000 + tv.tv_usec);
}

void zmq_sleep (int seconds_)
{
    sleep (seconds_);
}

#endif

void *zmq_stopwatch_start ()
{
    uint64_t *watch = (uint64_t*) malloc (sizeof (uint64_t));
    zmq_assert (watch);
    *watch = now ();
    return (void*) watch;
}

unsigned long zmq_stopwatch_stop (void *watch_)
{
    uint64_t end = now ();
    uint64_t start = *(uint64_t*) watch_;
    free (watch_);
    return (unsigned long) (end - start);
}

