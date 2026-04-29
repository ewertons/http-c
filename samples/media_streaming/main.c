/*
 * http-c sample: HTTPS media-streaming portal.
 *
 * What this sample does
 * ---------------------
 *   1. Starts an HTTPS server on 0.0.0.0:8086 with five routes:
 *
 *        GET /                  -> index.html (the player UI)
 *        GET /script.js         -> client-side player script
 *        GET /style.css         -> styling
 *        GET /media/sample.mp4  -> video, served with Range support
 *        GET /media/sample.mp3  -> audio, served with Range support
 *
 *   2. The static HTML/JS/CSS assets and the media files are loaded from
 *      disk into memory at startup. From then on the server only slices
 *      into those buffers; no per-request file I/O.
 *
 *   3. Media routes honour HTTP Range requests and respond with
 *      `206 Partial Content` and a `Content-Range` header. Each response
 *      body is capped at MEDIA_CHUNK_BYTES so it fits comfortably in the
 *      per-slot 8 KB send buffer. Browsers (Chrome, Firefox, Safari)
 *      stitch the many small range responses back together and feed them
 *      into the <video>/<audio> element transparently.
 *
 * What this sample is NOT
 * -----------------------
 *   - It is not Transfer-Encoding: chunked (the library doesn't speak
 *     that yet). Each HTTP response is fully self-contained, with a
 *     known Content-Length, and we just send a lot of them.
 *   - It is not adaptive bitrate / HLS / DASH. There is no manifest;
 *     just plain byte ranges over a single MP4 / MP3 each.
 *
 * Layout expected at runtime (relative to the current working directory):
 *
 *       samples/media_streaming/web/index.html
 *       samples/media_streaming/web/script.js
 *       samples/media_streaming/web/style.css
 *       samples/media_streaming/media/sample.mp4
 *       samples/media_streaming/media/sample.mp3
 *       samples/certs/server.cert.pem
 *       samples/certs/server.key.pem
 *
 * The launch_with_docker.cmd helper sets up that exact layout inside an
 * Ubuntu container. See README.md.
 *
 * Press Ctrl+C to stop the server.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Note: deliberately not including the "common_lib_c.h" umbrella here.
 * It pulls in common-lib-c's `stack_t` (an unrelated container type)
 * which collides with POSIX's `stack_t` from <signal.h>. The http-c
 * headers below give us everything this sample actually needs. */

#include <http_request.h>
#include <http_response.h>
#include "http_methods.h"
#include "http_codes.h"
#include "http_versions.h"
#include "http_headers.h"
#include "http_endpoint.h"
#include "http_connection.h"
#include "http_server.h"
#include "http_server_storage.h"
#include "task.h"

/* ------------------------------------------------------------------------- *
 * Tunables.
 * ------------------------------------------------------------------------- */

#define DEFAULT_SERVER_CERT_PATH "samples/certs/server.cert.pem"
#define DEFAULT_SERVER_PK_PATH   "samples/certs/server.key.pem"
#define SERVER_PORT              8086

#define WEB_ROOT_DEFAULT         "samples/media_streaming/web"
#define MEDIA_ROOT_DEFAULT       "samples/media_streaming/media"

/* Each response body is capped at this many bytes so the full HTTP
 * response (status line + headers + body) fits in the 8 KB per-slot
 * send buffer used by http_server_storage_get_for_server_host(). */
#define MEDIA_CHUNK_BYTES        4096

/* Hard upper bounds on the static asset and media file sizes we will
 * mmap into RAM at startup. Plenty of headroom for the toy assets
 * shipped with this sample. */
#define MAX_ASSET_BYTES          (32 * 1024)
#define MAX_MEDIA_BYTES          (16 * 1024 * 1024)

/* ------------------------------------------------------------------------- *
 * 206 Partial Content - not in http_codes.h, declare locally.
 * ------------------------------------------------------------------------- */

static const span_t HTTP_CODE_206_LOCAL          = span_from_str_literal("206");
static const span_t HTTP_REASON_PHRASE_206_LOCAL = span_from_str_literal("Partial Content");
static const span_t HTTP_HEADER_RANGE_LOCAL      = span_from_str_literal("Range");
static const span_t HTTP_HEADER_CONTENT_RANGE    = span_from_str_literal("Content-Range");

