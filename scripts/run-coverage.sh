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

# ---------------------------------------------------------------------------
# Provision the TLS test certificates that the socket/http_endpoint/http_server
# integration tests expect at /tmp/http-c-certs/. Without these, ~1000 lines
# of socket.c / http_connection.c / http_server.c never run and the coverage
# number understates the true exercised surface.
# ---------------------------------------------------------------------------
CERT_DIR="/tmp/http-c-certs"
if [[ ! -f "${CERT_DIR}/server/server.cert.pem" ]]; then
    echo "==> Provisioning TLS test certs under ${CERT_DIR}"
    mkdir -p "${CERT_DIR}/ca" "${CERT_DIR}/client" "${CERT_DIR}/server"

    # Self-signed CA (acts as its own intermediate; the chain.ca is just the
    # CA itself which is enough for the simple verify the tests perform).
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 -sha256 \
        -keyout "${CERT_DIR}/ca/ca.key.pem" \
        -out    "${CERT_DIR}/ca/chain.ca.cert.pem" \
        -subj "/CN=http-c-test-ca" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,digitalSignature" \
        >/dev/null 2>&1

    # Server cert signed by the CA, with localhost SAN.
    openssl req -newkey rsa:2048 -nodes \
        -keyout "${CERT_DIR}/server/server.key.pem" \
        -out    "${CERT_DIR}/server/server.csr.pem" \
        -subj "/CN=localhost" \
        >/dev/null 2>&1
    openssl x509 -req -in "${CERT_DIR}/server/server.csr.pem" \
        -CA "${CERT_DIR}/ca/chain.ca.cert.pem" \
        -CAkey "${CERT_DIR}/ca/ca.key.pem" \
        -CAcreateserial \
        -days 30 -sha256 \
        -extfile <(printf "subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n") \
        -out "${CERT_DIR}/server/server.cert.pem" \
        >/dev/null 2>&1

    # Client cert signed by the same CA.
    openssl req -newkey rsa:2048 -nodes \
        -keyout "${CERT_DIR}/client/client.key.pem" \
        -out    "${CERT_DIR}/client/client.csr.pem" \
        -subj "/CN=http-c-test-client" \
        >/dev/null 2>&1
    openssl x509 -req -in "${CERT_DIR}/client/client.csr.pem" \
        -CA "${CERT_DIR}/ca/chain.ca.cert.pem" \
        -CAkey "${CERT_DIR}/ca/ca.key.pem" \
        -CAcreateserial \
        -days 30 -sha256 \
        -extfile <(printf "extendedKeyUsage=clientAuth\n") \
        -out "${CERT_DIR}/client/client.cert.pem" \
        >/dev/null 2>&1
fi

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
