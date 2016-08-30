/*
    Copyright (c) 2013 Insollo Entertainment, LLC. All rights reserved.
    Copyright 2015 Garrett D'Amore <garrett@damore.org>
    Copyright 2016 Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>
    Copyright (c) 2016 Jack R. Dunaway. All rights reserved.

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

#ifndef TESTUTIL_H_INCLUDED
#define TESTUTIL_H_INCLUDED

#include "../src/nn.h"

/*  For test simplicity include all SP public headers. */
#include "../src/bus.h"
#include "../src/pair.h"
#include "../src/pipeline.h"
#include "../src/pubsub.h"
#include "../src/reqrep.h"
#include "../src/survey.h"

/*  For test simplicity include all transport public headers. */
#include "../src/inproc.h"
#include "../src/ipc.h"
#include "../src/tcp.h"
#include "../src/ws.h"

/*  In addition to a few source files needed for this test utility, include
    common threading and synchronization utilities used to create tests. */
#include "../src/utils/attr.h"
#include "../src/utils/clock.c"
#include "../src/utils/err.c"
#include "../src/utils/sem.c"
#include "../src/utils/sleep.c"
#include "../src/utils/thread.c"

/*  Public API test harness prototypes. */
static int nn_socket_ (char *file, int line, int domain, int protocol);
static int nn_connect_ (char *file, int line, int sock, char *address);
static int nn_bind_ (char *file, int line, int sock, char *address);
static void nn_send_ (char *file, int line, int sock, char *data);
static void nn_recv_ (char *file, int line, int sock, char *data);
static int nn_close_ (char *file, int line, int sock);
static int nn_setsockopt_ (char *file, int line, int sock, int level,
    int option, const void *optval, size_t optlen);

/*  Test-specific helper functions. */
static void nn_connect_expect_fail_ (char *file, int line, int sock,
    char *address, int expectederr);
static void nn_connect_expect_fail_ (char *file, int line, int sock,
    char *address, int expectederr);
static void nn_close_termed_ (char *file, int line, int sock);
static void nn_recv_expect_timeo_ (char *file, int line, int sock, int timeo);
static int nn_wait_for_stat_ (char *file, int line, int sock,
    int statistic, uint64_t goal, int timeout, char *name);
static void nn_assert_stat_value_ (char *file, int line, int sock,
    int statistic, uint64_t expected, char *name);
static int nn_test_get_socket_sp (char *file, int line, int sock);

/*  Array of all Scalability Protocol socket types. Useful for tests that
    iterate over all SPs to test a feature. The length of this array is
    defined as preprocessor constant to provide ability for usage as a
    static array initializer. */
static const NN_TEST_ALL_SP [] = {NN_PAIR, NN_REQ, NN_REP, NN_PUB, NN_SUB,
    NN_SURVEYOR, NN_RESPONDENT, NN_PUSH, NN_PULL, NN_BUS};
#define NN_TEST_ALL_SP_LEN \
    (sizeof (NN_TEST_ALL_SP) / sizeof (NN_TEST_ALL_SP [0]))

/*  Array of all transports. Useful for tests that iterate over all transports
    to test a feature. The length of this array is defined as preprocessor
    constant to provide ability for usage as a static array initializer. */
static const NN_TEST_ALL_TRANSPORTS [] = {NN_INPROC, NN_IPC, NN_TCP, NN_WS};
#define NN_TEST_ALL_TRANSPORTS_LEN \
    (sizeof (NN_TEST_ALL_TRANSPORTS) / sizeof (NN_TEST_ALL_TRANSPORTS [0]))

/*  Convenience macro that wraps `nn_socket()` in a test harness. */
#define test_socket(d, p) nn_socket_ (__FILE__, __LINE__, (d), (p))

/*  Convenience macro that wraps `nn_connect()` in a test harness. */
#define test_connect(s, addr) nn_connect_ (__FILE__, __LINE__, (s), (addr))

/*  Convenience macro that wraps `nn_bind()` in a test harness. */
#define test_bind(s, addr) nn_bind_ (__FILE__, __LINE__, (s), (addr))

/*  Convenience macro that wraps `nn_send()` in a test harness. msg is the
    message to send and must be a NULL-terminated string or string literal. */
#define test_send(s, msg) nn_send_ (__FILE__, __LINE__, (s), (msg))

/*  Convenience macro that wraps `nn_recv()` in a test harness. msg is the
    expected message and must be a NULL-terminated string or string literal. */
