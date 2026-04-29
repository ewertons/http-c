# Media streaming over HTTP: a primer for `http-c`

This document captures the design discussion behind
[`samples/media_streaming`](../samples/media_streaming/README.md) and
explains how the two mainstream browser-video delivery techniques map
onto a minimal HTTP server like `http-c`. It is deliberately
implementation-agnostic; nothing here is specific to our library beyond
the closing section.

---

## 1. HTML5 `<video>` / `<audio>` is the modern baseline

In 2026, the browser's built-in `<video>` and `<audio>` elements +
JavaScript are the standard, correct way to play media in a web
portal. Flash is gone. Silverlight is gone. **HTML5 won.** Every major
streaming site -- YouTube, Netflix, Twitch, Disney+, Spotify Web --
ships exactly this stack. Anything you read suggesting a custom plugin
or applet is at least a decade out of date.

Two delivery patterns sit underneath that element:

1. **Progressive download / byte-range streaming** -- one file, served
   over plain HTTP with `Accept-Ranges: bytes`.
2. **Adaptive bitrate streaming (HLS or DASH)** -- the asset is
   pre-segmented into many small chunks at multiple bitrates plus a
   manifest. A JavaScript player adapts which bitrate it pulls in
   real-time based on observed bandwidth.

Both run on top of plain HTTP(S). **Neither requires any special
"media protocol" support in the server.** The HTTP server's job in
both cases is identical: return bytes for a URL, honour `Range`, set
`Content-Type`. That's it. Everything that makes Netflix feel
Netflix-y is layered on top.

---

## 2. Progressive download / Range streaming

### How it works

- The asset is one file on disk: `video.mp4`, `audio.mp3`, etc.
- The browser opens it via `<video src="/video.mp4">`.
- The first request is typically `GET /video.mp4 HTTP/1.1` with
  `Range: bytes=0-`. The server replies `206 Partial Content` plus
  `Content-Range: bytes 0-N/TOTAL`.
- When the user seeks (drags the scrubber, or jumps via JS), the
  browser issues another Range request for the byte offset that
  corresponds to the target timestamp.
- The browser's `<video>` element handles framing, decoding, A/V
  sync, buffering, and playback control natively.

### Server side

Trivial. **Any** HTTP server that supports the `Range` header works.
There is no transcoding, no manifest, no segmenter, no special media
endpoint -- one file, served as-is.

In `http-c`, this is precisely what
[`samples/media_streaming`](../samples/media_streaming/README.md)
demonstrates: two routes for the media files, two handlers that parse
`Range`, slice into a pre-loaded buffer, and respond with `206`.

### Trade-offs

| Property | Details |
|---|---|
| **Encoder cost** | One pass. Encode once, serve forever. |
| **Storage** | 1x the asset. |
| **Latency to first frame** | Low for short files; with `+movflags faststart` the player can begin decoding after pulling the `moov` atom + a few seconds of media. |
| **Seeking** | Works fine. The browser uses the file's index (`moov`) to translate timestamp -> byte offset and issues a range request to that offset. |
| **Bitrate adaptation** | **None.** One file, one bitrate. If the network slows, the player buffers/stalls. |
| **Resilience over flaky networks** | Hiccup-prone. There is no fallback bitrate. |
| **Live streaming** | Doesn't work. You cannot byte-range a stream that is still being produced. |
| **DRM, ad insertion, multi-language audio** | Awkward at best. |
| **Best fit** | Short-to-medium VOD on stable networks; LAN/local demos; CDN-fronted MP4 downloads where the producer trusts the consumer's bandwidth. |

### Container/codec gotchas

- Browsers want **MP4 (H.264 + AAC)** or **WebM (VP9/AV1 + Opus)**.
  MKV/AVI generally won't play. Remux first:
  `ffmpeg -i in.mkv -c copy out.mp4`.
- For MP4, **always** use `-movflags +faststart` so the `moov` atom is
  at the front of the file. Without it, the player must download the
  whole file before it can start -- which defeats the whole point of
  streaming.
- For audio, MP3 or AAC are the safe choices.

### What it looks like on the wire

