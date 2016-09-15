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

#ifndef NN_BSTREAM_INCLUDED
#define NN_BSTREAM_INCLUDED

#include "ustream.h"
#include "astream.h"

#include "../../transport.h"

/*  State machine managing bound stream endpoint. */

struct nn_bstream {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
    Thus it is derived from epbase. */
    struct nn_epbase epbase;

    /*  The underlying listening STREAM socket. */
    struct nn_stream usock;

    /*  The connection being accepted at the moment. */
    struct nn_astream *astream;

    /*  List of accepted connections. */
    struct nn_list astreams;

    /*  Virtual function table for stream subclass overrides. */
    struct nn_stream_vfptr *vft;
};

int nn_bstream_create (struct nn_bstream *bstream, void *hint,
    struct nn_epbase **epbase, struct nn_stream_vfptr *vft);

#endif
