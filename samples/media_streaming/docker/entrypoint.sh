#!/usr/bin/env bash
#
# Container entrypoint for the http-c media-streaming sample.
#
# The TLS cert is supplied by the *host* (via a bind-mount at /certs)
# so the same cert can be imported into the host browser's trust store
# AND served by this container. The host launcher
# (samples/media_streaming/launch_with_docker.cmd) handles all of that.
#
# This script just:
#   1. Verifies that /certs holds server.cert.pem + server.key.pem.
#   2. Builds the http-c sample out-of-tree under /build.
#   3. Synthesises sample.mp4 / sample.mp3 in /build/media via
#      ffmpeg's lavfi virtual sources -- no external assets needed.
#   4. Launches media_streaming_sample on 0.0.0.0:8086.
#
# The host's source tree is bind-mounted read-only at /src; nothing in
# this script writes back to it.
#
set -euo pipefail

SRC_DIR="${SRC_DIR:-/src}"
BUILD_DIR="${BUILD_DIR:-/build}"
CERT_DIR="${CERT_DIR:-/certs}"
MEDIA_DIR="${BUILD_DIR}/media"

CERT_FILE="${CERT_DIR}/server.cert.pem"
KEY_FILE="${CERT_DIR}/server.key.pem"

echo "==> http-c media-streaming container"
echo "    source : ${SRC_DIR}"
echo "    build  : ${BUILD_DIR}"
echo "    certs  : ${CERT_DIR}"

# --- 1. Cert / key sanity check ----------------------------------------------
if [[ ! -f "${CERT_FILE}" || ! -f "${KEY_FILE}" ]]; then
    echo "[error] expected ${CERT_FILE} and ${KEY_FILE} to be present."
    echo "        Run launch_with_docker.cmd from the host -- it provisions"
    echo "        the cert dir and bind-mounts it at /certs."
    exit 1
fi

# --- 2. Configure and build --------------------------------------------------
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    echo "==> Configuring (first run)"
    cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
fi

echo "==> Building media_streaming_sample"
cmake --build "${BUILD_DIR}" --target media_streaming_sample -- -j"$(nproc)"

# --- 3. Throwaway media via ffmpeg lavfi -------------------------------------
mkdir -p "${MEDIA_DIR}"
VIDEO_FILE="${MEDIA_DIR}/sample.mp4"
AUDIO_FILE="${MEDIA_DIR}/sample.mp3"

if [[ ! -f "${VIDEO_FILE}" ]]; then
    echo "==> Generating sample.mp4 (5s, 320x240 testsrc + 440Hz tone)"
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc=size=320x240:rate=24:duration=5" \
        -f lavfi -i "sine=frequency=440:duration=5" \
        -shortest \
        -c:v libx264 -pix_fmt yuv420p -preset veryfast -profile:v baseline \
        -c:a aac -b:a 96k \
        -movflags +faststart \
        "${VIDEO_FILE}"
fi

if [[ ! -f "${AUDIO_FILE}" ]]; then
    echo "==> Generating sample.mp3 (5s, 880Hz tone)"
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "sine=frequency=880:duration=5" \
        -c:a libmp3lame -b:a 96k \
        "${AUDIO_FILE}"
fi

ls -lh "${VIDEO_FILE}" "${AUDIO_FILE}"

# --- 4. Launch the server ----------------------------------------------------
export HTTP_C_SERVER_CERT="${CERT_FILE}"
export HTTP_C_SERVER_KEY="${KEY_FILE}"
export HTTP_C_WEB_ROOT="${SRC_DIR}/samples/media_streaming/web"
export HTTP_C_MEDIA_ROOT="${MEDIA_DIR}"

echo "==> Launching media_streaming_sample"
echo "    cert = ${CERT_FILE}"
echo "    key  = ${KEY_FILE}"
echo "    Open https://localhost:8086/ in your browser."
echo

exec "${BUILD_DIR}/samples/media_streaming/media_streaming_sample"
