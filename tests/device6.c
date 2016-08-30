/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
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
static char addr_h [128];
static char addr_i [128];
static char addr_j [128];
struct nn_sem ready;

void device5 (NN_UNUSED void *arg)
{
    int rc;
    int dev0;
    int dev1;

    /*  Intialise the device sockets. */
    dev0 = test_socket (AF_SP_RAW, NN_RESPONDENT);
    test_bind (dev0, addr_h);
    dev1 = test_socket (AF_SP_RAW, NN_SURVEYOR);
    test_bind (dev1, addr_i);

    /*  Notify launcher once ready. */
    nn_sem_post (&ready);

    /*  Run the device. */
    nn_clear_errno ();
    rc = nn_device (dev0, dev1);
    nn_assert_is_error (rc == -1, EBADF);

    /*  Clean up. */
    test_close_termed (dev0);
    test_close_termed (dev1);
}

void device6 (NN_UNUSED void *arg)
{
    int rc;
    int dev2;
    int dev3;

    /*  Intialise the device sockets. */
    dev2 = test_socket (AF_SP_RAW, NN_RESPONDENT);
    test_connect (dev2, addr_i);
    dev3 = test_socket (AF_SP_RAW, NN_SURVEYOR);
    test_bind (dev3, addr_j);

    /*  Notify launcher once ready. */
    test_wait_for_stat (dev2, NN_STAT_CURRENT_CONNECTIONS, 1, 1000);
    nn_sem_post (&ready);

    /*  Run the device. */
    nn_clear_errno ();
    rc = nn_device (dev2, dev3);
    nn_assert_is_error (rc == -1, EBADF);

    /*  Clean up. */
    test_close_termed (dev2);
    test_close_termed (dev3);
}

int main (int argc, char *argv [])
{
    int time;
    int end0;
    int end1;
    struct nn_thread thread5;
    struct nn_thread thread6;

    int port = get_test_port(argc, argv);

    test_build_addr (addr_h, "tcp", "127.0.0.1", port);
    test_build_addr (addr_i, "tcp", "127.0.0.1", port + 1);
    test_build_addr (addr_j, "tcp", "127.0.0.1", port + 2);

    /*  Test the bi-directional device with SURVEYOR(headers). */

    /*  Start the devices. */
    nn_sem_init (&ready);
    nn_thread_init (&thread5, device5, NULL);
    nn_sem_wait (&ready);
    nn_thread_init (&thread6, device6, NULL);
    nn_sem_wait (&ready);

    /*  Create two sockets to connect to the device. */
    end0 = test_socket (AF_SP, NN_SURVEYOR);
    test_connect (end0, addr_h);
    end1 = test_socket (AF_SP, NN_RESPONDENT);
    test_connect (end1, addr_j);

    /*  In preparation for I/O, wait until both previous calls to connect
        have established connections at least once. */
    time = test_wait_for_stat (end1, NN_STAT_CURRENT_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);
    time = test_wait_for_stat (end0, NN_STAT_CURRENT_CONNECTIONS, 1, 1000);
    nn_assert (time >= 0);

    /*  Pass a message between endpoints. */
    test_send (end0, "XYZ");
    test_recv (end1, "XYZ");

    /*  Now send a reply. */
    test_send (end1, "REPLYXYZ");
    test_recv (end0, "REPLYXYZ");

    /*  Clean up. */
    test_close (end0);
    test_close (end1);

    /*  Shut down the devices. */
    nn_term ();
    nn_thread_term (&thread5);
    nn_thread_term (&thread6);
    nn_sem_term (&ready);

    return 0;
}

