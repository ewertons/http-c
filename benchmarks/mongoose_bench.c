/*
 * Benchmark peer using Mongoose's built-in HTTPS server.
 *
 * Serves GET / with the same "hello\n" body and explicit Content-Length so
 * load generators can use HTTP/1.1 keep-alive. Mongoose runs its own
 * single-threaded poll loop internally.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "mongoose.h"

#ifndef BENCH_CERT_PATH
#define BENCH_CERT_PATH "/tmp/http-c-certs/server/server.cert.pem"
#endif
#ifndef BENCH_KEY_PATH
#define BENCH_KEY_PATH  "/tmp/http-c-certs/server/server.key.pem"
#endif
#ifndef BENCH_URL
#define BENCH_URL "https://0.0.0.0:8444"
#endif

static const char* s_body = "hello\n";
static volatile int s_running = 1;

static void on_sig(int sig) { (void)sig; s_running = 0; }

static void cb(struct mg_connection* c, int ev, void* ev_data)
{
    if (ev == MG_EV_ACCEPT)
    {
        struct mg_tls_opts opts = {
            .cert = mg_file_read(&mg_fs_posix, BENCH_CERT_PATH),
            .key  = mg_file_read(&mg_fs_posix, BENCH_KEY_PATH),
        };
        mg_tls_init(c, &opts);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message* hm = (struct mg_http_message*)ev_data;
        if (mg_match(hm->uri, mg_str("/"), NULL))
        {
            mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "%s", s_body);
        }
        else
        {
            mg_http_reply(c, 404, "", "not found\n");
        }
    }
}

int main(void)
{
    struct mg_mgr mgr;
    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&mgr);
    if (mg_http_listen(&mgr, BENCH_URL, cb, NULL) == NULL)
    {
        fprintf(stderr, "mg_http_listen failed\n");
        return 1;
    }
    fprintf(stderr, "mongoose bench listening on %s\n", BENCH_URL);

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    while (s_running) mg_mgr_poll(&mgr, 1000);
    mg_mgr_free(&mgr);
    return 0;
}
