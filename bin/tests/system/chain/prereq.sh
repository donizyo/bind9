#!/bin/sh
#
# Copyright (C) 2017  Internet Systems Consortium, Inc. ("ISC")
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

SYSTEMTESTTOP=..
. $SYSTEMTESTTOP/conf.sh

$SHELL ../testcrypto.sh || exit 255

if test -n "$PYTHON"
then
    if $PYTHON -c "import dns" 2> /dev/null
    then
        :
    else
        echo "I:This test requires the dnspython module." >&2
        exit 1
    fi
else
    echo "I:This test requires Python and the dnspython module." >&2
    exit 1
fi

if $PERL -e 'use Net::DNS;' 2>/dev/null
then
    if $PERL -e 'use Net::DNS; die if ($Net::DNS::VERSION >= 0.69 && $Net::DNS::VERSION <= 0.74);' 2>/dev/null
    then
        :
    else
        echo "I:Net::DNS versions 0.69 to 0.74 have bugs that cause this test to fail: please update." >&2
        exit 1
    fi
else
    echo "I:This test requires the perl Net::DNS library." >&2
    exit 1
fi
