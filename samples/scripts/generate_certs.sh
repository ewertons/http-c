#!/usr/bin/env bash
#
# Generate a self-signed RSA certificate + private key for the http-c
# sample. The pair is suitable only for local demos: CN=localhost,
# subjectAltName=DNS:localhost,IP:127.0.0.1, valid for 30 days.
#
# Usage:
#   ./generate_certs.sh [output_dir]
#
# If output_dir is omitted it defaults to <repo>/samples/certs.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
DEFAULT_OUT_DIR="$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)/certs"
OUT_DIR="${1:-${DEFAULT_OUT_DIR}}"

CERT_PATH="${OUT_DIR}/server.cert.pem"
KEY_PATH="${OUT_DIR}/server.key.pem"

mkdir -p "${OUT_DIR}"

if [[ -f "${CERT_PATH}" && -f "${KEY_PATH}" ]]; then
    echo "Certificate already exists at:"
    echo "  ${CERT_PATH}"
    echo "  ${KEY_PATH}"
    echo "Delete them first if you want to regenerate."
    exit 0
fi

OPENSSL_CONF="$(mktemp)"
trap 'rm -f "${OPENSSL_CONF}"' EXIT

cat >"${OPENSSL_CONF}" <<'EOF'
[ req ]
distinguished_name = req_distinguished_name
prompt             = no
x509_extensions    = v3_ext

[ req_distinguished_name ]
C  = US
ST = WA
L  = Redmond
O  = http-c sample
CN = localhost

[ v3_ext ]
basicConstraints     = critical, CA:TRUE
keyUsage             = critical, digitalSignature, keyEncipherment, keyCertSign
extendedKeyUsage     = serverAuth
subjectAltName       = @alt_names

[ alt_names ]
DNS.1 = localhost
IP.1  = 127.0.0.1
EOF

echo "Generating self-signed server cert and key into:"
echo "  ${CERT_PATH}"
echo "  ${KEY_PATH}"

openssl req \
    -x509 \
    -newkey rsa:2048 \
    -nodes \
    -days 30 \
    -sha256 \
    -keyout "${KEY_PATH}" \
    -out    "${CERT_PATH}" \
    -config "${OPENSSL_CONF}"

chmod 600 "${KEY_PATH}"
chmod 644 "${CERT_PATH}"

echo
echo "Done."
