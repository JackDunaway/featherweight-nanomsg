/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.

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

#ifndef NN_WIRE_INCLUDED
#define NN_WIRE_INCLUDED

#include <stdint.h>

/*  Length, in octets, of the Request ID packed into the header of a message
    as defined within nanomsg RFCs. */
#define NN_WIRE_REQID_LEN sizeof (uint32_t)

/*  Returns non-zero for the final Request ID in a stack indicating the
    receiving socket should process the message rather than forward it. */
#define nn_reqid_is_final(id) ((id) & 0x80000000)

/*  Returns the next serial Request ID with the most important bit set
    to indicate that this is the bottom of the backtrace stack. */
#define nn_reqid_next(id) ((++(id)) | 0x80000000)

uint16_t nn_gets (const uint8_t *buf);
void nn_puts (uint8_t *buf, uint16_t val);
uint32_t nn_getl (const uint8_t *buf);
void nn_putl (uint8_t *buf, uint32_t val);
uint64_t nn_getll (const uint8_t *buf);
void nn_putll (uint8_t *buf, uint64_t val);

#endif

