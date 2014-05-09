#!/bin/bash
# Prep the environment for future recompiles
export CLEAN_SOURCE=no
export CONCURRENCY_LEVEL=33
echo CLEAN_SOURCE=$CLEAN_SOURCE
echo CONCURRENCY_LEVEL=$CONCURRENCY_LEVEL

# RE-Compile it!
cd linux-3.13.6+duet
time fakeroot make-kpkg --initrd --append-to-version=+duet kernel_image kernel_headers

# ...and RE-compile the btrfs tools!
cd ../btrfs-progs-3.12+duet
make
