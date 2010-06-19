/*
    Copyright (c) 2007-2010 iMatix Corporation

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

#include <new>
#include <string.h>

#include "../include/zmq.h"

#include "ctx.hpp"
#include "socket_base.hpp"
#include "io_thread.hpp"
#include "platform.hpp"
#include "err.hpp"
#include "pipe.hpp"

#if defined ZMQ_HAVE_WINDOWS
#include "windows.h"
#endif

zmq::ctx_t::ctx_t (uint32_t io_threads_) :
    terminated (false)
{
#ifdef ZMQ_HAVE_WINDOWS
    //  Intialise Windows sockets. Note that WSAStartup can be called multiple
    //  times given that WSACleanup will be called for each WSAStartup.
    WORD version_requested = MAKEWORD (2, 2);
    WSADATA wsa_data;
    int rc = WSAStartup (version_requested, &wsa_data);
    zmq_assert (rc == 0);
    zmq_assert (LOBYTE (wsa_data.wVersion) == 2 &&
        HIBYTE (wsa_data.wVersion) == 2);
#endif

    //  Initialise the array of signalers.
    slot_count = max_sockets + io_threads_;
    slots = (signaler_t**) malloc (sizeof (signaler_t*) * slot_count);
    zmq_assert (slots);

    //  Create I/O thread objects and launch them.
    for (uint32_t i = 0; i != io_threads_; i++) {
        io_thread_t *io_thread = new (std::nothrow) io_thread_t (this, i);
        zmq_assert (io_thread);
        io_threads.push_back (io_thread);
        slots [i] = io_thread->get_signaler ();
        io_thread->start ();
    }

    //  In the unused part of the slot array, create a list of empty slots.
    for (uint32_t i = slot_count - 1; i >= io_threads_; i--) {
        empty_slots.push_back (i);
        slots [i] = NULL;
    }
}

int zmq::ctx_t::term ()
{
    //  First send stop command to sockets so that any
    //  blocking calls are interrupted.
    for (sockets_t::size_type i = 0; i != sockets.size (); i++)
        sockets [i]->stop ();

    //  Then mark context as terminated.
    slot_sync.lock ();
    zmq_assert (!terminated);
    terminated = true;
    bool destroy = sockets.empty ();
    slot_sync.unlock ();
    
    //  If there are no sockets open, destroy the context immediately.
    if (destroy)
        delete this;

    return 0;
}

zmq::ctx_t::~ctx_t ()
{
    //  Check that there are no remaining open sockets.
    zmq_assert (sockets.empty ());

    //  Get rid of remaining zombie sockets. The mutex provides a memory
    //  barrier needed to migrate all the zombies into this thread.
    slot_sync.lock ();
    while (!zombies.empty ())
        dezombify ();
    slot_sync.unlock ();

    //  Ask I/O threads to terminate. If stop signal wasn't sent to I/O
    //  thread subsequent invocation of destructor would hang-up.
    for (io_threads_t::size_type i = 0; i != io_threads.size (); i++)
        io_threads [i]->stop ();

    //  Wait till I/O threads actually terminate.
    for (io_threads_t::size_type i = 0; i != io_threads.size (); i++)
        delete io_threads [i];

    //  Deallocate the array of slot. No special work is
    //  needed as signalers themselves were deallocated with their
    //  corresponding io_thread/socket objects.
    free (slots);
    
#ifdef ZMQ_HAVE_WINDOWS
    //  On Windows, uninitialise socket layer.
    int rc = WSACleanup ();
    wsa_assert (rc != SOCKET_ERROR);
#endif
}

zmq::socket_base_t *zmq::ctx_t::create_socket (int type_)
{
    slot_sync.lock ();

    //  Free the slots, if possible.
    dezombify ();

    //  If max_sockets limit was reached, return error.
    if (empty_slots.empty ()) {
        slot_sync.unlock ();
        errno = EMFILE;
        return NULL;
    }

    //  Choose a slot for the socket.
    uint32_t slot = empty_slots.back ();
    empty_slots.pop_back ();

    //  Create the socket and register its signaler.
    socket_base_t *s = socket_base_t::create (type_, this, slot);
    if (!s) {
        empty_slots.push_back (slot);
        slot_sync.unlock ();
        return NULL;
    }
    sockets.push_back (s);
    slots [slot] = s->get_signaler ();
    
    slot_sync.unlock ();

    return s;
}

void zmq::ctx_t::destroy_socket (socket_base_t *socket_)
{
    slot_sync.lock ();

    //  Zombify the socket. Notice that the lock above provides the memory
    //  barrier needed to migrate zombie-to-be socket from it's native thread
    //  to shared data area synchronised by slot_sync.
    sockets.erase (socket_);
    zombies.push_back (socket_);

    //  Deallocate zombie sockets, if possible.
    dezombify ();

    //  Check whether this is final shutdown (zmq_term has been called and there
    //  are no more open sockets).
    bool destroy = (sockets.empty () && terminated);

    slot_sync.unlock ();

    //  Terminate the whole 0MQ infrastructure, including any remaining
    //  zombie sockets.
    if (destroy)
       delete this;
}

void zmq::ctx_t::send_command (uint32_t slot_, const command_t &command_)
{
    slots [slot_]->send (command_);
}

bool zmq::ctx_t::recv_command (uint32_t slot_, command_t *command_, bool block_)
{
    return slots [slot_]->recv (command_, block_);
}

zmq::io_thread_t *zmq::ctx_t::choose_io_thread (uint64_t affinity_)
{
    //  Find the I/O thread with minimum load.
    zmq_assert (io_threads.size () > 0);
    int min_load = -1;
    io_threads_t::size_type result = 0;
    for (io_threads_t::size_type i = 0; i != io_threads.size (); i++) {
        if (!affinity_ || (affinity_ & (uint64_t (1) << i))) {
            int load = io_threads [i]->get_load ();
            if (min_load == -1 || load < min_load) {
                min_load = load;
                result = i;
            }
        }
    }
    zmq_assert (min_load != -1);
    return io_threads [result];
}

int zmq::ctx_t::register_endpoint (const char *addr_,
    socket_base_t *socket_)
{
    endpoints_sync.lock ();

    bool inserted = endpoints.insert (std::make_pair (std::string (addr_),
        socket_)).second;
    if (!inserted) {
        errno = EADDRINUSE;
        endpoints_sync.unlock ();
        return -1;
    }

    endpoints_sync.unlock ();
    return 0;
}

void zmq::ctx_t::unregister_endpoints (socket_base_t *socket_)
{
    endpoints_sync.lock ();

    endpoints_t::iterator it = endpoints.begin ();
    while (it != endpoints.end ()) {
        if (it->second == socket_) {
            endpoints_t::iterator to_erase = it;
            it++;
            endpoints.erase (to_erase);
            continue;
        }
        it++;
    }
        
    endpoints_sync.unlock ();
}

zmq::socket_base_t *zmq::ctx_t::find_endpoint (const char *addr_)
{
     endpoints_sync.lock ();

     endpoints_t::iterator it = endpoints.find (addr_);
     if (it == endpoints.end ()) {
         endpoints_sync.unlock ();
         errno = ECONNREFUSED;
         return NULL;
     }
     socket_base_t *endpoint = it->second;

     //  Increment the command sequence number of the peer so that it won't
     //  get deallocated until "bind" command is issued by the caller.
     //  The subsequent 'bind' has to be called with inc_seqnum parameter
     //  set to false, so that the seqnum isn't incremented twice.
     endpoint->inc_seqnum ();

     endpoints_sync.unlock ();
     return endpoint;
}

void zmq::ctx_t::dezombify ()
{
    for (zombies_t::size_type i = 0; i != zombies.size ();)
        if (zombies [i]->dezombify ())
            zombies.erase (zombies [i]);
        else
            i++;
}

