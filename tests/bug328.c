/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright 2016 Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>
    Copyright 2016 Garrett D'Amore <garrett@damore.org>

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
char addr [128];

int main (int argc, char *argv [])
{
    uint64_t stat;
    int time;
    int sb;
    int sc;

    test_build_addr (addr, "tcp", "127.0.0.1", get_test_port (argc, argv));

    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, addr);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, addr);
    test_send (sc, "ABC");
    test_recv (sb, "ABC");
    time = test_wait_for_stat (sc, NN_STAT_CURRENT_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);
    stat = nn_get_statistic (sc, NN_STAT_CURRENT_CONNECTIONS);
    nn_assert (stat == 1);
    test_close (sb);
    time = test_wait_for_stat (sc, NN_STAT_BROKEN_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);
    stat = nn_get_statistic (sc, NN_STAT_CURRENT_CONNECTIONS);
    nn_assert (stat == 0);
    test_close (sc);

    return 0;
}

