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
static char addr_a [128];
static char addr_b [128];

void device4 (NN_UNUSED void *arg)
{
    int rc;
    int devf;
    int devg;

    /*  Intialise the device sockets. */
    devf = test_socket (AF_SP_RAW, NN_REP);
    test_bind (devf, addr_a);
    devg = test_socket (AF_SP_RAW, NN_REQ);
    test_bind (devg, addr_b);

    /*  Run the device. */
    nn_clear_errno ();
    rc = nn_device (devf, devg);
    nn_assert_is_error (rc == -1, EBADF);

    /*  Clean up. */
    test_close (devg);
    test_close (devf);
}

int main (int argc, char *argv [])
{
    int endf;
    int endg;
    struct nn_thread thread4;

    int port = get_test_port (argc, argv);

    test_build_addr (addr_a, "tcp", "127.0.0.1", port);
    test_build_addr (addr_b, "tcp", "127.0.0.1", port + 1);

    /*  Test the bi-directional device with REQ/REP (headers). */

    /*  Start the device. */
    nn_thread_init (&thread4, device4, NULL);

    /*  Create two sockets to connect to the device. */
    endf = test_socket (AF_SP, NN_REQ);
    test_connect (endf, addr_a);
    endg = test_socket (AF_SP, NN_REP);
    test_connect (endg, addr_b);

    /*  Pass a message between endpoints. Note that because REP/REQ provides
        best-effort reliability to send and resend as connections are
        established, no wait is required after `nn_connect ()`. */
    test_send (endf, "XYZ");
    test_recv (endg, "XYZ");

    /*  Now send a reply. */
    test_send (endg, "REPLYXYZ");
    test_recv (endf, "REPLYXYZ");

    /*  Clean up. */
    test_close (endg);
    test_close (endf);

    /*  Shut down the devices. */
    nn_term ();
    nn_thread_term (&thread4);

    return 0;
}