```
GET /video.mp4 HTTP/1.1
Range: bytes=0-

HTTP/1.1 206 Partial Content
Content-Type: video/mp4
Content-Length: 4096
Content-Range: bytes 0-4095/106449
Accept-Ranges: bytes
<4096 bytes of MP4>
```

The browser keeps requesting subsequent ranges as the playhead
advances and as you seek. With `http-c`'s 8 KB per-slot send buffer,
the sample caps each response at 4 KB so the full HTTP response
(status line + headers + body) fits.

---

## 3. Adaptive bitrate streaming (HLS / DASH)

### How it works

- An offline ffmpeg job pre-segments the source into many short chunks
  -- typically 2-10 seconds each -- at **multiple bitrates** (e.g.
  240p, 480p, 720p, 1080p), and writes a **manifest** listing them all:
  - **HLS** uses `.m3u8` text manifests and `.ts` or `fMP4` segments.
    Apple's protocol; native in Safari and iOS.
  - **DASH** uses an `.mpd` XML manifest and `.m4s` fMP4 segments.
    More flexible, used by YouTube and Netflix.
- The browser fetches the manifest first via plain HTTP GET.
- A JavaScript player (**hls.js**, **dash.js**, Shaka Player, etc.)
  parses the manifest, then **continuously decides** which bitrate's
  *next* segment to fetch based on:
  - Measured throughput on the previous N segments.
  - Buffer occupancy (how much un-played media it has on hand).
  - Player state (paused, seeking, fullscreen, etc.).
- Segments are fed into the `<video>` element via the **Media Source
  Extensions** (MSE) API, which lets JS append arbitrary fMP4/TS bytes
  to the element's internal source buffer.

### Server side

**Still just an HTTP server.** Each segment and the manifest is a
static file. `http-c` could host an HLS tree as-is -- same `Range` /
`200` plumbing. **The "adaptive" intelligence lives entirely in the
client**; the server has no idea which bitrate any given client is
currently watching, and it doesn't need to.

A real HLS layout:

```
hls/
    master.m3u8          -- top-level: lists each rendition
    240p/
        index.m3u8       -- segment list for 240p
        seg00001.ts
        seg00002.ts
        ...
    480p/
        index.m3u8
        seg00001.ts
        ...
    720p/
        ...
```

Serving that from `http-c` is no different from serving the static
HTML/CSS/JS in our existing sample.

### Trade-offs

| Property | Details |
|---|---|
| **Encoder cost** | Have to transcode N renditions. Expensive at ingest, cheap at serve. |
| **Storage** | Roughly N x the asset (one copy per rendition), plus per-segment overhead. |
| **Latency to first frame** | Low *and* consistent. Players start at a low bitrate (fast first segment) and upshift as the buffer grows. |
| **Seeking** | Excellent. The manifest indexes timestamps to segment URLs; the player jumps directly to the right segment. |
| **Bitrate adaptation** | **Yes.** Network dip -> next segment fetched at lower bitrate. Network recovers -> upshift. Invisible to the user. |
| **Resilience** | Designed for the public internet. Rebuffers are rare. |
| **Live streaming** | **First-class.** Encoder keeps appending new segments to the manifest; players poll for updates. This is how Twitch, YouTube Live, and every IPTV product works. |
| **DRM, ad insertion, multi-language audio, subtitles** | First-class concepts in both HLS and DASH specs. |
| **Best fit** | Anything public-facing, anything live, anything where viewer bandwidth varies. |

### What it looks like on the wire

```
GET /hls/master.m3u8 HTTP/1.1
Host: example.com

HTTP/1.1 200 OK
Content-Type: application/vnd.apple.mpegurl
Content-Length: 380
#EXTM3U
#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=320x240
240p/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=1200000,RESOLUTION=854x480
480p/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1280x720
720p/index.m3u8
```

Then the player, having decided on (say) 480p:

```
GET /hls/480p/index.m3u8 HTTP/1.1   ...
GET /hls/480p/seg00001.ts HTTP/1.1  ...
GET /hls/480p/seg00002.ts HTTP/1.1  ...
```

After segment 3 it notices throughput dropped; it switches to 240p:

```
GET /hls/240p/seg00004.ts HTTP/1.1  ...
```

To the server these are just GETs for static files. There is **no
session, no protocol upgrade, no streaming socket**. Each segment is
its own self-contained HTTP transaction.

