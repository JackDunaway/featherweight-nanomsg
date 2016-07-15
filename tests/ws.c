/*
    Copyright (c) 2012 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2014-2016 Jack R. Dunaway. All rights reserved.
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

/*  Test parameters. */
static char addr [128];
static char addr_any [128];

/*  Verify that we drop messages properly when sending invalid UTF-8, but not
    when sending valid UTF-8. */
void test_text ()
{
    int sb;
    int sc;
    int opt;
    uint8_t bad[20];

    /*  Negative testing... bad UTF-8 data for text. */
    sb = test_socket (AF_SP, NN_PAIR);
    sc = test_socket (AF_SP, NN_PAIR);

    opt = NN_WS_MSG_TYPE_TEXT;
    test_setsockopt (sb, NN_WS, NN_WS_MSG_TYPE, &opt, sizeof (opt));
    opt = NN_WS_MSG_TYPE_TEXT;
    test_setsockopt (sc, NN_WS, NN_WS_MSG_TYPE, &opt, sizeof (opt));
    opt = 1000;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVTIMEO, &opt, sizeof (opt));

    test_bind (sb, addr);
    test_connect (sc, addr);

    test_send (sc, "GOOD");
    test_recv (sb, "GOOD");

    /*  and the bad ... */
    strcpy ((char *)bad, "BAD.");
    bad[2] = (char)0xDD;
    test_send (sc, (char *)bad);

    /*  Make sure we dropped the frame. */
    test_recv_expect_timeo (sb, 50);

    test_close (sb);
    test_close (sc);

    return;
}

int main (int argc, char *argv [])
{
    uint64_t stat;
    size_t sz;
    int time;
    int rc;
    int sb;
    int sc;
    int s1;
    int s2;
    int sb2;
    int opt;
    int i;

    int port = get_test_port (argc, argv);

    test_build_addr (addr, "ws", "127.0.0.1", port);
    test_build_addr (addr_any, "ws", "*", port);

    /*  Try closing bound but unconnected socket. */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr_any);
    test_close (sb);

    /*  Try closing a TCP socket while it not connected. At the same time
        test specifying the local address for the connection. */
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, addr);
    test_close (sc);

    /*  Open the socket anew. */
    sc = test_socket (AF_SP, NN_PAIR);

    /*  Check socket options. */
    sz = sizeof (opt);
    rc = nn_getsockopt (sc, NN_WS, NN_WS_MSG_TYPE, &opt, &sz);
    errno_assert (rc == 0);
    nn_assert (sz == sizeof (opt));
    nn_assert (opt == NN_WS_MSG_TYPE_BINARY);

    /*  Default port 80 should be assumed if not explicitly declared. */
    test_connect (sc, "ws://127.0.0.1");

    /*  Try using invalid address strings. */
    test_connect_fail (sc, "ws://*:", EINVAL);
    test_connect_fail (sc, "ws://*:1000000", EINVAL);
    test_connect_fail (sc, "ws://*:some_port", EINVAL);
    test_connect_fail (sc, "ws://eth10000;127.0.0.1:5555", ENODEV);
    test_connect_fail (sc, "ws://:5555", EINVAL);
    test_connect_fail (sc, "ws://-hostname:5555", EINVAL);
    test_connect_fail (sc, "ws://abc.123.---.#:5555", EINVAL);
    test_connect_fail (sc, "ws://[::1]:5555", EINVAL);
    test_connect_fail (sc, "ws://abc.123.:5555", EINVAL);
    test_connect_fail (sc, "ws://abc...123:5555", EINVAL);
    test_connect_fail (sc, "ws://.123:5555", EINVAL);

    test_bind_fail (sc, "ws://127.0.0.1:", EINVAL);
    test_bind_fail (sc, "ws://127.0.0.1:1000000", EINVAL);
    test_bind_fail (sc, "ws://eth10000:5555", ENODEV);

    test_close (sc);

    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, addr);

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
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    s1 = test_socket (AF_SP, NN_PAIR);
    stat = nn_get_statistic (s1, NN_STAT_BROKEN_CONNECTIONS);
    nn_assert (stat == 0);
    test_connect (s1, addr);
    time = test_wait_for_stat (s1, NN_STAT_ESTABLISHED_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);
    s2 = test_socket (AF_SP, NN_PAIR);
    stat = nn_get_statistic (s2, NN_STAT_BROKEN_CONNECTIONS);
    nn_assert (stat == 0);
    test_connect (s2, addr);
    time = test_wait_for_stat (s2, NN_STAT_BROKEN_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);
    test_close (s2);
    test_close (s1);
    test_close (sb);

    /*  Test two sockets binding to the same address. */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    sb2 = test_socket (AF_SP, NN_PAIR);
    test_bind_fail (sb2, addr, EADDRINUSE);
    test_close(sb);
    test_close(sb2);

    /*  Test that NN_RCVMAXSIZE can be -1, but not lower */
    sb = test_socket (AF_SP, NN_PAIR);
    opt = -1;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    opt = -2;
    nn_clear_errno ();
    rc = nn_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    nn_assert_is_error (rc == -1, EINVAL);
    test_close (sb);

    /*  Test NN_RCVMAXSIZE limit */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, addr);
    opt = 1000;
    test_setsockopt (sc, NN_SOL_SOCKET, NN_SNDTIMEO, &opt, sizeof (opt));
    nn_assert (opt == 1000);
    opt = 1000;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVTIMEO, &opt, sizeof (opt));
    nn_assert (opt == 1000);
    opt = 4;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    test_send (sc, "ABC");
    test_recv (sb, "ABC");
    test_send (sc, "ABCD");
    test_recv (sb, "ABCD");
    test_send (sc, "ABCDE");
    test_recv_expect_timeo (sb, 50);

    /*  Increase the size limit, reconnect, then try sending again. The reason a
        reconnect is necessary is because after a protocol violation, the
        connecting socket will not continue automatic reconnection attempts. */
    opt = 5;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    test_connect (sc, addr);
    test_send (sc, "ABCDE");
    test_recv (sb, "ABCDE");
    test_close (sb);
    test_close (sc);

    test_text ();

    /*  Test closing a socket waiting to connect to a non-existent peer. */
    sc = test_socket (AF_SP, NN_PAIR);
    stat = nn_get_statistic (sc, NN_STAT_CONNECT_ERRORS);
    nn_assert (stat == 0);
    test_connect (sc, addr);

    /*  The timeout value upper bound is designed considering Windows by
        default does not report STATUS_CONNECTION_REFUSED (ECONNREFUSED)
        until 1000msec after connecting begins. */
    time = test_wait_for_stat (sc, NN_STAT_CONNECT_ERRORS, 1, 2000);
    nn_assert (time >= 0);
    test_close (sc);

    return 0;
}