static const span_t TEXT_HTML  = span_from_str_literal("text/html; charset=utf-8");
static const span_t TEXT_CSS   = span_from_str_literal("text/css; charset=utf-8");
static const span_t TEXT_JS    = span_from_str_literal("application/javascript; charset=utf-8");
static const span_t VIDEO_MP4  = span_from_str_literal("video/mp4");
static const span_t AUDIO_MPEG = span_from_str_literal("audio/mpeg");
static const span_t BYTES_UNIT = span_from_str_literal("bytes");

/* ------------------------------------------------------------------------- *
 * In-memory file registry. The handler dispatches purely off user_context.
 * ------------------------------------------------------------------------- */

typedef struct served_file
{
    const char* disk_path;       /* where to read from at startup */
    span_t      content_type;    /* served as Content-Type */
    bool        range_capable;   /* media files = true, html/css/js = false */
    /* Filled in by load_file(): */
    uint8_t*    data;
    uint32_t    size;
} served_file_t;

static served_file_t s_index_html  = { .content_type = { 0 }, .range_capable = false };
static served_file_t s_script_js   = { .content_type = { 0 }, .range_capable = false };
static served_file_t s_style_css   = { .content_type = { 0 }, .range_capable = false };
static served_file_t s_video_mp4   = { .content_type = { 0 }, .range_capable = true  };
static served_file_t s_audio_mp3   = { .content_type = { 0 }, .range_capable = true  };

static result_t load_file(served_file_t* f, const char* path, span_t content_type, uint32_t max_bytes)
{
    f->disk_path    = path;
    f->content_type = content_type;
    f->data         = NULL;
    f->size         = 0;

    FILE* fp = fopen(path, "rb");
    if (fp == NULL)
    {
        fprintf(stderr, "load_file: cannot open '%s'\n", path);
        return error;
    }
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return error;
    }
    long sz = ftell(fp);
    if (sz < 0 || (uint32_t)sz > max_bytes)
    {
        fprintf(stderr, "load_file: '%s' is %ld bytes (limit %u)\n", path, sz, max_bytes);
        fclose(fp);
        return error;
    }
    rewind(fp);

    f->data = (uint8_t*)malloc((size_t)sz);
    if (f->data == NULL)
    {
        fclose(fp);
        return error;
    }
    if (sz > 0 && fread(f->data, 1, (size_t)sz, fp) != (size_t)sz)
    {
        free(f->data);
        f->data = NULL;
        fclose(fp);
        return error;
    }
    fclose(fp);
    f->size = (uint32_t)sz;
    printf("  loaded %s (%u bytes)\n", path, f->size);
    return ok;
}

/* ------------------------------------------------------------------------- *
 * Tiny helpers for response building.
 *
 * The server's event loop is single-threaded and never invokes two
 * handlers concurrently, so static per-handler scratch buffers are safe.
 * ------------------------------------------------------------------------- */

