#!/bin/bash

TARGET=${1:-tpx3app}

: ${CXX:=g++}
LDFLAGS+=" -lPocoJSON -lPocoUtil -lPocoNet -lPocoFoundation -lpthread"

WARN_FLAGS+=" -Wall -Wextra"

if [ -z "${DEBUG}" ]; then
    SPEED_FLAGS+="-O3 -ffast-math -DNDEBUG -march=native"
elif [ -z "${NOOPT}" ]; then
    SPEED_FLAGS+="-Og -ggdb  -DNDEBUG -march=native"
else
    SPEED_FLAGS+="-O0 -ggdb"
fi

CXXFLAGS+=" $WARN_FLAGS $SPEED_FLAGS"

TEST_FLAGS+=" -Og -ggdb -march=native"

test -f README.md || { echo "This command must be executed from the git top directory"; exit 1; }

VERSION="$(git branch --show-current) $(git log -n1 --format="%h %as")"
(($? == 0)) || { echo "This command must be executed inside the git repository"; exit 1; }
echo "const char VERSION[]=\"$VERSION\";" > src/include/version.h

case "$TARGET" in
    "tpx3app")
        cmd="${CXX} -I src/include src/main.cpp src/processing.cpp src/global.cpp -std=c++17 ${CXXFLAGS} ${LDFLAGS} -o tpx3app"
        echo "$cmd"
        eval "$cmd";;
    "server")
        cmd="${CXX} -I src/include src/test_server.cpp -std=c++17 ${CXXFLAGS} ${LDFLAGS} -o server"
        echo "$cmd"
        eval "$cmd";;
    "test")
        cmd="${CXX} -I src/include src/test.cpp -std=c++17 ${TEST_FLAGS} -o test"
        echo "$cmd"
        eval "$cmd";;
    "doc")
        cmd="doxygen doc/doxygen.cfg"
        echo "$cmd"
        eval "$cmd";;
    "help"|"--help"|"-h")
        echo "Use: ${0} <target>"
        echo
        echo "Compile targets:"
        echo "  tpx3app        (default) analysis application"
        echo "  server         raw data replay server"
        echo "  test           some unit tests for parts of the queueing code"
        echo "  doc            compile documentation in doc/html"
        echo "Debendencies:"
        echo "  ${LDFLAGS}"
        echo "Environment:"
        echo "  CXX            C++-17 and g++ options compatible compiler"
        echo "  tpx3app, server:"
        echo "    CXXFLAGS     extra compiler flags"
        echo "    LDFLAGS      extra linker flags"
        echo "    SPEED_FLAGS  extra optimization flags"
        echo "    WARN_FLAGS   extra warning flags"
        echo "  test:"
        echo "    TEST_FLAGS   extra test executable flags";;
    *)
        echo "unknown target: $TARGET";;
esac
