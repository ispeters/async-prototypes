#!/bin/sh
# usage: build_tree.sh <g++|clang++> <flags> <output>
cc=$1; std=-std=c++20; [ "$cc" = clang++ ] && std=-std=c++23
exec $cc $std -Wall -Wextra -fno-omit-frame-pointer $2 tree_bench.cpp tree.cpp mstack.cpp async_abi.o -o $3