---

## 4. Why YouTube and Netflix feel faster than a single MP4

Three reasons, none of which are "their HTTP server is faster":

1. **CDN proximity.** The next segment is cached on a server 5 ms
   away from the viewer, not 80 ms. This is largely independent of the
   delivery technique -- but adaptive streaming benefits more because
   each individual request is small and latency-sensitive.
2. **Adaptive bitrate.** The *first* segment is intentionally
   low-quality so it arrives in well under a second. Quality ramps up
   while you are already watching. Progressive download cannot do
   this -- there is one file at one bitrate, and the only way to
   discover that the network can't keep up is to fail.
3. **Generous read-ahead buffers.** The player fetches several
   segments ahead so a network blip never reaches your eyes. This
   works *because* segments are small and indexed -- with
   progressive Range requests the browser can do something similar,
   but it has no guidance on segment boundaries.

Progressive download can match #1 with a CDN, but it can't do #2 or
#3 by definition.

---

## 5. Comparison at a glance

| | Progressive / Range | Adaptive (HLS/DASH) |
|---|---|---|
| Server complexity | Trivial (`http-c` does this today) | Trivial (still just static GETs) |
| Encoder complexity | Single encode | Multi-rendition transcode + segmentation |
| Storage | 1x | N x (number of bitrates) |
| Manifest | None | Required (`.m3u8` / `.mpd`) |
| Adapts to bandwidth | No | Yes |
| Live streaming | No | Yes |
| Seeking | Good | Excellent |
| DRM / ads / subs | Awkward | First-class |
| Browser support | Native everywhere | Safari = native HLS; others need hls.js / dash.js (MSE) |
| First-frame latency | Variable | Consistently low |
| Player code | None (`<video src=...>`) | hls.js / dash.js / Shaka |

---

## 6. What this means for `http-c`

`http-c` is a **plain HTTP server**. From its point of view, both
techniques look identical: incoming `GET`, outgoing `200` (or `206`)
with the right headers. So:

- **Progressive Range streaming**: implemented today in
  [`samples/media_streaming`](../samples/media_streaming/README.md).
  No library work needed.
- **HLS / DASH**: requires **zero new library features**. To add a
  hypothetical `samples/hls_streaming` you would only need:
  1. An ffmpeg invocation (or a pre-segmenting pipeline) that emits
     the segments + manifest into a directory.
  2. A few more routes that serve those static files. The library
     already does this perfectly.
  3. A different `index.html` that loads `hls.js`, points it at
     `/hls/master.m3u8`, and attaches it to a `<video>` element.

There is no streaming-protocol surface in `http-c` because there
doesn't need to be one. Adaptive streaming is a *content-shape*
choice, not a *transport* choice.

### Where `http-c`'s current limits do bite

For the progressive case there is one library-imposed quirk: the
**per-slot send buffer is 8 KB** by default
(`HTTP_SERVER_HOST_RESPONSE_BUFFER_SIZE`). The full HTTP response --
status line + headers + body -- must fit in that buffer. Today the
sample handles this by capping each response body at 4 KB and relying
on the browser to ask for the next range. That is the correct
protocol pattern; real video sites do the same with larger windows.

A future enhancement that would make media serving cleaner would be
**streaming response bodies on the server side** (analogous to the
client-side streaming demonstrated in
[`samples/streaming`](../samples/streaming/README.md)) so that a
single response could push an arbitrarily large body without holding
it all in the send buffer at once. That would let the server return a
whole large segment in one HTTP transaction. It is not necessary for
correctness, only for fewer round trips per segment.

### Summary

| Question | Answer |
|---|---|
| Is HTML5 video the right choice in 2026? | **Yes.** It's the only choice. |
| Is `http-c` enough to host a streaming site? | **Yes**, for both progressive and HLS/DASH. The "streaming smarts" live in the encoder and in the browser-side player, not in the server. |
| Why are big platforms faster? | CDNs, adaptive bitrate, and aggressive read-ahead -- none of which are HTTP server features. |
| What would the next sample look like? | An `hls_streaming` variant: same server, same routes-on-static-files pattern, with a one-shot ffmpeg in the entrypoint that produces an HLS ladder, and an `hls.js`-based `index.html`. |
