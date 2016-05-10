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

#ifndef NN_CONT_INCLUDED
#define NN_CONT_INCLUDED

#include "err.h"

/*  Takes a pointer to a member variable and computes pointer to the owning
    structure that contains it. The return value will be NULL if the input
    pointer is NULL. */
#define nn_cont_unsafe(owner_ptr, owner_type, member_name) \
    (owner_ptr ? ((owner_type*) (((char*) owner_ptr) - \
        offsetof(owner_type, member_name))) : NULL)

/*  Takes a pointer to a member variable, computes pointer to the owning
    structure that contains it, then assigns its value to owner_ptr. This macro
    asserts that the member pointer must be a valid pointer, so be mindful to
    not pass a NULL value. */
#define nn_cont_assert(owner_ptr, member_ptr, owner_type, member_name) \
    do { \
        nn_assert (member_ptr); \
        owner_ptr = ((owner_type*) (((char*) member_ptr) - \
            offsetof(owner_type, member_name))); \
    } while (0)

#endif
