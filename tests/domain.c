/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.

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
const int ALL_DOMAIN [] = {AF_SP, AF_SP_RAW};
#define ALL_DOMAIN_LEN (sizeof (ALL_DOMAIN) / sizeof (ALL_DOMAIN [0]))

int main (int argc, char *argv [])
{
    int current_protocol;
    int current_domain;
    size_t optsz;
    int opt;
    int rc;
    int s;
    int i;
    int j;

    /*  Test the NN_DOMAIN and NN_PROTOCOL socket options over every possible
        domain and protocol. */
    for (j = 0; j < ALL_DOMAIN_LEN; j++) {

        current_domain = ALL_DOMAIN [j];

        for (i = 0; i < NN_TEST_ALL_SP_LEN; i++) {

            current_protocol = NN_TEST_ALL_SP [j];

            s = test_socket (current_domain, current_protocol);

            optsz = sizeof (opt);
            rc = nn_getsockopt (s, NN_SOL_SOCKET, NN_DOMAIN, &opt, &optsz);
            errno_assert (rc == 0);
            nn_assert (optsz == sizeof (opt));
            nn_assert (opt == current_domain);

            optsz = sizeof (opt);
            rc = nn_getsockopt (s, NN_SOL_SOCKET, NN_PROTOCOL, &opt, &optsz);
            errno_assert (rc == 0);
            nn_assert (optsz == sizeof (opt));
            nn_assert (opt == current_protocol);

            test_close (s);
        }
    }

    return 0;
}

