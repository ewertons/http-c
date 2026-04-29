# http-c sample: media-streaming web portal

A complete demo: an HTTPS server (built with `http-c`) hosts a tiny web
page with **Play video** / **Play audio** / **Stop** buttons. The page
points an HTML5 `<video>` and `<audio>` element at endpoints on the same
server, which streams the media in response to standard HTTP **Range**
requests. Both elements use the `loop` attribute, so playback restarts
forever until the user clicks **Stop**.

```
+------------------+     HTTPS      +-----------------------+
|  Browser         | <------------> |  http-c HTTP server   |
|  <video>/<audio> |   Range: ...   |  + 5 routes           |
+------------------+   206 slices   |  + in-memory MP4/MP3  |
                                    +-----------------------+
```

## Routes

| Method | Path                  | What it returns                               |
|--------|-----------------------|-----------------------------------------------|
| GET    | `/`, `/index.html`    | The player UI (HTML)                          |
| GET    | `/script.js`          | Tiny play/stop controller                     |
| GET    | `/style.css`          | Styling                                       |
| GET    | `/media/sample.mp4`   | Video, **`206 Partial Content`** + `Content-Range` |
| GET    | `/media/sample.mp3`   | Audio, **`206 Partial Content`** + `Content-Range` |

The server reads the static assets and the media files into RAM at
startup; per-request work is just slicing into those buffers.

## Why Range requests, and why each chunk is 4 KB

`http_server`'s default per-slot send buffer is 8 KB
(`HTTP_SERVER_HOST_RESPONSE_BUFFER_SIZE`). The complete HTTP response
(status line + headers + body) must fit in that buffer. Rather than
inflate the buffer for one sample, we serve up to `MEDIA_CHUNK_BYTES`
(4 KB) per response and let the browser ask for as many ranges as it
likes. This is exactly the protocol pattern real video sites use, just
with smaller windows. For each request the handler:

1. Looks for a `Range: bytes=START-END` header on the request.
2. Clamps the requested window to `[0, file_size - 1]`, then trims it
   down to at most `MEDIA_CHUNK_BYTES`.
3. Replies with `206 Partial Content`,
   `Content-Range: bytes START-END/TOTAL`, `Content-Length`, and the
   slice as the body.

If the client doesn't send `Range` (curl without `-r`, for example),
the handler defaults to the whole file and then trims to the 4 KB cap;
the client gets the first 4 KB and would need to request the next.

## What this sample is NOT

- **Not** chunked transfer-encoding (the library does not implement
  `Transfer-Encoding: chunked` yet).
- **Not** adaptive bitrate / HLS / DASH. There is no manifest. Just one
  MP4 and one MP3 each, served byte-for-byte over Range requests.
- **Not** fancy. There is no per-request file I/O, no MIME sniffing, no
  directory listing -- only the five routes above.

## Quick start (Windows host, Docker Desktop)

```cmd
samples\media_streaming\launch_with_docker.cmd
```

That single command:

1. Builds `http-c-media-streaming:latest` (Ubuntu 24.04 + build deps +
   `openssl` + `ffmpeg`). Cached after the first run.
2. Generates a stable self-signed `localhost` cert under
   `%TEMP%\http-c-media-streaming-cert` (one-time; reused on later runs).
3. **Imports that cert into your `Cert:\CurrentUser\Root` store** (no
   admin rights needed) so Edge / Chrome trust `https://localhost:8086`
   without a warning.
4. Runs the image with the repo bind-mounted read-only at `/src`, the
   cert dir bind-mounted at `/certs`, and port 8086 forwarded to the
   host. The container's entrypoint:
   - Configures + builds `media_streaming_sample` out-of-tree under
     `/build`.
   - Synthesises a 5-second `sample.mp4` (320x240 testsrc + 440Hz tone)
     and `sample.mp3` (880Hz tone) with **ffmpeg's `lavfi` sources** --
     no external assets required.
   - Starts the server, loading the bind-mounted cert/key.
5. **On Ctrl+C, removes the cert from your trust store.**

