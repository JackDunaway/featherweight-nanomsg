/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
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
#define addr_inproc "inproc://a"
#define addr_ipc "ipc://test-separation.ipc"
static char addr_tcp [128];

/*  This test checks whether the library prevents interconnecting sockets
    between different non-compatible protocols. */

int main (int argc, char *argv [])
{
    int timeo;
    int pair;
    int pull;
    int rc;

    test_build_addr (addr_tcp, "tcp", "127.0.0.1", get_test_port (argc, argv));

    /*  Inproc: Bind first, connect second. */
    pair = test_socket (AF_SP, NN_PAIR);
    test_bind (pair, addr_inproc);
    pull = test_socket (AF_SP, NN_PULL);
    test_connect (pull, addr_inproc);
    timeo = 100;
    test_setsockopt (pair, NN_SOL_SOCKET, NN_SNDTIMEO, &timeo, sizeof (timeo));
    nn_clear_errno ();
    rc = nn_send (pair, "ABC", 3, 0);
    nn_assert_is_error (rc == -1, ETIMEDOUT);
    test_close (pull);
    test_close (pair);

    /*  Inproc: Connect first, bind second. */
    pull = test_socket (AF_SP, NN_PULL);
    test_connect (pull, addr_inproc);
    pair = test_socket (AF_SP, NN_PAIR);
    test_bind (pair, addr_inproc);
    timeo = 100;
    test_setsockopt (pair, NN_SOL_SOCKET, NN_SNDTIMEO, &timeo, sizeof (timeo));
    nn_clear_errno ();
    rc = nn_send (pair, "ABC", 3, 0);
    nn_assert_is_error (rc == -1, ETIMEDOUT);
    test_close (pull);
    test_close (pair);

#if !defined NN_HAVE_WINDOWS

    /*  IPC */
    pair = test_socket (AF_SP, NN_PAIR);
    test_bind (pair, addr_ipc);
    pull = test_socket (AF_SP, NN_PULL);
    test_connect (pull, addr_ipc);
    timeo = 100;
    test_setsockopt (pair, NN_SOL_SOCKET, NN_SNDTIMEO, &timeo, sizeof (timeo));
    nn_clear_errno ();
    rc = nn_send (pair, "ABC", 3, 0);
    nn_assert_is_error (rc == -1, ETIMEDOUT);
    test_close (pull);
    test_close (pair);

#endif

    /*  TCP */
    pair = test_socket (AF_SP, NN_PAIR);
    test_bind (pair, addr_tcp);
    pull = test_socket (AF_SP, NN_PULL);
    test_connect (pull, addr_tcp);
    timeo = 100;
    test_setsockopt (pair, NN_SOL_SOCKET, NN_SNDTIMEO, &timeo, sizeof (timeo));
    nn_clear_errno ();
    rc = nn_send (pair, "ABC", 3, 0);
    nn_assert_is_error (rc == -1, ETIMEDOUT);
    test_close (pull);
    test_close (pair);

    return 0;
}

