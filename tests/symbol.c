/*
    Copyright (c) 2013 Evan Wies <evan@neomantra.net>

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

int main ()
{
    struct nn_symbol_properties sym;
    const char *name;
    int value;
    int sz;
    int i;

    /*  This value is arbitrary but known not to collide on any field. */
    const struct nn_symbol_properties invalid = { -42, "", -42, -42, -42};

    sz = sizeof (sym);

    /*  Test symbol index below valid range. */
    nn_clear_errno ();
    name = nn_symbol (-1, NULL);
    nn_assert_is_error (name == NULL, EINVAL);
    nn_assert (nn_symbol_info (-1, &sym, sz) == 0);

    /*  Test symbol index above valid range. */
    nn_clear_errno ();
    name = nn_symbol (2000, NULL);
    nn_assert_is_error (name == NULL, EINVAL);
    nn_assert (nn_symbol_info (2000, &sym, sz) == 0);

    /*  Test symbol index within valid range. */
    nn_assert (nn_symbol (6, &value) != NULL);
    nn_assert (value != 0);
    nn_assert (nn_symbol_info (6, &sym, sz) == sizeof (sym));

    i = 0;
    while (1) {
        /*  Reset each iteration to an invalid sentinel. */
        value = invalid.value;

        /*  Reset errno each iteration to ensure it adheres to contract to set
            errno on failure. Note that the value is not tested on success,
            since the value of errno on success is not defined. */
        nn_clear_errno ();

        name = nn_symbol (i, &value);
        if (name) {
            /*  Ensure sentinel value has been replaced by valid value. */
            nn_assert (value != invalid.value);
            sym = invalid;
            nn_assert (nn_symbol_info (i, &sym, sz) == sizeof (sym));
            nn_assert (sym.name == name);
            nn_assert (sym.value == value);
            nn_assert (sym.ns != invalid.ns);
            nn_assert (sym.type != invalid.type);
            nn_assert (sym.unit != invalid.unit);
        }
        else {
            /*  Ensure errno contains expected value specified by function.  */
            nn_assert_is_error (name == NULL, EINVAL);
            
            /*  This function is expected to fail also, but does not specify
                a postcondition on errno. */
            nn_assert (nn_symbol_info (i, &sym, sz) == 0);

            break;
        }
        i++;
    }

    /*  This number is the expected number of symbols to be exported. This
        value is intentionally hard-coded, but should be equal to
        SYM_VALUE_NAMES_LEN. This brittleness is meant to alert developers
        during infrequent changes to exported symbols to come back to this
        test and evaluate the other heuristics. */
    nn_assert (i == 121);

    return 0;
}