#define test_recv(s, msg) nn_recv_ (__FILE__, __LINE__, (s), (msg))

/*  Convenience macro to set up a recv operation expected to timeout. The
    NN_RCVTIMEO is temporarily set according to parameter timeo, then once the
    function completes successfully, the socket option is reset to its original
    value. Any recv result other than ETIMEDOUT will fail an assertion and
    abort the test. */
#define test_recv_expect_timeo(s, timeo) \
    nn_recv_expect_timeo_ (__FILE__, __LINE__, (s), (timeo))

/*  Convenience macro that wraps `nn_close()` in a test harness. */
#define test_close(s) nn_close_ (__FILE__, __LINE__, (s))

/*  Convenience macro for testing `nn_close()` on a socket where `nn_term()`
    was called in a concurrent thread, meaning there is a race as to which
    call succeeds. */
#define test_close_termed(s) nn_close_termed_ (__FILE__, __LINE__, (s))

/*  Convenience macro for testing `nn_connect()` expected failure modes. */
#define test_connect_fail(s, addr, expectederr) \
    nn_connect_expect_fail_ (__FILE__, __LINE__, (s), (addr), (expectederr))

/*  Convenience macro for testing `nn_bind()` expected failure modes. */
#define test_bind_fail(s, addr, expectederr) \
    nn_bind_expect_fail_ (__FILE__, __LINE__, (s), (addr), (expectederr))

/*  Convenience macro that wraps `nn_setsocketopt()` in a test harness. */
#define test_setsockopt(s, level, opt, val, sz) \
    nn_setsockopt_ (__FILE__, __LINE__, (s), (level), (opt), (val), (sz))

/*  Convenience macro that waits until a socket has reached a specified goal on
    a specified statistic. One such usage is to call this just after the
    first `nn_connect` to wait for 1 lifetime connection as an alternative to
    an open-loop `nn_sleep ()`. **CAVEAT**: the very nature of libnanomsg to
    maintain persistent, asynchronous connections means that once this
    function successfully returns, the socket is not guaranteed to still have
    precisely the same state as the postcondition that made this function
    return successfully (e.g., a synchronous send/recv may fail just after this
    function reporting a connection has been established, since the connection
    may have already dropped again, or otherwise altered in a concurrent
    thread). This function merely reports a historical fact - the crossing of
    a goal line - and it is misguided to deduce a current state from the return
    value of this function. Returns a non-negative time in milliseconds of
    the approximate time to reach the goal on success, and -1 with `errno`
    set on failure. */
