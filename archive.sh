#!/bin/bash

# Env:
# ARCHIVE_DEFAULT   default archive file (optional, default="./code.tgz")

#-------------------------------------------------------

function datecmd() {
    date +"%Y%m%d"
}

BACKUP_FILE="${ARCHIVE_DEFAULT:-"./code.tgz"}"
PREPEND_DATE="${DATE:-"$(datecmd)"}-"
if (($# > 0)); then
    OPTION="$1"
    shift
    if [ "$OPTION" == "-backup" ]; then
        BACKUP_FILE="$1"
        shift
        if [ -z "$BACKUP_FILE" ]; then
            echo "missing file name for $OPTION option"
            exit -1
        fi
    elif [ "$OPTION" == "-no-prepend-date" ]; then
        PREPEND_DATE=""
    elif [ "$OPTION" == "-h" ] || [ "$OPTION" == "--help" ]; then
        echo "Use: ${0} [-backup <file>] [-no-prepend-date]"
        echo
        echo "Create code tgz archive. By default with prepended date."
        echo
        echo "Environment:"
        echo "  ARCHIVE_DEFAULT    default file name (./code.tgz)"
        echo "  DATE               date to prepend ($(datecmd))"
        exit 0
    else
        echo "unknown option $OPTION"
        exit -1
    fi
fi

if [ -n "$PREPEND_DATE" ]; then
    FNAME="$(basename "$BACKUP_FILE")"
    DNAME="$(dirname "$BACKUP_FILE")"
    BACKUP_FILE="${DNAME}/${PREPEND_DATE}${FNAME}"
fi

CMD="tar czf $BACKUP_FILE src/*.cpp src/include/*.h compile.sh archive.sh generate_data/generate_data.jl generate_data/generate_map.jl generate_data/data"
echo "$CMD"
eval "$CMD"
