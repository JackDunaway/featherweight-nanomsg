/*
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

#include "../src/nn.h"
#include "../src/reqrep.h"

#include "testutil.h"

#define TIMEOUT 100
#define MESSAGE "ABC"
#define ITERATIONS 100

int main (int argc, const char *argv[])
{
    char tcp_addr [128];
    char ws_addr [128];
    char *addr;
    size_t sz;
    int initial_sleep;
    int transport;
    int timeo;
    int rep;
    int req;
    int i;

    test_addr_from (tcp_addr, "tcp", "127.0.0.1", get_test_port (argc, argv));
    test_addr_from (ws_addr, "ws", "127.0.0.1", get_test_port (argc, argv) + 1);
    sz = sizeof (timeo);

    /*  Iterate this test over all transports. */
    for (transport = 0; transport < 4; transport++) {

        switch (transport) {
        case 0:
            addr = tcp_addr;
            initial_sleep = 0;
            break;
        case 1:
            addr = ws_addr;
            initial_sleep = 0;
            break;
        case 2:
            addr = "inproc://nn_test_reqrep_cancellation";
            initial_sleep = 0;
            break;
        case 3:
            addr = "ipc://nn_test_reqrep_cancellation";
            initial_sleep = 0;
            break;
        }

        rep = test_socket (AF_SP, NN_REP);
        test_bind (rep, addr);
        timeo = TIMEOUT;
        test_setsockopt (rep, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo, sz);
        nn_assert (timeo == TIMEOUT);
        test_setsockopt (rep, NN_SOL_SOCKET, NN_SNDTIMEO, &timeo, sz);
        nn_assert (timeo == TIMEOUT);

        nn_sleep (initial_sleep);

        for (i = 0; i != ITERATIONS; i++) {
            req = test_socket (AF_SP, NN_REQ);
            test_connect (req, addr);
            timeo = TIMEOUT;
            test_setsockopt (req, NN_SOL_SOCKET, NN_SNDTIMEO, &timeo, sz);
            nn_assert (timeo == TIMEOUT);
            test_setsockopt (req, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo, sz);
            nn_assert (timeo == TIMEOUT);

            /*  Send two requests in rapid succession, effectively cancelling
                the first request. For any given transport, it's a race
                condition whether the REP socket receives the first or second
                message first (or even if the first message is recv'd at all),
                so we request the same message payload twice for simplicity.
                This test is intended to ensure all sockets shut down properly
                and are race-free; it is not intended to test subtle ordering
                differences between transports. */
            test_send (req, MESSAGE);
            test_send (req, MESSAGE);
            test_recv (rep, MESSAGE);

            test_close (req);
        }

        test_close (rep);
        }

    return 0;
}

