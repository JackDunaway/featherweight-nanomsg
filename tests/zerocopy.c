/*
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
    Copyright (c) 2014 Achille Roussel. All rights reserved.

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

#include "../src/nn.h"
#include "../src/pubsub.h"
#include "../src/reqrep.h"

#include "testutil.h"

#include <string.h>

#define NN_TEST_MSG1 "Hello World!"
#define NN_TEST_MSG_LEN1 sizeof (NN_TEST_MSG1) - 1

void test_allocmsg_reqrep ()
{
    int rc;
    int req;
    void *p;
    struct nn_iovec iov;
    struct nn_msghdr hdr;

    /*  Try to create an oversized message. */
    nn_clear_errno ();
    p = nn_allocmsg (-1, 0);
    nn_assert_is_error (!p, ENOMEM);
    nn_clear_errno ();
    p = nn_allocmsg (-3, 0);
    nn_assert_is_error (!p, ENOMEM);

    /*  Try to create a message of unknown type. */
    nn_clear_errno ();
    p = nn_allocmsg (100, 333);
    nn_assert_is_error (!p, EINVAL);

    /*  Create a socket. */
    req = test_socket (AF_SP_RAW, NN_REQ);

    /*  Make send fail and check whether the buffer is left alone
        rather than deallocated. */
    p = nn_allocmsg (100, 0);
    nn_assert_alloc (p);
    nn_clear_errno ();
    rc = nn_send (req, &p, NN_MSG, NN_DONTWAIT);
    nn_assert_is_error (rc < 0, EAGAIN);
    memset (p, 0, 100);
    rc = nn_freemsg (p);
    errno_assert (rc == 0);

    /*  Same thing with nn_sendmsg(). */
    p = nn_allocmsg (100, 0);
    nn_assert_alloc (p);
    iov.iov_base = &p;
    iov.iov_len = NN_MSG;
    memset (&hdr, 0, sizeof (hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    nn_clear_errno ();
    rc = nn_sendmsg (req, &hdr, NN_DONTWAIT);
    nn_assert_is_error (rc == -1, EAGAIN);
    memset (p, 0, 100);
    rc = nn_freemsg (p);
    errno_assert (rc == 0);

    /*  Clean up. */
    test_close (req);
}

void test_reallocmsg_reqrep ()
{
    int rc;
    int req;
    int rep;
    void *p;
    void *p2;

    /*  Create sockets. */
    req = test_socket (AF_SP, NN_REQ);
    rep = test_socket (AF_SP, NN_REP);
    rc = test_bind (rep, "inproc://test");
    rc = test_connect (req, "inproc://test");

    /*  Create message, make sure we handle overflow. */
    p = nn_allocmsg (100, 0);
    nn_assert_alloc (p);
    nn_clear_errno ();
    p2 = nn_reallocmsg (p, (size_t)-3);
    nn_assert_is_error (!p2, ENOMEM);

    /*  Realloc to fit data size. */
    memcpy (p, NN_TEST_MSG1, NN_TEST_MSG_LEN1);
    p = nn_reallocmsg (p, NN_TEST_MSG_LEN1);
    nn_assert_alloc (p);
    rc = nn_send (req, &p, NN_MSG, 0);
    errno_assert (rc == NN_TEST_MSG_LEN1);

    /*  Receive request and send response. */
    rc = nn_recv (rep, &p, NN_MSG, 0);
    errno_assert (rc == NN_TEST_MSG_LEN1);
    rc = nn_send (rep, &p, NN_MSG, 0);
    errno_assert (rc == NN_TEST_MSG_LEN1);

    /*  Receive response and free message. */
    rc = nn_recv (req, &p, NN_MSG, 0);
    errno_assert (rc == NN_TEST_MSG_LEN1);
    rc = memcmp (p, NN_TEST_MSG1, NN_TEST_MSG_LEN1);
    nn_assert (rc == 0);
    rc = nn_freemsg (p);
    errno_assert (rc == 0);

    /*  Clean up. */
    test_close (req);
    test_close (rep);
}

void test_reallocmsg_pubsub ()
{
    int rc;
    int pub;
    int sub1;
    int sub2;
    void *p;
    void *p1;
    void *p2;

    /*  Create sockets. */
    pub = test_socket (AF_SP, NN_PUB);
    sub1 = test_socket (AF_SP, NN_SUB);
    sub2 = test_socket (AF_SP, NN_SUB);
    test_bind (pub, "inproc://test");
    test_connect (sub1, "inproc://test");
    test_connect (sub2, "inproc://test");
    test_setsockopt (sub1, NN_SUB, NN_SUB_SUBSCRIBE, "", 0);
    test_setsockopt (sub2, NN_SUB, NN_SUB_SUBSCRIBE, "", 0);

    /*  Publish message. */
    p = nn_allocmsg (12, 0);
    nn_assert_alloc (p);
    memcpy (p, NN_TEST_MSG1, NN_TEST_MSG_LEN1);
    rc = nn_send (pub, &p, NN_MSG, 0);
    errno_assert (rc == NN_TEST_MSG_LEN1);

    /*  Receive messages and ensure they are the same message. */
    rc = nn_recv (sub1, &p1, NN_MSG, 0);
    errno_assert (rc == NN_TEST_MSG_LEN1);
    rc = nn_recv (sub2, &p2, NN_MSG, 0);
    errno_assert (rc == NN_TEST_MSG_LEN1);
    rc = memcmp (p1, NN_TEST_MSG1, NN_TEST_MSG_LEN1);
    nn_assert (rc == 0);
    rc = memcmp (p2, NN_TEST_MSG1, NN_TEST_MSG_LEN1);
    nn_assert (rc == 0);
    
    /*  libnanomsg never zero copies anymore - it used to be an attribute of
        the inproc transport, but frankly its a mistake for anyone to depend
        on that. The implementation must be free to copy, move data, etc.
        The only thing that should be guaranteed is that the "ownership" of the
        message on send is passed to libnanomsg. libnanomsg may give that
        message to an inproc receiver, or it can do something else (like copy
        the data) with it. The rationale for deprecating this behavior is that
        multiple recipient sockets, or even two peer sockets as a trivial pair,
        regardless the scalability protocol, shouldn't be concerned with
        potentially interfering with one another by having concurrent access to
        the same memory location. If they did, a key value proposition
        of libnanomsg to protect memory access across threads is eliminated. An
        application needing such capability is likely better served by
        synchronization primitives instead of a dangerously-low-level zero-copy
        feature within libnanomsg. */
    /*  nn_assert (p1 == p2); */

    /*  Release messages. */
    rc = nn_freemsg (p1);
    errno_assert (rc == 0);
    rc = nn_freemsg (p2);
    errno_assert (rc == 0);

    /*  Clean up. */
    test_close (sub2);
    test_close (sub1);
    test_close (pub);
}

int main ()
{
    test_allocmsg_reqrep ();
    test_reallocmsg_reqrep ();
    test_reallocmsg_pubsub ();

    return 0;
}

