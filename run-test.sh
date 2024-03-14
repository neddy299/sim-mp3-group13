#!/bin/bash
./sim "$1" "$2" "$3" 1>/dev/null

diff --strip-trailing-cr -q _out_"$1"_"$2"_test.txt "$4" 1>/dev/null

if [ $? -ne 0 ]; then
    echo -e "\033[0;31m[$1 $2 $3]: Test failed!\033[0m"
else
    echo -e "\033[0;32m[$1 $2 $3]: Test passed!\033[0m"
fi
