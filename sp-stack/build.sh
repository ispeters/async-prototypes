#!/bin/sh
# usage: build.sh <g++|clang++> <-O level> <output> <sources...>
cc=$1; O=$2; out=$3; shift 3
std=-std=c++20; [ "$cc" = clang++ ] && std=-std=c++23
exec $cc $std -Wall -Wextra -fno-omit-frame-pointer $O "$@" stores.cpp mstack.cpp async_abi.o -o $out
