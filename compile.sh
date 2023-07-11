#!/bin/bash

: ${CXX:=g++}
LDFLAGS+=" -lPocoJSON -lPocoUtil -lPocoNet -lPocoFoundation"

WARN_FLAGS+=" -Wall -Wextra"

if [ -z "${DEBUG}" ]; then
    SPEED_FLAGS+="-Ofast -march=native -DNDEBUG"
else
    SPEED_FLAGS+="-Og -ggdb -march=native"
fi

CXXFLAGS+=" $WARN_FLAGS $SPEED_FLAGS"

cmd="${CXX} -I src/include src/main.cpp -std=c++17 ${CXXFLAGS} ${LDFLAGS} -o tpx3app"
echo "$cmd"
eval "$cmd"
