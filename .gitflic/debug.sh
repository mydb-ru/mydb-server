#!/usr/bin/env bash

set -xeuo pipefail

build() {
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
    chown -R ubuntu /build

    # Have to run MTR as non-root. Changing stdin to /dev/null is a workaround
    # for a weird main.loaddata_special failure that only occurs when starting
    # MTR under non-root user in a Docker container
    {
        su - ubuntu <<EOF
/build/mysql-test/mtr --mem --parallel=auto \
    --suite=default --force --max-test-fail=0 --report-unstable-tests \
    --unit-tests --unit-tests-report < /dev/null
EOF
    } || true

    local failures

    set +x
    failures="$(grep 'Failing test(s): ' /build/debug_log.txt)"
    set -x

    failures="${failures/Failing test(s): /}"
    failures="${failures/shutdown_report/}"
    failures="${failures/unit_tests/--unit-tests --unit-tests-report}"

    if [[ -z "$failures" ]]; then
        return 0
    fi

    rm -rf /build/mysql-test/var

    # Re-run failing tests without /dev/shm but with libeatmydata
    su - ubuntu <<EOF
eatmydata /build/mysql-test/mtr --parallel=auto \
    --suite=default --force --max-test-fail=0 --report-unstable-tests \
    $failures \
    < /dev/null
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

main "$@" 2>&1 | tee /build/debug_log.txt
