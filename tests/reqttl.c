/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
    Copyright 2016 Garrett D'Amore <garrett@damore.org>
    Copyright 2016 Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>

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
static char addr_a [128];
static char addr_b [128];
int dev0;
int dev1;

void device (NN_UNUSED void *arg)
{
    int rc;

    /*  Run the device. */
    nn_clear_errno ();
    rc = nn_device (dev0, dev1);
    nn_assert_is_error (rc == -1, EBADF);
}

int main (int argc, char *argv [])
{
    struct nn_thread thread1;
    size_t sz;
    int maxttl;
    int timeo;
    int end0;
    int end1;
    int rc;

    int port = get_test_port(argc, argv);

    test_build_addr (addr_a, "tcp", "127.0.0.1", port);
    test_build_addr (addr_b, "tcp", "127.0.0.1", port + 1);

    /*  Intialise the device sockets. */
    dev0 = test_socket (AF_SP_RAW, NN_REP);
    dev1 = test_socket (AF_SP_RAW, NN_REQ);

    test_bind (dev0, addr_a);
    test_bind (dev1, addr_b);

    /*  Start the device. */
    nn_thread_init (&thread1, device, NULL);

    end0 = test_socket (AF_SP, NN_REQ);
    end1 = test_socket (AF_SP, NN_REP);

    /*  Test the bi-directional device TTL */ 
    test_connect (end0, addr_a);
    test_connect (end1, addr_b);

    /*  Set up max receive timeout. */
    timeo = 1000;
    test_setsockopt (end0, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo, sizeof (timeo));
    timeo = 1000;
    test_setsockopt (end1, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo, sizeof (timeo));

    /*  Test default TTL is 8. */
    sz = sizeof (maxttl);
    maxttl = -1;
    rc = nn_getsockopt(end1, NN_SOL_SOCKET, NN_MAXTTL, &maxttl, &sz);
    nn_assert (rc == 0);
    nn_assert (sz == sizeof (maxttl));
    nn_assert (maxttl == 8);

    /*  Test to make sure option TTL cannot be set below 1. */
    maxttl = -1;
    nn_clear_errno ();
    rc = nn_setsockopt(end1, NN_SOL_SOCKET, NN_MAXTTL, &maxttl, sizeof (maxttl));
    nn_assert_is_error (rc == -1, EINVAL);
    nn_assert (maxttl == -1);
    maxttl = 0;
    nn_clear_errno ();
    rc = nn_setsockopt(end1, NN_SOL_SOCKET, NN_MAXTTL, &maxttl, sizeof (maxttl));
    nn_assert_is_error (rc == -1, EINVAL);
    nn_assert (maxttl == 0);

    /*  Test to set non-integer size */
    maxttl = 8;
    nn_clear_errno ();
    rc = nn_setsockopt(end1, NN_SOL_SOCKET, NN_MAXTTL, &maxttl, 1);
    nn_assert_is_error (rc == -1, EINVAL);
    nn_assert (maxttl == 8);

    /*  Pass a message end-to-end between endpoints. */
    test_send (end0, "XYZ");
    test_recv (end1, "XYZ");

    /*  Now send a reply. */
    test_send (end1, "REPLYXYZ\n");
    test_recv (end0, "REPLYXYZ\n");

    /*  Reduce max TTL so that message is dropped on a hop (by the device). */
    maxttl = 1;
    test_setsockopt (end0, NN_SOL_SOCKET, NN_MAXTTL, &maxttl, sizeof (maxttl));
    test_setsockopt (end1, NN_SOL_SOCKET, NN_MAXTTL, &maxttl, sizeof (maxttl));
    test_send (end0, "DROPTHIS");
    test_recv_expect_timeo (end1, 50);

    /*  Now increase max TTL and expect success again. */
    maxttl = 2;
    test_setsockopt (end0, NN_SOL_SOCKET, NN_MAXTTL, &maxttl, sizeof (maxttl));
    test_setsockopt (end1, NN_SOL_SOCKET, NN_MAXTTL, &maxttl, sizeof (maxttl));

    /*  Final end-to-end test. */
    test_send (end0, "DONTDROP");
    test_recv (end1, "DONTDROP");
    test_send (end1, "GOTIT");
    test_recv (end0, "GOTIT");

    /*  Clean up. */
    test_close (end0);
    test_close (end1);

    /*  Shut down the devices. */
    test_close (dev0);
    test_close (dev1);

    nn_thread_term (&thread1);

    return 0;
}