static int format_u32(char* out, size_t cap, uint32_t v)
{
    int n = snprintf(out, cap, "%u", (unsigned)v);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

/* Parse "bytes=START-" or "bytes=START-END" into [start, end_inclusive].
 * Returns false if the header is missing or unparseable; caller should
 * treat that as "client wants the whole resource starting at 0". */
static bool parse_range(span_t value, uint32_t total, uint32_t* out_start, uint32_t* out_end)
{
    /* Copy into a small null-terminated buffer for sscanf. */
    char tmp[64];
    uint32_t n = span_get_size(value);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, span_get_ptr(value), n);
    tmp[n] = '\0';

    unsigned long start = 0, end = 0;
    int matched = sscanf(tmp, "bytes=%lu-%lu", &start, &end);
    if (matched == 2)
    {
        if (start >= total) return false;
        if (end >= total) end = total - 1;
        *out_start = (uint32_t)start;
        *out_end   = (uint32_t)end;
        return true;
    }
    matched = sscanf(tmp, "bytes=%lu-", &start);
    if (matched == 1)
    {
        if (start >= total) return false;
        *out_start = (uint32_t)start;
        *out_end   = total - 1;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- *
 * Static asset handler (HTML / CSS / JS): always returns 200 with the
 * full body. The assets are tiny enough to fit in one response.
 * ------------------------------------------------------------------------- */

static uint8_t  s_static_headers_buf[256];
static char     s_static_clen_buf[16];

static void static_handler(http_request_t*  request,
                           span_t*          path_matches,
                           uint16_t         path_match_count,
                           http_response_t* out_response,
                           void*            user_context)
{
    (void)request;
    (void)path_matches;
    (void)path_match_count;
    served_file_t* f = (served_file_t*)user_context;

    int clen_n = format_u32(s_static_clen_buf, sizeof(s_static_clen_buf), f->size);
    span_t clen_span = span_init((uint8_t*)s_static_clen_buf, (uint32_t)clen_n);

    if (http_headers_init(&out_response->headers,
                          span_init(s_static_headers_buf,
                                    (uint32_t)sizeof(s_static_headers_buf))) != HL_RESULT_OK)
    {
        return;
    }
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CONTENT_TYPE,   f->content_type);
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CONTENT_LENGTH, clen_span);
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CACHE_CONTROL,
                           span_from_str_literal("no-store"));

    out_response->code          = HTTP_CODE_200;
    out_response->reason_phrase = HTTP_REASON_PHRASE_200;
    out_response->body          = span_init(f->data, f->size);
}

/* ------------------------------------------------------------------------- *
 * Media handler (MP4 / MP3): serves a Range slice, capped at
 * MEDIA_CHUNK_BYTES bytes. Always responds with 206 Partial Content,
 * even for the implicit "give me the whole thing" case (start=0,
 * end=size-1) -- the browser is happy either way and using 206
 * uniformly keeps the code simple.
 * ------------------------------------------------------------------------- */

static uint8_t  s_media_headers_buf[256];
static char     s_media_clen_buf[16];
static char     s_media_crange_buf[64];

static void media_handler(http_request_t*  request,
                          span_t*          path_matches,
                          uint16_t         path_match_count,
                          http_response_t* out_response,
                          void*            user_context)
{
    (void)path_matches;
    (void)path_match_count;
    served_file_t* f = (served_file_t*)user_context;

    if (f->size == 0)
    {
        /* No bytes to serve; fall through to 404 by leaving the default
         * response untouched. */
        return;
    }

    /* Determine [start, end] from the Range header, defaulting to the
     * entire file if no/invalid Range header was sent. */
    uint32_t start = 0;
    uint32_t end   = f->size - 1;

    span_t range_value;
    if (http_headers_find(&request->headers, HTTP_HEADER_RANGE_LOCAL, &range_value) == HL_RESULT_OK)
    {
        uint32_t rs = 0, re = 0;
        if (parse_range(range_value, f->size, &rs, &re))
        {
            start = rs;
            end   = re;
        }
    }

    /* Cap chunk size so the response fits in the per-slot send buffer. */
    if (end - start + 1 > MEDIA_CHUNK_BYTES)
    {
        end = start + MEDIA_CHUNK_BYTES - 1;
    }
    uint32_t length = end - start + 1;

    /* Format Content-Length and Content-Range. */
    int clen_n = format_u32(s_media_clen_buf, sizeof(s_media_clen_buf), length);
    span_t clen_span = span_init((uint8_t*)s_media_clen_buf, (uint32_t)clen_n);

    int crange_n = snprintf(s_media_crange_buf, sizeof(s_media_crange_buf),
                            "bytes %u-%u/%u",
                            (unsigned)start, (unsigned)end, (unsigned)f->size);
    if (crange_n <= 0 || (size_t)crange_n >= sizeof(s_media_crange_buf))
    {
        return; /* leave 404 */
    }
    span_t crange_span = span_init((uint8_t*)s_media_crange_buf, (uint32_t)crange_n);

    if (http_headers_init(&out_response->headers,
                          span_init(s_media_headers_buf,
                                    (uint32_t)sizeof(s_media_headers_buf))) != HL_RESULT_OK)
    {
        return;
    }
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CONTENT_TYPE,   f->content_type);
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CONTENT_LENGTH, clen_span);
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_ACCEPT_RANGES,  BYTES_UNIT);
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CONTENT_RANGE,  crange_span);
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CACHE_CONTROL,
                           span_from_str_literal("no-store"));

    out_response->code          = HTTP_CODE_206_LOCAL;
    out_response->reason_phrase = HTTP_REASON_PHRASE_206_LOCAL;
    out_response->body          = span_init(f->data + start, length);

    printf("  %.*s -> 206 [%u-%u/%u] (%u bytes)\n",
           (int)span_get_size(request->path), (const char*)span_get_ptr(request->path),
           (unsigned)start, (unsigned)end, (unsigned)f->size, (unsigned)length);
    fflush(stdout);
}

