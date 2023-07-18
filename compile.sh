#!/bin/bash

TARGET=${1:-tpx3app}

: ${CXX:=g++}
LDFLAGS+=" -lPocoJSON -lPocoUtil -lPocoNet -lPocoFoundation"

WARN_FLAGS+=" -Wall -Wextra"

if [ -z "${DEBUG}" ]; then
    SPEED_FLAGS+="-Ofast -march=native -DNDEBUG"
else
    SPEED_FLAGS+="-Og -ggdb -march=native"
fi

CXXFLAGS+=" $WARN_FLAGS $SPEED_FLAGS"

TEST_FLAGS+=" -Og -ggdb -march=native"

case "$TARGET" in
    "tpx3app")
        cmd="${CXX} -I src/include src/main.cpp -std=c++17 ${CXXFLAGS} ${LDFLAGS} -o tpx3app"
        echo "$cmd"
        eval "$cmd";;
    "test")
        cmd="${CXX} -I src/include src/test.cpp -std=c++17 ${TEST_FLAGS} -o test"
        echo "$cmd"
        eval "$cmd";;
    *)
        echo "unknown target: $TARGET";;
esac
