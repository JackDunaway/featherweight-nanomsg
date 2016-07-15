/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright (c) 2015-2016 Jack R. Dunaway. All rights reserved.

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
#define TEST_REPETITIONS 10
#define WORKERS 10
#define MSG_COUNT 10
#define MSG "ws"
#define MSGSZ (sizeof (MSG) - 1)
static char addr [128];
struct nn_sem ready;

static void subscriber_worker (void *arg)
{
    char msg [MSGSZ];
    int count = 0;
    int rc;
    int s;

    s = *(int *) arg;
    nn_sem_post (&ready);

    while (1) {
        rc = nn_recv (s, &msg, sizeof (msg), 0);
        if (rc != MSGSZ) {
            break;
        }
        count++;
        nn_yield ();
    }
    /*  Socket is expected to be closed by caller.  */
    nn_assert_is_error (rc == -1, EBADF);

    /*  Ensure at least one message was received, but no more than expected. */
    nn_assert (0 < count && count <= MSG_COUNT);
}

int main (int argc, char *argv [])
{
    struct nn_thread pool [WORKERS];
    int sockets [WORKERS];
    int sndtimeo = 0;
    uint64_t time;
    int sb;
    int s;
    int i;
    int j;

    test_addr_from (addr, "ws", "127.0.0.1", get_test_port (argc, argv));

    /*  Test condition of closing sockets that are blocking in another thread. */
    for (i = 0; i != TEST_REPETITIONS; ++i) {

        /*  Initialize local socket. */
        sb = test_socket (AF_SP, NN_PUB);
        test_bind (sb, addr);
        test_setsockopt (sb, NN_SOL_SOCKET, NN_SNDTIMEO,
            &sndtimeo, sizeof (sndtimeo));

        /*  Launch worker pool. */
        nn_sem_init (&ready);
        for (j = 0; j < WORKERS; j++){
            s = test_socket (AF_SP, NN_SUB);
            test_setsockopt (s, NN_SUB, NN_SUB_SUBSCRIBE, "", 0);
            test_connect (s, addr);
            time = test_wait_for_stat (s, NN_STAT_CURRENT_CONNECTIONS, 1, 2000);
            nn_assert (time >= 0);
            sockets [j] = s;
            nn_thread_init (&pool [j], subscriber_worker, &s);
            nn_sem_wait (&ready);
        }

        /*  Send a few messages to ensure worker pool is active. */
        for (j = 0; j < MSG_COUNT; ++j) {
            test_send (sb, MSG);
            nn_yield ();
        }

        /*  Wait for clean shutdown of worker pool. */
        for (j = 0; j < WORKERS; j++) {
            test_close (sockets [j]);
            nn_thread_term (&pool [j]);
        }

        test_close (sb);
    }

    return 0;
}