/* ------------------------------------------------------------------------- *
 * Server task plumbing -- mirrors the structure of the other samples.
 * ------------------------------------------------------------------------- */

typedef struct server_args
{
    const char*               cert_path;
    const char*               key_path;
    http_server_t*            server;
    task_completion_source_t* ready;
} server_args_t;

static void on_server_state_changed(http_server_t*       server,
                                    http_server_state_t  new_state,
                                    void*                user_context)
{
    (void)server;
    server_args_t* args = (server_args_t*)user_context;
    switch (new_state)
    {
        case http_server_state_running:
            (void)task_completion_source_set_result(args->ready, ok);
            break;
        case http_server_state_stopping:
            printf("server: stopping\n");
            break;
        case http_server_state_stopped:
            printf("server: stopped\n");
            break;
        default:
            break;
    }
}

static result_t run_server(void* state, task_t* self)
{
    (void)self;
    server_args_t* args = (server_args_t*)state;

    http_server_config_t cfg = { 0 };
    cfg.port                     = SERVER_PORT;
    cfg.tls.enable               = true;
    cfg.tls.certificate_file     = args->cert_path;
    cfg.tls.private_key_file     = args->key_path;
    cfg.on_state_changed         = on_server_state_changed;
    cfg.on_state_changed_context = args;

    if (http_server_init(args->server, &cfg,
                         http_server_storage_get_for_server_host()) != ok)
    {
        fprintf(stderr, "server: http_server_init failed (cert/key path correct? "
                        "port %d in use?)\n", SERVER_PORT);
        (void)task_completion_source_set_result(args->ready, error);
        return error;
    }

    (void)http_server_add_route(args->server, HTTP_METHOD_GET,
                                span_from_str_literal("^/$"),
                                static_handler, &s_index_html);
    (void)http_server_add_route(args->server, HTTP_METHOD_GET,
                                span_from_str_literal("^/index\\.html$"),
                                static_handler, &s_index_html);
    (void)http_server_add_route(args->server, HTTP_METHOD_GET,
                                span_from_str_literal("^/script\\.js$"),
                                static_handler, &s_script_js);
    (void)http_server_add_route(args->server, HTTP_METHOD_GET,
                                span_from_str_literal("^/style\\.css$"),
                                static_handler, &s_style_css);
    (void)http_server_add_route(args->server, HTTP_METHOD_GET,
                                span_from_str_literal("^/media/sample\\.mp4$"),
                                media_handler, &s_video_mp4);
    (void)http_server_add_route(args->server, HTTP_METHOD_GET,
                                span_from_str_literal("^/media/sample\\.mp3$"),
                                media_handler, &s_audio_mp3);

    result_t run_result = http_server_run(args->server);
    (void)http_server_deinit(args->server);
    return run_result;
}

/* ------------------------------------------------------------------------- *
 * Ctrl+C handler. Sets a global pointer to the running server so the
 * signal handler can ask it to stop, then the main thread joins.
 * ------------------------------------------------------------------------- */

static http_server_t* volatile g_server_for_signal = NULL;

static void on_sigint(int sig)
{
    (void)sig;
    if (g_server_for_signal != NULL)
    {
        (void)http_server_stop(g_server_for_signal);
    }
}

/* ------------------------------------------------------------------------- *
 * Entry point.
 * ------------------------------------------------------------------------- */

