/*
    Copyright (c) 2013-2014 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.

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
#define addr_a "inproc://a"
#define addr_b "inproc://b"

int main (int argc, char *argv [])
{
    int rc;
    int push1;
    int push2;
    int pull1;
    int pull2;
    int sndprio;
    int rcvprio;

    /*  Test send priorities. */

    pull1 = test_socket (AF_SP, NN_PULL);
    test_bind (pull1, addr_a);
    pull2 = test_socket (AF_SP, NN_PULL);
    test_bind (pull2, addr_b);
    push1 = test_socket (AF_SP, NN_PUSH);
    sndprio = 1;
    rc = nn_setsockopt (push1, NN_SOL_SOCKET, NN_SNDPRIO,
        &sndprio, sizeof (sndprio));
    errno_assert (rc == 0);
    test_connect (push1, addr_a);
    sndprio = 2;
    rc = nn_setsockopt (push1, NN_SOL_SOCKET, NN_SNDPRIO,
        &sndprio, sizeof (sndprio));
    errno_assert (rc == 0);
    test_connect (push1, addr_b);

    test_send (push1, "ABC");
    test_send (push1, "DEF");
    test_recv (pull1, "ABC");
    test_recv (pull1, "DEF");

    test_close (pull1);
    test_close (push1);
    test_close (pull2);

    /*  Test receive priorities. */

    push1 = test_socket (AF_SP, NN_PUSH);
    test_bind (push1, addr_a);
    push2 = test_socket (AF_SP, NN_PUSH);
    test_bind (push2, addr_b);
    pull1 = test_socket (AF_SP, NN_PULL);
    rcvprio = 2;
    rc = nn_setsockopt (pull1, NN_SOL_SOCKET, NN_RCVPRIO,
        &rcvprio, sizeof (rcvprio));
    errno_assert (rc == 0);
    test_connect (pull1, addr_a);
    rcvprio = 1;
    rc = nn_setsockopt (pull1, NN_SOL_SOCKET, NN_RCVPRIO,
        &rcvprio, sizeof (rcvprio));
    errno_assert (rc == 0);
    test_connect (pull1, addr_b);

    test_send (push1, "ABC");
    test_send (push2, "DEF");
    test_recv (pull1, "DEF");
    test_recv (pull1, "ABC");

    test_close (pull1);
    test_close (push2);
    test_close (push1);

    /*  Test removing a pipe from the list. */

    push1 = test_socket (AF_SP, NN_PUSH);
    test_bind (push1, addr_a);
    pull1 = test_socket (AF_SP, NN_PULL);
    test_connect (pull1, addr_a);

    test_send (push1, "ABC");
    test_recv (pull1, "ABC");
    test_close (pull1);

    nn_clear_errno ();
    rc = nn_send (push1, "ABC", 3, NN_DONTWAIT);
    nn_assert_is_error (rc == -1, EAGAIN);

    pull1 = test_socket (AF_SP, NN_PULL);
    test_connect (pull1, addr_a);

    test_send (push1, "ABC");
    test_recv (pull1, "ABC");
    test_close (pull1);
    test_close (push1);

    return 0;
}
