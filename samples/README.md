# http-c samples

| Sample | What it shows |
|---|---|
| [`server_client`](server_client/README.md) | In-process HTTPS server + client doing a `GET /` round trip with full wire-level tracing on both sides. The "hello world" of the library. |
| [`streaming`](streaming/README.md) | Client streams a fixed-length request body onto the socket a chunk at a time via `stream_write` on the connection's underlying stream — never materialising the full body in client memory. |
| [`media_streaming`](media_streaming/README.md) | HTTPS web portal: HTML5 `<video>` / `<audio>` player served by the library, with `206 Partial Content` Range responses for the media. One-shot Docker launcher synthesises throwaway MP4/MP3 with ffmpeg. |

Both samples share `samples/scripts/generate_certs.sh` (self-signed cert
generator) and the `samples/certs/` output directory (gitignored).
