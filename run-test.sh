#!/bin/bash
TESTFILE=_test_"$1"_"$2"_"$3"

./sim "$1" "$2" "$3" 1>$TESTFILE

diff --strip-trailing-cr -q $TESTFILE "$4" 1>/dev/null

if [ $? -ne 0 ]; then
    echo -e "\033[0;31m[$1 $2 $3]: Test failed!\033[0m"
else
    echo -e "\033[0;32m[$1 $2 $3]: Test passed!\033[0m"
fi
