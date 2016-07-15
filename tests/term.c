/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.

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

static void worker (void *arg)
{
    struct nn_sem *ready;
    char buf [3];
    int rc;
    int s;

    /*  Test socket. */
    s = test_socket (AF_SP, NN_PAIR);

    ready = arg;
    nn_sem_post (ready);

    /*  Launch blocking function to check that it will be unblocked once
        nn_term() is called from the main thread. */
    nn_clear_errno ();
    rc = nn_recv (s, buf, sizeof (buf), 0);
    nn_assert_is_error (rc == -1, EBADF);

    /*  Check that all subsequent operations fail in synchronous manner. */
    nn_clear_errno ();
    rc = nn_recv (s, buf, sizeof (buf), 0);
    nn_assert_is_error (rc == -1, EBADF);
    test_close (s);
}

int main (int argc, char *argv [])
{
    struct nn_thread thread;
    struct nn_sem ready;
    int rc;
    int s;

    /*  Close the socket with no associated endpoints. */
    s = test_socket (AF_SP, NN_PAIR);
    test_close (s);

    /*  Test nn_term() before nn_close(). */
    nn_sem_init (&ready);
    nn_thread_init (&thread, worker, &ready);
    nn_sem_wait (&ready);
    nn_sem_term (&ready);
    nn_term ();

    /*  Check that it's not possible to create new sockets after nn_term(). */
    nn_clear_errno ();
    rc = nn_socket (AF_SP, NN_PAIR);
    nn_assert_is_error (rc == -1, ETERM);

    /*  Wait till worker thread terminates. */
    nn_thread_term (&thread);

    return 0;
}

