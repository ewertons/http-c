#!/usr/bin/env bash
#
# Run the full build + test suite inside the Ubuntu container.
# Generates a minimal self-signed CA + server + client certificate set,
# patches the test source TBD constants, builds, and runs ctest.
#
# Designed to be invoked inside the http-c-build:ubuntu24 image with the
# repository mounted at /work.
#
set -euo pipefail

ROOT=/work
CERT_ROOT=/tmp/http-c-certs
CA_DIR="$CERT_ROOT/ca"
SERVER_DIR="$CERT_ROOT/server"
CLIENT_DIR="$CERT_ROOT/client"

mkdir -p "$CA_DIR" "$SERVER_DIR" "$CLIENT_DIR"

echo "==> Generating self-signed CA"
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -subj "/CN=Test Root CA" \
    -keyout "$CA_DIR/ca.key.pem" \
    -out    "$CA_DIR/ca.cert.pem" >/dev/null 2>&1

echo "==> Generating server certificate (CN=localhost)"
openssl req -newkey rsa:2048 -nodes \
    -subj "/CN=localhost" \
    -keyout "$SERVER_DIR/server.key.pem" \
    -out    "$SERVER_DIR/server.csr.pem" >/dev/null 2>&1

cat > "$SERVER_DIR/server.ext" <<'EOF'
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names
[alt_names]
DNS.1 = localhost
IP.1 = 127.0.0.1
EOF

openssl x509 -req -in "$SERVER_DIR/server.csr.pem" \
    -CA "$CA_DIR/ca.cert.pem" -CAkey "$CA_DIR/ca.key.pem" -CAcreateserial \
    -out "$SERVER_DIR/server.cert.pem" -days 365 -sha256 \
    -extfile "$SERVER_DIR/server.ext" >/dev/null 2>&1

echo "==> Generating client certificate"
openssl req -newkey rsa:2048 -nodes \
    -subj "/CN=test-client" \
    -keyout "$CLIENT_DIR/client.key.pem" \
    -out    "$CLIENT_DIR/client.csr.pem" >/dev/null 2>&1

cat > "$CLIENT_DIR/client.ext" <<'EOF'
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = clientAuth
EOF

openssl x509 -req -in "$CLIENT_DIR/client.csr.pem" \
    -CA "$CA_DIR/ca.cert.pem" -CAkey "$CA_DIR/ca.key.pem" -CAcreateserial \
    -out "$CLIENT_DIR/client.cert.pem" -days 365 -sha256 \
    -extfile "$CLIENT_DIR/client.ext" >/dev/null 2>&1

# Chain file is just the CA cert (single-level) for the trust store.
cp "$CA_DIR/ca.cert.pem" "$CA_DIR/chain.ca.cert.pem"

CLIENT_CERT="$CLIENT_DIR/client.cert.pem"
CLIENT_KEY="$CLIENT_DIR/client.key.pem"
SERVER_CERT="$SERVER_DIR/server.cert.pem"
SERVER_KEY="$SERVER_DIR/server.key.pem"
CA_CHAIN="$CA_DIR/chain.ca.cert.pem"

echo "==> Patching TBD path constants in test sources"
patch_paths() {
    local file="$1"
    sed -i \
        -e "s@#define CLIENT_CERT_PATH \".*\"@#define CLIENT_CERT_PATH \"$CLIENT_CERT\"@g" \
        -e "s@#define CLIENT_PK_PATH \".*\"@#define CLIENT_PK_PATH \"$CLIENT_KEY\"@g" \
        -e "s@#define SERVER_CERT_PATH \".*\"@#define SERVER_CERT_PATH \"$SERVER_CERT\"@g" \
        -e "s@#define SERVER_PK_PATH \".*\"@#define SERVER_PK_PATH \"$SERVER_KEY\"@g" \
        -e "s@#define CA_CHAIN_PATH \".*\"@#define CA_CHAIN_PATH \"$CA_CHAIN\"@g" \
        "$file"
}

patch_paths "$ROOT/deps/common-lib-c/tests/src/test_socket.c"
patch_paths "$ROOT/tests/test_http_endpoint.c"
patch_paths "$ROOT/tests/test_http_server.c"

echo "==> Configuring + building"
rm -rf "$ROOT/build"
cmake -S "$ROOT" -B "$ROOT/build" >/dev/null
cmake --build "$ROOT/build" -j"$(nproc)"

echo "==> Running tests"
cd "$ROOT/build"
ctest --output-on-failure --timeout 30
