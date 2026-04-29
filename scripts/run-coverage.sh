#!/usr/bin/env bash
#
# run-coverage.sh -- build with gcov instrumentation, run ctest, and
# emit an HTML coverage report under build-coverage/coverage/.
#
# Usage (from repo root):
#
#   scripts/run-coverage.sh            # build + test + report
#   scripts/run-coverage.sh --rebuild  # nuke build-coverage/ first
#
# Requirements: gcc, cmake, gcovr, libcmocka-dev, libssl-dev, plus a
# valid TLS cert/key pair under samples/certs (or HTTP_C_SERVER_CERT /
# HTTP_C_SERVER_KEY in the environment) for the integration tests that
# spin up an HTTPS endpoint.
#
# Designed for Linux hosts. The container at .devcontainer/Dockerfile
# already has all of the above.
#
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
BUILD_DIR="${REPO_ROOT}/build-coverage"
REPORT_DIR="${BUILD_DIR}/coverage"

if [[ "${1:-}" == "--rebuild" ]]; then
    rm -rf "${BUILD_DIR}"
fi

command -v gcovr >/dev/null 2>&1 || {
    echo "[error] gcovr not found. Install it with:"
    echo "          sudo apt-get install -y gcovr        # debian/ubuntu"
    echo "        or"
    echo "          pipx install gcovr"
    exit 1
}

echo "==> Configuring (HTTP_C_COVERAGE=ON)"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DHTTP_C_COVERAGE=ON >/dev/null

echo "==> Building"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "==> Running ctest"
# Run from inside the build dir so test binaries find their relative
# fixture paths (certs etc.) the same way they do in the regular build.
(cd "${BUILD_DIR}" && ctest --output-on-failure --timeout 60) || {
    echo "[warn] some tests failed; continuing to coverage report so you"
    echo "       can still see what was exercised."
}

echo "==> Generating coverage report"
mkdir -p "${REPORT_DIR}"

# We restrict to the library sources (src/*.c) and exclude:
#   - tests themselves (would just be 100% by definition)
#   - samples (binaries, not libraries)
#   - benchmarks (likewise)
#   - third-party deps that aren't ours -- but we DO include
#     deps/common-lib-c/src/ because that's our own codebase too
gcovr \
    --root "${REPO_ROOT}" \
    --filter "${REPO_ROOT}/src/" \
    --filter "${REPO_ROOT}/deps/common-lib-c/src/" \
    --exclude ".*tests.*" \
    --exclude ".*samples.*" \
    --exclude ".*benchmarks.*" \
    --txt "${REPORT_DIR}/summary.txt" \
    --html-details "${REPORT_DIR}/index.html" \
    --print-summary \
    "${BUILD_DIR}"

echo
echo "==> Done"
echo "    Plain text : ${REPORT_DIR}/summary.txt"
echo "    HTML       : ${REPORT_DIR}/index.html"
