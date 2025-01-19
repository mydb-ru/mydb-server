#!/usr/bin/env bash

set -xeuo pipefail

build() {
    rm -rf /build/*

    # To be able to generate commit ID in INFO_SRC
    git config --global --add safe.directory /src

    cmake -S /src -B /build \
        -DWITH_LD=mold \
        -DWITH_ROUTER=0 \
        -DWITH_SYSTEM_LIBS=on \
        -DWITH_NDBCLUSTER_STORAGE_ENGINE=0 \
        -DWITH_DEBUG=1 \
        -DOPTIMIZE_DEBUG_BUILDS=1 \
        -DMYSQL_MAINTAINER_MODE=0 \
        -DWITH_NUMA=1 \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -G Ninja

    ninja -C /build
}

test() {
    chown -R ubuntu:ubuntu /build

    su - ubuntu <<EOF
# Have to run MTR as non-root
eatmydata /build/mysql-test/mtr --parallel=auto \
    --suite=main --force --max-test-fail=0 --report-unstable-tests \
    --unit-tests --unit-tests-report
EOF
}

main() {
    if [[ $# -ne 1 ]]; then
        cat <<EOF
Usage: $0 command

Where command is one of: build, test.
EOF
        exit 1
    fi

    case $1 in
    build | test)
        "$1" "$@"
        ;;
    *)
        echo "Unsupported command: $1"
        exit 1
        ;;
    esac
}

main "$@"
