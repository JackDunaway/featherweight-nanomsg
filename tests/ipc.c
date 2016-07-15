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
#define LARGE_MSG_SZ 10000
#define addr "ipc://test.ipc"

int main (int argc, char *argv [])
{
    char buf [LARGE_MSG_SZ];
    uint64_t stat;
    int time;
    int sb;
    int sc;
    int i;
    int s1, s2;
#if !defined(NN_HAVE_WINDOWS)
    int rc;
#endif

    /*  Try closing a IPC socket while it not connected. */
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, addr);
    test_close (sc);

    /*  Open the socket anew. */
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, addr);

    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);

    /*  In preparation for I/O, wait until at least one established connection. */
    time = test_wait_for_stat (sc, NN_STAT_CURRENT_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);

    /*  Ping-pong test. */
    for (i = 0; i != 100; ++i) {
        test_send (sc, "0123456789012345678901234567890123456789");
        test_recv (sb, "0123456789012345678901234567890123456789");
        test_send (sb, "0123456789012345678901234567890123456789");
        test_recv (sc, "0123456789012345678901234567890123456789");
    }

    /*  Batch transfer test. */
    for (i = 0; i != 100; ++i) {
        test_send (sc, "XYZ");
    }
    for (i = 0; i != 100; ++i) {
        test_recv (sb, "XYZ");
    }

    /*  Send something large enough to trigger overlapped I/O on Windows. */
    for (i = 0; i < LARGE_MSG_SZ; ++i) {
        buf [i] = 48 + i % 10;
    }
    buf [LARGE_MSG_SZ - 1] = '\0';
    test_send (sc, buf);
    test_recv (sb, buf);

    test_close (sc);
    test_close (sb);

    /*  Test whether connection rejection is handled decently. */
    /*  Create pair. */
    sb = test_socket (AF_SP, NN_PAIR);
    s1 = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    nn_assert_stat_value (sb, NN_STAT_ACCEPTED_CONNECTIONS, 0);
    nn_assert_stat_value (s1, NN_STAT_ESTABLISHED_CONNECTIONS, 0);
    test_connect (s1, addr);

    /*  Wait until both in pair claim the connection is established. */
    time = test_wait_for_stat (sb, NN_STAT_ACCEPTED_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (s1, NN_STAT_ESTABLISHED_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (sb, NN_STAT_CURRENT_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (s1, NN_STAT_CURRENT_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);

    /*  Ensure intruding connection is rejected by the established pair. */
    s2 = test_socket (AF_SP, NN_PAIR);
    stat = nn_get_statistic (s2, NN_STAT_BROKEN_CONNECTIONS);
    nn_assert (stat == 0);
    stat = nn_get_statistic (s2, NN_STAT_ESTABLISHED_CONNECTIONS);
    nn_assert (stat == 0);
    test_connect (s2, addr);
    time = test_wait_for_stat (sb, NN_STAT_ACCEPTED_CONNECTIONS, 2, 2000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (s2, NN_STAT_ESTABLISHED_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (sb, NN_STAT_BROKEN_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);
    //time = test_wait_for_stat (s2, NN_STAT_BROKEN_CONNECTIONS, 1, 2000);
    //nn_assert (time >= 0);

    /*  Ensure bound socket claims to have rejected the intruder. */
    stat = nn_get_statistic (sb, NN_STAT_BROKEN_CONNECTIONS);
    nn_assert (stat >= 1);
    test_close (s2);
    test_close (s1);
    time = test_wait_for_stat (sb, NN_STAT_BROKEN_CONNECTIONS, 2, 2000);
    nn_assert (time >= 0);
    test_close (sb);

/*  TODO: On Windows, CreateNamedPipeA does not run exclusively. Until the
    underlying usock is fixed, just disable this test on Windows. */
#if !defined(NN_HAVE_WINDOWS)
    /*  Test two sockets binding to the same address. */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, SOCKET_ADDRESS);
    s1 = test_socket (AF_SP, NN_PAIR);
    nn_clear_errno ();
    rc = nn_bind (s1, SOCKET_ADDRESS);
    nn_assert_is_error (rc == -1, EADDRINUSE);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, SOCKET_ADDRESS);

    time = test_wait_for_stat (sc, NN_STAT_CONNECT_ERRORS, 3, ivl * 2);
    test_send (sb, "ABC");
    test_recv (sc, "ABC");
    test_close (sb);
    test_close (sc);
    test_close (s1);
#endif

    /*  Test closing a socket waiting to connect to a non-existent peer. */
    sc = test_socket (AF_SP, NN_PAIR);
    nn_assert_stat_value (sc, NN_STAT_CONNECT_ERRORS, 0);
    test_connect (sc, addr);
    time = test_wait_for_stat (sc, NN_STAT_CONNECT_ERRORS, 1, 1000);
    nn_assert (time >= 0);
    test_close (sc);

    return 0;
}
