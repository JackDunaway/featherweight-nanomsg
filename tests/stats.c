/*
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
static char addr [128];

int main (int argc, char *argv [])
{
    int time;
    int rep1;
    int req1;

    test_build_addr (addr, "tcp", "127.0.0.1", get_test_port (argc, argv));

    /*  Test req/rep with full socket types. */
    rep1 = test_socket (AF_SP, NN_REP);
    test_bind (rep1, addr);

    req1 = test_socket (AF_SP, NN_REQ);
    test_connect (req1, addr);
    time = test_wait_for_stat (rep1, NN_STAT_ACCEPTED_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (req1, NN_STAT_ESTABLISHED_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (req1, NN_STAT_CURRENT_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (req1, NN_STAT_CURRENT_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);

    nn_assert_stat_value (rep1, NN_STAT_ESTABLISHED_CONNECTIONS, 0);
    nn_assert_stat_value (rep1, NN_STAT_MESSAGES_SENT, 0);
    nn_assert_stat_value (rep1, NN_STAT_MESSAGES_RECEIVED, 0);

    nn_assert_stat_value (req1, NN_STAT_ACCEPTED_CONNECTIONS, 0);
    nn_assert_stat_value (req1, NN_STAT_MESSAGES_SENT, 0);
    nn_assert_stat_value (req1, NN_STAT_MESSAGES_RECEIVED, 0);

    test_send (req1, "ABC");
    time = test_wait_for_stat (req1, NN_STAT_MESSAGES_SENT, 1, 1000);
    nn_assert (time >= 0);

    nn_assert_stat_value (req1, NN_STAT_MESSAGES_SENT, 1);
    nn_assert_stat_value (req1, NN_STAT_BYTES_SENT, 3);
    nn_assert_stat_value (req1, NN_STAT_MESSAGES_RECEIVED, 0);
    nn_assert_stat_value (req1, NN_STAT_BYTES_RECEIVED, 0);

    test_recv(rep1, "ABC");

    nn_assert_stat_value (rep1, NN_STAT_MESSAGES_SENT, 0);
    nn_assert_stat_value (rep1, NN_STAT_BYTES_SENT, 0);
    nn_assert_stat_value (rep1, NN_STAT_MESSAGES_RECEIVED, 1);
    nn_assert_stat_value (rep1, NN_STAT_BYTES_RECEIVED, 3);

    test_send (rep1, "OK");
    test_recv (req1, "OK");

    nn_assert_stat_value (req1, NN_STAT_MESSAGES_SENT, 1);
    nn_assert_stat_value (req1, NN_STAT_BYTES_SENT, 3);
    nn_assert_stat_value (req1, NN_STAT_MESSAGES_RECEIVED, 1);
    nn_assert_stat_value (req1, NN_STAT_BYTES_RECEIVED, 2);

    nn_assert_stat_value (rep1, NN_STAT_MESSAGES_SENT, 1);
    nn_assert_stat_value (rep1, NN_STAT_BYTES_SENT, 2);
    nn_assert_stat_value (rep1, NN_STAT_MESSAGES_RECEIVED, 1);
    nn_assert_stat_value (rep1, NN_STAT_BYTES_RECEIVED, 3);

    test_close (req1);

    time = test_wait_for_stat (rep1, NN_STAT_BROKEN_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);

    nn_assert_stat_value (rep1, NN_STAT_ESTABLISHED_CONNECTIONS, 0);
    nn_assert_stat_value (rep1, NN_STAT_CURRENT_CONNECTIONS, 0);

    test_close (rep1);

    return 0;
}

