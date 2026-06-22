#!/bin/bash
./autogen.sh
./configure
make -j #CFLAGS="-g -Og -pthread -Wall -pedantic -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls -fsanitize=address"

cp memcached fleec
cp memcached-debug fleec-debug
