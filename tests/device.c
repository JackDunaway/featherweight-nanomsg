/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
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
#define addr_c "inproc://c"
#define addr_d "inproc://d"
#define addr_e "inproc://e"

void device1 (NN_UNUSED void *arg)
{
    int rc;
    int deva;
    int devb;

    /*  Intialise the device sockets. */
    deva = test_socket (AF_SP_RAW, NN_PAIR);
    test_bind (deva, addr_a);
    devb = test_socket (AF_SP_RAW, NN_PAIR);
    test_bind (devb, addr_b);

    /*  Run the device. */
    nn_clear_errno ();
    rc = nn_device (deva, devb);
    nn_assert_is_error (rc == -1, EBADF);

    /*  Clean up. */
    test_close_termed (devb);
    test_close_termed (deva);
}

void device2 (NN_UNUSED void *arg)
{
    int rc;
    int devc;
    int devd;

    /*  Intialise the device sockets. */
    devc = test_socket (AF_SP_RAW, NN_PULL);
    test_bind (devc, addr_c);
    devd = test_socket (AF_SP_RAW, NN_PUSH);
    test_bind (devd, addr_d);

    /*  Run the device. */
    nn_clear_errno ();
    rc = nn_device (devc, devd);
    nn_assert_is_error (rc == -1, EBADF);

    /*  Clean up. */
    test_close_termed (devc);
    test_close_termed (devd);
}

void device3 (NN_UNUSED void *arg)
{
    int rc;
    int deve;

    /*  Intialise the device socket. */
    deve = test_socket (AF_SP_RAW, NN_BUS);
    test_bind (deve, addr_e);

    /*  Run the device. */
    nn_clear_errno ();
    rc = nn_device (deve, -1);
    nn_assert_is_error (rc == -1, EBADF);

    /*  Clean up. */
    test_close_termed (deve);
}

int main (int argc, char *argv [])
{
    int time;
    int enda;
    int endb;
    int endc;
    int endd;
    int ende1;
    int ende2;
    struct nn_thread thread1;
    struct nn_thread thread2;
    struct nn_thread thread3;

    /*  Test the bi-directional device. */

    /*  Start the device. */
    nn_thread_init (&thread1, device1, NULL);

    /*  Create two sockets to connect to the device. */
    enda = test_socket (AF_SP, NN_PAIR);
    test_connect (enda, addr_a);
    endb = test_socket (AF_SP, NN_PAIR);
    test_connect (endb, addr_b);

    /*  Pass a pair of messages between endpoints. */
    test_send (enda, "ABC");
    test_recv (endb, "ABC");
    test_send (endb, "ABC");
    test_recv (enda, "ABC");

    /*  Clean up. */
    test_close (endb);
    test_close (enda);

    /*  Test the uni-directional device. */

    /*  Start the device. */
    nn_thread_init (&thread2, device2, NULL);

    /*  Create two sockets to connect to the device. */
    endc = test_socket (AF_SP, NN_PUSH);
    test_connect (endc, addr_c);
    endd = test_socket (AF_SP, NN_PULL);
    test_connect (endd, addr_d);

    /*  Pass a message between endpoints. */
    test_send (endc, "XYZ");
    test_recv (endd, "XYZ");

    /*  Clean up. */
    test_close (endd);
    test_close (endc);

    /*  Test the loopback device. */

    /*  Start the device. */
    nn_thread_init (&thread3, device3, NULL);

    /*  Create two sockets to connect to the device. */
    ende1 = test_socket (AF_SP, NN_BUS);
    test_connect (ende1, addr_e);
    ende2 = test_socket (AF_SP, NN_BUS);
    test_connect (ende2, addr_e);

    /*  BUS is unreliable by design, so wait for endpoints to first join. */
    time = test_wait_for_stat (ende1, NN_STAT_CURRENT_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (ende2, NN_STAT_CURRENT_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);

    /*  Pass a message to the bus. */
    test_send (ende1, "KLM");
    test_recv (ende2, "KLM");

    /*  Make sure that the message doesn't arrive at the socket it was
        originally sent to. */
    test_recv_expect_timeo (ende1, 10);

    /*  Clean up. */
    test_close (ende2);
    test_close (ende1);

    /*  Shut down the devices. */
    nn_term ();
    nn_thread_term (&thread1);
    nn_thread_term (&thread2);
    nn_thread_term (&thread3);

    return 0;
}

