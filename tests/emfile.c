/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "testutil.h"
#include "../src/utils/atomic.c"

/*  Test parameters. */
#define MAX_SOCKETS 2048
#define COMPETITORS 10
struct nn_atomic parallel_socks;
struct nn_atomic serial_socks;

void competitor (NN_UNUSED void *arg)
{
    int mysocks [MAX_SOCKETS];
    int i;

    /*  Basic synchronization before beginning the socket-creation race.  */
    nn_sleep (10);

    /*  Compete against other workers to create as many sockets as possible. */
    for (i = 0; i != MAX_SOCKETS; i++) {
        nn_clear_errno ();
        mysocks [i] = nn_socket (AF_SP, NN_PAIR);
        if (mysocks [i] < 0) {
            nn_assert_is_error (mysocks [i] == -1, EMFILE);
            i--;
            break;
        }
        nn_atomic_inc (&parallel_socks, 1);

        /*  Yielding here tends to create the most concurrent contention by
            intersticing all threads. */
        nn_yield ();
    }

    /*  It should be impossible for any one parallel competitor to have created
        more sockets than what was created in a single-threaded serial
        procedure. */
    nn_assert (i <= (int) serial_socks.n);

    /*  Ensure clean shutdown of all sockets created by this competitor. */
    for (i; i >=0; --i) {
        test_close (mysocks [i]);
    }

}

int main (int argc, char *argv [])
{
    struct nn_thread thread [COMPETITORS];
    int socks [MAX_SOCKETS];
    int i;

    /*  First, just create as many sockets as possible serially. */
    nn_atomic_init (&serial_socks, 0);
    for (i = 0; i != MAX_SOCKETS; ++i) {
        nn_clear_errno ();
        socks [i] = nn_socket (AF_SP, NN_PAIR);
        if (socks [i] < 0) {
            nn_assert_is_error (socks [i] == -1, EMFILE);
            i--;
            break;
        }
        nn_atomic_inc (&serial_socks, 1);
    }

    /*  Next, ensure a clean shutdown of all sockets. */
    for (i; i >=0; --i) {
        test_close (socks [i]);
    }

    /*  Launch many competitors in parallel that are all trying to create
        as many sockets as possible. */
    nn_atomic_init (&parallel_socks, 0);
    for (i = 0; i != COMPETITORS; ++i) {
        nn_thread_init (&thread [i], competitor, NULL);
    }
    for (i = 0; i != COMPETITORS; ++i) {
        nn_thread_term (&thread [i]);
    }

    /*  At a minimum, all threads would start concurrently and contend for
        new socket slots before any of the others began shutting down its
        own sockets.... */
    nn_assert (parallel_socks.n >= serial_socks.n);

    /*  ... and at maximum, each thread would create all its sockets and
        even have time to destroy them all prior to the next competitor
        beginning to create sockets. */
    nn_assert (parallel_socks.n <= serial_socks.n * COMPETITORS);

    nn_atomic_term (&parallel_socks);
    nn_atomic_term (&serial_socks);

    return 0;
}

