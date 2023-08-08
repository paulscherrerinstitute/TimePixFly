#!/bin/bash

# Env:
# ARCHIVE_DEFAULT   default archive file (optional, default="./code.tgz")

#-------------------------------------------------------
BACKUP_FILE="${ARCHIVE_DEFAULT:-"./code.tgz"}"
PREPEND_DATE="${DATE:-"$(date +"%Y%m%d")"}"
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
    else
        echo "unknown option $OPTION"
        exit -1
    fi
fi

if [ -n "$PREPEND_DATE" ]; then
    FNAME="$(basename "$BACKUP_FILE")"
    DNAME="$(dirname "$BACKUP_FILE")"
    BACKUP_FILE="${DNAME}/${PREPEND_DATE}-${FNAME}"
fi

CMD="tar czf $BACKUP_FILE src/*.cpp src/include/*.h compile.sh archive.sh generate_data/generate_data.jl generate_data/data"
echo "$CMD"
eval "$CMD"
