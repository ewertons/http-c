# http-c sample: client-side streaming upload

A demo of pushing a fixed-length request body onto the wire **a chunk
at a time** without ever holding the whole body in client-side memory.

## What this sample does

1. Starts an HTTPS server on `127.0.0.1:8085` with a `POST /upload` route.
2. Connects an HTTPS client to it.
3. Builds a request with `Content-Length: 4096` and an *empty* body span.
4. Calls `http_connection_send_request` — that flushes only the start
   line + headers + the blank line ending them.
5. Loops generating 8 × 512-byte chunks and pushing each chunk straight
   onto `connection.stream` via `stream_write`.
6. Reads back the server's `200 OK` response.

The client and server independently compute a `(sum * 31) + byte` rolling
hash and print it. Identical hashes prove the streamed bytes round-tripped
intact.

## Expected output

```
http-c streaming sample
  server cert: samples/certs/server.cert.pem
  server key : samples/certs/server.key.pem
  listening  : https://localhost:8085/upload
  body size  : 4096 bytes (8 chunks of 512)

----- CLIENT sending request (streaming body) -----
POST /upload HTTP/1.1  Content-Length: 4096  (8 chunks of 512 bytes)
  client wrote chunk @0 (512 bytes)
  client wrote chunk @512 (512 bytes)
  ...
  client done; expected hash 0x........

----- SERVER received upload -----
  bytes: 4096
  hash : 0x........        <- matches client

----- CLIENT received response -----
status: 200 OK
body: ok

Done.
```

## Build & run

```sh
./samples/scripts/generate_certs.sh        # one-time
cmake -S . -B build
cmake --build build --target streaming_sample
./build/samples/streaming/streaming_sample
```

Cert paths can be overridden via `HTTP_C_SERVER_CERT` and
`HTTP_C_SERVER_KEY` (see [`samples/server_client/README.md`](../server_client/README.md)).

## What "streaming" means here, and what it does *not*

This is **fixed-length streaming**: the client knows the total body size
up front and advertises it via `Content-Length`. The body is written to
the socket incrementally without being copied or buffered in the client.

The library does **not** currently implement:

- **`Transfer-Encoding: chunked`** — required when the client doesn't
  know the body size in advance. Without it, the client must compute the
  size before sending.
- **Server-side streaming receive** — `http_server_t` collects the full
  body into the slot's recv buffer before invoking the route handler.
  That bounds the maximum request body to `recv_buffer_size`
  (`HTTP_SERVER_HOST_REQUEST_BUFFER_SIZE`, 8 KB by default for the host
  storage). This sample sends 4 KB so it fits comfortably.
- **Streaming downloads** — same constraint applies to responses; the
  whole response is materialised into the client's recv buffer before
  parsing.

These would each be a real feature, not a sample workaround.

## Why this technique still works

`http_request_serialize_to` writes the start-line, headers, the blank
line that terminates the header block, and finally the body span. When
the body span is empty it stops at the blank line, and the connection's
underlying `stream_t` is left in a "ready to receive body bytes" state.
Any subsequent `stream_write` calls put bytes onto the wire that the
peer interprets as more body — until exactly `Content-Length` bytes
have arrived.
