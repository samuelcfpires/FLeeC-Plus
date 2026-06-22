#!/bin/bash

if [ -f Makefile ]; then
    make clean > /dev/null
fi

find . -type l -exec rm -f {} + > /dev/null

rm -rf .deps autom4te.cache aclocal.m4 version.m4
rm -f config.h config.h.in config.log config.status configure configure~ configure.ac~
rm -f Makefile doc/Makefile Makefile.in doc/Makefile.in
rm -f fleec fleec-debug stamp-h1
