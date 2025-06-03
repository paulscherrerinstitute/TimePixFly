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

# Simplify compilation for Christian Appels Appel computer ;-)
# If $POCO_DIR/include is a directory, take POCO_DIR as base,
# otherwise assume that POCO_DIR/<some_version> is the base.
# Try to find the highest version.
if [ "$(uname)" == "Darwin" ]; then
    if [[ "$CXXFLAGS" != *"poco"* ]]; then
        poco_dir=${POCO_DIR:-"/opt/homebrew/Cellar/poco"}
        if [ -d "$poco_dir/include" ]; then
            CXXFLAGS="-I$poco_dir/include $CXXFLAGS"
            LDFLAGS="-L$poco_dir/lib $LDFLAGS"
        elif [ -d "$poco_dir" ]; then
            poco_versions=($(ls "$poco_dir" | sort -n))
            n_versions=${#poco_versions[*]}
            if ((n_versions != 0)); then
                poco_inst="$poco_dir/${poco_versions[$((n_versions-1))]}"
                if [ -d "$poco_inst" ]; then
                    CXXFLAGS="-I$poco_inst/include $CXXFLAGS"
                    LDFLAGS="-L$poco_inst/lib $LDFLAGS"
                fi
            fi
        fi
    fi
fi

TEST_FLAGS+=" -Og -ggdb -march=native"

test -f README.md || { echo "This command must be executed from the git top directory"; exit 1; }

VERSION="$(git branch --show-current) $(git log -n1 --format="%h %as")"
(($? == 0)) || { echo "This command must be executed inside the git repository"; exit 1; }
echo "const char VERSION[]=\"$VERSION\";" > src/include/version.h

function strip_target() {
        if [ -n "${STRIP}" ]; then
            cmd="strip $1"
            echo "$cmd"
            eval "$cmd"
        fi
}

case "$TARGET" in
    "tpx3app")
        cmd="${CXX} -I src/include src/main.cpp src/processing.cpp src/xes_data_writer.cpp src/global.cpp src/energy_points.cpp -std=c++17 ${CXXFLAGS} ${LDFLAGS} -o tpx3app"
        echo "$cmd"
        eval "$cmd"
        strip_target "tpx3app";;
    "server")
        cmd="${CXX} -I src/include src/test_server.cpp -std=c++17 ${CXXFLAGS} ${LDFLAGS} -o server"
        echo "$cmd"
        eval "$cmd"
        strip_target "server";;
    "test")
        cmd="${CXX} -I src/include src/test.cpp -std=c++17 ${TEST_FLAGS} -o test"
        echo "$cmd"
        eval "$cmd";;
    "doc")
        cmd="doxygen doc/doxygen.cfg"
        echo "$cmd"
        eval "$cmd";;
    "flags")
        echo "CXXFLAGS=$CXXFLAGS"
        echo "LDFLAGS=$LDFLAGS"
        echo "TEST_FLAGS=$TEST_FLAGS";;
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
        echo "    STRIP        strip executable"
        echo "    DEBUG        debug friendly flags"
        echo "    NOOPT        no optimization (with DEBUG)"
        echo "  test:"
        echo "    TEST_FLAGS   extra test executable flags";;
    *)
        echo "unknown target: $TARGET";;
esac
