#!/bin/bash

./klient "$@" | \
   aplay -t raw -f cd -B 5000 -v - -D sysdefault