static const char* env_or_default(const char* name, const char* fallback)
{
    const char* v = getenv(name);
    return (v != NULL && v[0] != '\0') ? v : fallback;
}

static int join_path(char* out, size_t cap, const char* dir, const char* file)
{
    int n = snprintf(out, cap, "%s/%s", dir, file);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const char* server_cert_path = env_or_default("HTTP_C_SERVER_CERT", DEFAULT_SERVER_CERT_PATH);
    const char* server_key_path  = env_or_default("HTTP_C_SERVER_KEY",  DEFAULT_SERVER_PK_PATH);
    const char* web_root         = env_or_default("HTTP_C_WEB_ROOT",    WEB_ROOT_DEFAULT);
    const char* media_root       = env_or_default("HTTP_C_MEDIA_ROOT",  MEDIA_ROOT_DEFAULT);

    printf("http-c media-streaming sample\n");
    printf("  server cert: %s\n", server_cert_path);
    printf("  server key : %s\n", server_key_path);
    printf("  web root   : %s\n", web_root);
    printf("  media root : %s\n", media_root);
    printf("  listening  : https://0.0.0.0:%d/\n", SERVER_PORT);

    /* --- Load static assets and media into RAM. --- */
    char p[512];
    if (join_path(p, sizeof(p), web_root,   "index.html") < 0 ||
        load_file(&s_index_html, strdup(p), TEXT_HTML, MAX_ASSET_BYTES) != ok) return 1;
    if (join_path(p, sizeof(p), web_root,   "script.js")  < 0 ||
        load_file(&s_script_js,  strdup(p), TEXT_JS,   MAX_ASSET_BYTES) != ok) return 1;
    if (join_path(p, sizeof(p), web_root,   "style.css")  < 0 ||
        load_file(&s_style_css,  strdup(p), TEXT_CSS,  MAX_ASSET_BYTES) != ok) return 1;
    if (join_path(p, sizeof(p), media_root, "sample.mp4") < 0 ||
        load_file(&s_video_mp4,  strdup(p), VIDEO_MP4, MAX_MEDIA_BYTES) != ok) return 1;
    if (join_path(p, sizeof(p), media_root, "sample.mp3") < 0 ||
        load_file(&s_audio_mp3,  strdup(p), AUDIO_MPEG, MAX_MEDIA_BYTES) != ok) return 1;

    if (task_platform_init() != ok)
    {
        fprintf(stderr, "task_platform_init failed\n");
        return 1;
    }

    http_server_t            http_server;
    task_completion_source_t server_ready;
    if (task_completion_source_init(&server_ready) != ok)
    {
        fprintf(stderr, "task_completion_source_init failed\n");
        (void)task_platform_deinit();
        return 1;
    }

    server_args_t server_args = {
        .cert_path = server_cert_path,
        .key_path  = server_key_path,
        .server    = &http_server,
        .ready     = &server_ready,
    };

    task_t* server_task = task_run(run_server, &server_args);
    if (server_task == NULL)
    {
        fprintf(stderr, "task_run(run_server) failed\n");
        (void)task_completion_source_deinit(&server_ready);
        (void)task_platform_deinit();
        return 1;
    }

    if (task_completion_source_wait(&server_ready) != ok)
    {
        (void)task_wait(server_task);
        task_release(server_task);
        (void)task_completion_source_deinit(&server_ready);
        (void)task_platform_deinit();
        return 1;
    }

    /* Server is up. Wire Ctrl+C to a clean stop and block until that
     * happens (or the run loop returns on its own). */
    g_server_for_signal = &http_server;
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    printf("\nReady. Open https://localhost:%d/ in your browser.\n", SERVER_PORT);
    printf("(Self-signed cert -- accept the security warning once.)\n");
    printf("Press Ctrl+C to stop.\n\n");
    fflush(stdout);

    (void)task_wait(server_task);
    task_release(server_task);
    (void)task_completion_source_deinit(&server_ready);
    (void)task_platform_deinit();

    free(s_index_html.data);
    free(s_script_js.data);
    free(s_style_css.data);
    free(s_video_mp4.data);
    free(s_audio_mp3.data);

    printf("Done.\n");
    return 0;
}
