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

/*  Test parameters. */
#define addr "inproc://a"
int sc;
int sb;

void worker (void *arg)
{
    struct nn_sem *ready;

    ready = arg;
    nn_sem_post (ready);
    test_send (sc, "ABC");
    nn_sem_post (ready);
    test_send (sc, "ABC");
}

int main (int argc, char *argv [])
{
    struct nn_thread thread;
    struct nn_sem ready;

    /*  Check whether blocking on send/recv works as expected. */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, addr);

    nn_sem_init (&ready);
    nn_thread_init (&thread, worker, &ready);
    nn_sem_wait (&ready);
    test_recv (sb, "ABC");
    nn_sem_wait (&ready);
    test_recv (sb, "ABC");
    nn_thread_term (&thread);
    nn_sem_term (&ready);

    test_close (sc);
    test_close (sb);

    return 0;
}

