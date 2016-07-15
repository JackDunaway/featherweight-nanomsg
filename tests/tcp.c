/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright 2015 Garrett D'Amore <garrett@damore.org>
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

int main (int argc, char *argv [])
{
    char addr [128];
    char addr2 [128];
    void *dummy_buf;
    uint64_t stat;
    size_t sz;
    int time;
    int opt;
    int rc;
    int sb;
    int i;
    int s1;
    int s2;
    int sc;

    int port = get_test_port (argc, argv);
    
    test_build_addr (addr, "tcp", "127.0.0.1", port);

    /*  Try closing bound but unconnected socket. */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    test_close (sb);

    /*  Try closing a TCP socket while it not connected. At the same time
        test specifying the local address for the connection. */
    sc = test_socket (AF_SP, NN_PAIR);
    test_build_addr (addr2, "tcp", "127.0.0.1;127.0.0.1", port);
    test_connect (sc, addr2);
    test_close (sc);

    /*  Open the socket anew. */
    sc = test_socket (AF_SP, NN_PAIR);

    /*  Check NODELAY socket option. */
    sz = sizeof (opt);
    rc = nn_getsockopt (sc, NN_TCP, NN_TCP_NODELAY, &opt, &sz);
    errno_assert (rc == 0);
    nn_assert (sz == sizeof (opt));
    nn_assert (opt == 0);
    opt = 2;
    nn_clear_errno ();
    rc = nn_setsockopt (sc, NN_TCP, NN_TCP_NODELAY, &opt, sizeof (opt));
    nn_assert_is_error (rc == -1, EINVAL);
    opt = 1;
    rc = nn_setsockopt (sc, NN_TCP, NN_TCP_NODELAY, &opt, sizeof (opt));
    errno_assert (rc == 0);
    sz = sizeof (opt);
    rc = nn_getsockopt (sc, NN_TCP, NN_TCP_NODELAY, &opt, &sz);
    errno_assert (rc == 0);
    nn_assert (sz == sizeof (opt));
    nn_assert (opt == 1);

    /*  Try using invalid address strings. */
    test_connect_fail (sc, "tcp://*:", EINVAL);
    test_connect_fail (sc, "tcp://*:1000000", EINVAL);
    test_connect_fail (sc, "tcp://*:some_port", EINVAL);
    test_connect_fail (sc, "tcp://eth10000;127.0.0.1:5555", ENODEV);
    test_connect_fail (sc, "tcp://127.0.0.1", EINVAL);
    test_bind_fail (sc, "tcp://127.0.0.1:", EINVAL);
    test_bind_fail (sc, "tcp://127.0.0.1:1000000", EINVAL);
    test_bind_fail (sc, "tcp://eth10000:5555", ENODEV);
    test_connect_fail (sc, "tcp://:5555", EINVAL);
    test_connect_fail (sc, "tcp://-hostname:5555", EINVAL);
    test_connect_fail (sc, "tcp://abc.123.---.#:5555", EINVAL);
    test_connect_fail (sc, "tcp://[::1]:5555", EINVAL);
    test_connect_fail (sc, "tcp://abc.123.:5555", EINVAL);
    test_connect_fail (sc, "tcp://abc...123:5555", EINVAL);
    test_connect_fail (sc, "tcp://.123:5555", EINVAL);

    /*  Connect correctly. Do so before binding the peer socket. */
    test_connect (sc, addr);
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);

    /*  Ping-pong test. */
    for (i = 0; i != 100; ++i) {
        test_send (sc, "ABC");
        test_recv (sb, "ABC");
        test_send (sb, "DEF");
        test_recv (sc, "DEF");
    }

    /*  Batch transfer test. */
    for (i = 0; i != 100; ++i) {
        test_send (sc, "0123456789012345678901234567890123456789");
    }
    for (i = 0; i != 100; ++i) {
        test_recv (sb, "0123456789012345678901234567890123456789");
    }

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
    time = test_wait_for_stat (s2, NN_STAT_BROKEN_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);

    /*  Ensure bound socket claims to have rejected the intruder. */
    stat = nn_get_statistic (sb, NN_STAT_BROKEN_CONNECTIONS);
    nn_assert (stat >= 1);
    test_close (s2);
    test_close (s1);
    time = test_wait_for_stat (sb, NN_STAT_BROKEN_CONNECTIONS, 2, 2000);
    nn_assert (time >= 0);
    test_close (sb);

    /*  Test two sockets binding to the same address. */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    s1 = test_socket (AF_SP, NN_PAIR);
    test_bind_fail (s1, addr, EADDRINUSE);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, addr);
    test_send (sb, "ABC");
    test_recv (sc, "ABC");
    test_close (sb);
    test_close (s1);
    test_close (sc);

    /*  Test NN_RCVMAXSIZE limit */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    s1 = test_socket (AF_SP, NN_PAIR);
    test_connect (s1, addr);
    opt = 4;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    test_send (s1, "ABC");
    test_recv (sb, "ABC");
    test_send (s1, "0123456789012345678901234567890123456789");
    nn_clear_errno ();
    rc = nn_recv (sb, &dummy_buf, NN_MSG, NN_DONTWAIT);
    nn_assert_is_error (rc == -1, EAGAIN);
    test_close (sb);
    test_close (s1);

    /*  Test that NN_RCVMAXSIZE can be -1, but not lower */
    sb = test_socket (AF_SP, NN_PAIR);
    opt = -1;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    opt = -2;
    nn_clear_errno ();
    rc = nn_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    nn_assert_is_error (rc == -1, EINVAL);
    test_close (sb);

    /*  Test closing a socket waiting to connect to a non-existent peer. */
    sc = test_socket (AF_SP, NN_PAIR);
    nn_assert_stat_value (sc, NN_STAT_CONNECT_ERRORS, 0);
    test_connect (sc, addr);

    /*  The timeout value upper bound is designed considering Windows by
        default does not report STATUS_CONNECTION_REFUSED (ECONNREFUSED)
        until 1000msec after connecting begins. */
    time = test_wait_for_stat (sc, NN_STAT_CONNECT_ERRORS, 1, 2000);
    nn_assert (time >= 0);
    test_close (sc);

    return 0;
}
