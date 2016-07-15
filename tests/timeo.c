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
#include "../src/utils/stopwatch.c"

/*  Sweep several times, especially passing through some potential points
    of interest such as multiples of 100msec (the default timeout period
    for a few retry operations in nanomsg. */
#define TIMEOUT_SWEEP_MAX 2100
#define TIMEOUT_SWEEP_INC 20

/*  Ensure the sweep maximum is an integer multiple of the increment. */
CT_ASSERT (TIMEOUT_SWEEP_MAX % TIMEOUT_SWEEP_INC == 0);
CT_ASSERT (TIMEOUT_SWEEP_INC > 0);
#define TIMEOUT_SWEEP_PTS ((TIMEOUT_SWEEP_MAX / TIMEOUT_SWEEP_INC) + 1)

enum nn_test_operation {NN_TEST_OP_SEND, NN_TEST_OP_RECV};

struct nn_test_args {
    int timeo;
    enum nn_test_operation op;
    struct nn_sem ready;
};

static void iotimeout (void *args)
{
    struct nn_test_args t;
    struct nn_stopwatch stopwatch;
    uint64_t elapsed;
    char buf [3];
    int rc;
    int s;

    /*  Make a local copy of the arguments and notify launcher we're ready. */
    t = *(struct nn_test_args *) args;
    s = test_socket (AF_SP, NN_PAIR);
    test_setsockopt (s, NN_SOL_SOCKET, NN_RCVTIMEO, &t.timeo, sizeof (t.timeo));
    test_setsockopt (s, NN_SOL_SOCKET, NN_SNDTIMEO, &t.timeo, sizeof (t.timeo));
    nn_yield ();
    nn_sem_post (&t.ready);

    /*  Note: errno is cleared and checked inside the stopwatch in the spirit
        of ensuring correctness of the I/O operations. It is assumed this
        operation does not substantively affect timing. */

    /* Test I/O operation timeout accuracy. */
    nn_stopwatch_init (&stopwatch);
    nn_clear_errno ();
    switch (t.op) {
    case NN_TEST_OP_SEND:
        rc = nn_send (s, "ABC", 3, 0);
        break;
    case NN_TEST_OP_RECV:
        rc = nn_recv (s, buf, sizeof (buf), 0);
        break;
    }
    nn_assert_is_error (rc == -1, ETIMEDOUT);
    elapsed = nn_stopwatch_term (&stopwatch);
    nn_assert_elapsed_time (elapsed, t.timeo * 1000);

    /*  Clean up. */
    test_close (s);
}

int main (int argc, char *argv [])
{
    struct nn_thread pool [TIMEOUT_SWEEP_PTS * 2] = {0};
    struct nn_test_args test;
    int i;

    /*  Initialize test resources. */
    nn_sem_init (&test.ready);

    for (i = 0; i != TIMEOUT_SWEEP_PTS; ++i) {
        test.timeo = TIMEOUT_SWEEP_MAX - i * TIMEOUT_SWEEP_INC;
        test.op = NN_TEST_OP_SEND;
        nn_thread_init (&pool [i], iotimeout, &test);
        nn_sem_wait (&test.ready);
        nn_yield ();
        test.op = NN_TEST_OP_RECV;
        nn_thread_init (&pool [i + TIMEOUT_SWEEP_PTS], iotimeout, &test);
        nn_sem_wait (&test.ready);
        nn_yield ();
    }

    /*  Clean up. */
    for (i = 0; i != TIMEOUT_SWEEP_PTS * 2; ++i) {
        nn_thread_term (&pool [i]);
    }
    nn_sem_term (&test.ready);

    return 0;
}