When you see `Ready. Open https://localhost:8086/ in your browser.`,
do exactly that. Edge or Chrome will load it cleanly. **Firefox uses
its own cert store** and will still warn -- accept it once or stick to
Edge/Chrome.

Press **Ctrl+C** in the launcher window to stop the server and untrust
the cert. If you instead force-close the window (the X button), the
cert is left in your trust store; the next run reuses it (no
duplicates), or remove it manually via `certmgr.msc` ->
*Trusted Root Certification Authorities* -> *Certificates* ->
"http-c sample".

### Wiping everything

```cmd
samples\media_streaming\cleanup.cmd
```

Idempotent. Stops + removes the container, deletes the
`http-c-media-streaming:latest` image, deletes the cert dir under
`%TEMP%`, and removes **every** "http-c sample" cert from your
`Cert:\CurrentUser\Root` store.

## Quick start (Linux host, no Docker)

If you already have a Linux dev environment with `cmake`, `libssl-dev`,
`openssl` and `ffmpeg`, you can skip the container:

```sh
# 1. Build
cmake -S . -B build
cmake --build build --target media_streaming_sample -j

# 2. Cert
./samples/scripts/generate_certs.sh

# 3. Media (one-time; lavfi generators, no external assets)
mkdir -p samples/media_streaming/media
ffmpeg -hide_banner -y \
    -f lavfi -i "testsrc=size=320x240:rate=24:duration=5" \
    -f lavfi -i "sine=frequency=440:duration=5" -shortest \
    -c:v libx264 -pix_fmt yuv420p -preset veryfast -profile:v baseline \
    -c:a aac -b:a 96k -movflags +faststart \
    samples/media_streaming/media/sample.mp4
ffmpeg -hide_banner -y \
    -f lavfi -i "sine=frequency=880:duration=5" \
    -c:a libmp3lame -b:a 96k \
    samples/media_streaming/media/sample.mp3

# 4. Run
./build/samples/media_streaming/media_streaming_sample
```

Then browse to https://localhost:8086/.

## Bring your own media

If you'd rather see your own files, drop them in as
`samples/media_streaming/media/sample.mp4` and `sample.mp3` (any size up
to `MAX_MEDIA_BYTES` = 16 MB). For the Docker launcher, override the
media directory via an env var (edit `docker/entrypoint.sh` to point
`HTTP_C_MEDIA_ROOT` at a bind-mounted host path).

A few container/codec gotchas worth knowing:

- The browser's `<video>` element wants **MP4 (H.264 + AAC)** or
  **WebM (VP9/AV1 + Opus)**. MKV/AVI usually won't play directly --
  remux first: `ffmpeg -i in.mkv -c copy out.mp4`.
- For MP4, prefer `-movflags +faststart` so the `moov` atom is at the
  front of the file. Without that, browsers can't start playing until
  the whole file is downloaded -- which defeats the streaming.

## Files in this sample

```
samples/media_streaming/
    main.c                 -- the server, route handlers, range parser
    CMakeLists.txt         -- builds media_streaming_sample
    launch_with_docker.cmd -- Windows one-shot launcher
    docker/
        Dockerfile         -- ubuntu:24.04 + build deps + ffmpeg
        entrypoint.sh      -- build + cert + media + run, inside container
    web/
        index.html         -- player UI
        script.js          -- play / stop / loop controller
        style.css          -- minimal dark theme
    media/                 -- (created at runtime; .gitignored)
```

## Configuration env vars

| Variable                | Default                                | Purpose                          |
|-------------------------|----------------------------------------|----------------------------------|
| `HTTP_C_SERVER_CERT`    | `samples/certs/server.cert.pem`        | TLS certificate path             |
| `HTTP_C_SERVER_KEY`     | `samples/certs/server.key.pem`         | TLS private key path             |
| `HTTP_C_WEB_ROOT`       | `samples/media_streaming/web`          | Where to find HTML/JS/CSS        |
| `HTTP_C_MEDIA_ROOT`     | `samples/media_streaming/media`        | Where to find sample.mp4/mp3     |