#define test_wait_for_stat(s, stat, goal, timeo) \
    nn_wait_for_stat_ (__FILE__, __LINE__, (s), (stat), (goal), (timeo), #stat)

/*  Convenience macro that tests for the exact value of a socket statistic.
    Note that most statistics are a moving target, especially when a persistent
    connection is still active, so this function should only when the value
    is constrained with certainty. */
#define nn_assert_stat_value(s, stat, expected) \
    nn_assert_stat_value_ (__FILE__, __LINE__, (s), (stat), (expected), #stat)

/*  Gets the scalability protocol of a socket. */
#define test_get_socket_sp(s) nn_test_get_socket_sp (__FILE__, __LINE__, (s))

/*  Convenience macro that parses command line argument into the beginning of
    the port range assigned by the test runner to the test instance. */
#define get_test_port(argc, argv) (atoi ((argc) < 2 ? "5555" : (argv) [1]))

/*  Convenience macro for building a test address into a statically-allocated
    fixed-size buffer. */
#define test_build_addr(buf, proto, ip, port)\
    (snprintf ((buf), sizeof ((buf)), "%s://%s:%d", (proto), (ip), (port)))

/*  Convenience macro for building a test address into a statically-allocated
    fixed-size buffer. */
#define test_get_transport_addr(buf, transport, port)\
    do {\
        switch ((transport)) {\
        case NN_INPROC:\
            snprintf ((buf), sizeof ((buf)), "inproc://test_%d.ipc", (port));\
            break;\
        case NN_IPC:\
            snprintf ((buf), sizeof ((buf)), "ipc://test_%d.ipc", (port));\
            break;\
        case NN_TCP:\
            test_build_addr ((buf), "tcp", "127.0.0.1", (port));\
            break;\
        case NN_WS:\
            test_build_addr ((buf), "ws", "127.0.0.1", (port));\
            break;\
        default:\
            nn_assert_unreachable ("Unexpected transport.");\
            break;\
        }\
    } while (0);

static int NN_UNUSED nn_socket_ (char *file, int line, int domain, int protocol)
{
    int sock;

    nn_clear_errno ();
    sock = nn_socket (domain, protocol);
    if (sock == -1) {
        fprintf (stderr, "Failed create socket: %s [%d]\n(%s:%d)\n",
            nn_err_strerror (errno), (int) errno, file, line);
        nn_err_abort ();
    }

    return sock;
}

static int NN_UNUSED nn_connect_ (char *file, int line, int sock, char *address)
{
    int rc;

    nn_clear_errno ();
    rc = nn_connect (sock, address);
    if (rc < 0) {
        fprintf (stderr, "Failed connect to \"%s\": %s [%d]\n(%s:%d)\n",
            address, nn_err_strerror (errno), (int) errno, file, line);
        nn_err_abort ();
    }
    return rc;
}

static int NN_UNUSED nn_bind_ (char *file, int line, int sock, char *address)
{
    int rc;

    nn_clear_errno ();
    rc = nn_bind (sock, address);
    if (rc < 0) {
        fprintf (stderr, "Failed bind to \"%s\": %s [%d]\n(%s:%d)\n",
            address, nn_err_strerror (errno), (int) errno, file, line);
        nn_err_abort ();
    }
    return rc;
}

static int NN_UNUSED nn_setsockopt_ (char *file, int line, int sock,
    int level, int option, const void *optval, size_t optlen)
{
    int rc;

    nn_clear_errno ();
    rc = nn_setsockopt (sock, level, option, optval, optlen);
    if (rc < 0) {
        fprintf (stderr, "Failed set option \"%d\": %s [%d]\n(%s:%d)\n",
            option, nn_err_strerror (errno), (int) errno, file, line);
        nn_err_abort ();
    }
    return rc;
}

static int NN_UNUSED nn_close_ (char *file, int line, int sock)
{
    int rc;

    nn_clear_errno ();
    rc = nn_close (sock);
    if (rc != 0) {
        fprintf (stderr, "Failed to close socket: %s [%d]\n(%s:%d)\n",
            nn_err_strerror (errno), (int) errno, file, line);
        nn_err_abort ();
    }

    return rc;
}

static void NN_UNUSED nn_close_termed_ (char *file, int line, int sock)
{
    int rc;

    nn_clear_errno ();
    rc = nn_close (sock);

    /*  This `nn_close()` just won the race, or ... */
    if (rc == 0) {
        return;
    }

    /*  ... the concurrent `nn_term()` just won the race. */
    nn_assert_is_error (rc == -1, EBADF);
}

static void NN_UNUSED nn_send_ (char *file, int line, int sock, char *data)
{
    size_t data_len;
    int rc;

    data_len = strlen (data);

    nn_clear_errno ();
    rc = nn_send (sock, data, data_len, 0);
    if (rc < 0) {
        fprintf (stderr, "Failed to send: %s [%d]\n(%s:%d)\n",
            nn_err_strerror (errno), (int) errno, file, line);
        nn_err_abort ();
    }
    if (rc != (int) data_len) {
        fprintf (stderr, "Data to send is truncated: %d != %d\n(%s:%d)\n",
            rc, (int) data_len, file, line);
        nn_err_abort ();
    }
}

static void NN_UNUSED nn_recv_ (char *file, int line, int sock, char *data)
{
    size_t data_len;
    int rc;
    char *buf;

    data_len = strlen (data);
    /*  We allocate plus one byte so that we are sure that message received
        has correct length and not truncated.  */
    buf = malloc (data_len + 1);
    nn_assert_alloc (buf);

    nn_clear_errno ();
    rc = nn_recv (sock, buf, data_len + 1, 0);
    if (rc < 0) {
        fprintf (stderr, "Failed to recv: %s [%d]\n(%s:%d)\n",
            nn_err_strerror (errno), (int) errno, file, line);
        nn_err_abort ();
    }
    if (rc != (int) data_len) {
        fprintf (stderr, "Received data has wrong length: [%d != %d]\n(%s:%d)\n",
            rc, (int) data_len, file, line);
        nn_err_abort ();
    }
    if (memcmp (data, buf, data_len) != 0) {
        /*  We don't print the data as it may have binary garbage.  */
        fprintf (stderr, "Received data is wrong\n(%s:%d)\n", file, line);
        nn_err_abort ();
    }

    free (buf);
}

static void nn_connect_expect_fail_ (char *file, int line, int sock,
    char *address, int expectederr)
{   
    int rc;

    nn_clear_errno ();
    rc = nn_connect (sock, address);
    nn_assert_is_error (rc == -1, expectederr);
}

static void nn_bind_expect_fail_ (char *file, int line, int sock,
    char *address, int expectederr)
{   
    int rc;

    nn_clear_errno ();
    rc = nn_bind (sock, address);
    nn_assert_is_error (rc == -1, expectederr);
}

static void NN_UNUSED nn_recv_expect_timeo_ (char *file, int line, int sock, int timeo)
{
    char buf [1024];
    size_t sz;
    int orig;
    int rc;

    /*  Even though we cannot assert a timeout means "success", in this case
        anything else is an explicit failure. For this reason, we temporarily
        decrease the timeout for the sake of reducing test time, knowing the
        full timeout period is the expected time penalty. */

    /*  Store original timeout. */
    rc = nn_getsockopt (sock, NN_SOL_SOCKET, NN_RCVTIMEO, &orig, &sz);
    nn_assert (rc == 0);
    nn_assert (sz == sizeof (orig));

    /*  Set temporary allowed timeout. */
    test_setsockopt (sock, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo, sizeof (timeo));

    nn_clear_errno ();
    rc = nn_recv (sock, buf, sizeof (buf), 0);
    if (rc < 0 && errno != ETIMEDOUT) {
        fprintf (stderr, "Expected ETIMEDOUT but received [%d]: %s\n(%s:%d)\n",
            (int) errno, nn_err_strerror (errno), file, line);
        nn_err_abort ();
    } else if (rc >= 0) {
        fprintf (stderr, "Did not drop message: [%d bytes]\n(%s:%d)\n",
            rc, file, line);
        nn_err_abort ();
    }

    /*  Reset original timeout. */
    test_setsockopt (sock, NN_SOL_SOCKET, NN_RCVTIMEO, &orig, sizeof (orig));
}

static int nn_wait_for_stat_ (char *file, int line, int sock,
    int statistic, uint64_t goal, int timeout, char *name)
{
    uint64_t current;
    uint64_t deadline;
    uint64_t start;

    start = nn_clock_ms ();
    deadline = start + timeout;

    /*  TODO: consider equipping the public API with the ability to register
        callbacks for these events. It's far more elegant than polling here,
        and opens new abilities at the application layer also. */
    while (1) {
        current = nn_get_statistic (sock, statistic);
        /*  Unexpected error retrieving statistic, such as invalid socket
            or statistic. */
        if (current == (uint64_t) -1) {
            fprintf (stderr, "Failed to get [%s]: %s\n(%s:%d)\n",
                name, nn_err_strerror (errno), file, line);
            nn_err_abort ();
        }

        /*  Has goal been met? */
        if (current >= goal) {
            return (int) (nn_clock_ms () - start);
        }

        if (nn_clock_ms () > deadline) {
            errno = ETIMEDOUT;
            return -1;
        }

        nn_sleep (2);
    }
}

static void nn_assert_stat_value_ (char *file, int line, int sock,
    int statistic, uint64_t expected, char *name)
{
    uint64_t actual;

    actual = nn_get_statistic (sock, statistic);

    /*  Early return if expected value. This may also be used when the expected
        outcome is failure (e.g., (uint64_t) -1 to indicate error). */
    if (actual == expected) {
        return;
    }

    /*  Unexpected error retrieving statistic, such as invalid socket
        or statistic. */
    if (actual == (uint64_t) -1) {
        fprintf (stderr, "Failed to get [%s]: %s\n(%s:%d)\n",
            name, nn_err_strerror (errno), file, line);
        nn_err_abort ();
    }

    if (actual != expected) {
        fprintf (stderr, "Expected [%s == %d] but got [%d]\n(%s:%d)\n",
            name, (int) expected, (int) actual, file, line);
        nn_err_abort ();
    }
}

static int nn_test_get_socket_sp (char *file, int line, int sock)
{
    size_t sz;
    int type;
    int rc;

    rc = nn_getsockopt (sock, NN_SOL_SOCKET, NN_PROTOCOL, &type, &sz);
    nn_assert (rc == 0);
    nn_assert (sz == sizeof (type));

    return type;
}

#endif
