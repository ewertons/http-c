# API review: lwIP HTTPD (apps/http)

This document captures how lwIP's HTTP server (`apps/http`, commonly
called the *httpd*) is obtained, configured, and used. Unlike Mongoose
or http-c, lwIP is *not* a Linux-first library: it is a TCP/IP stack
intended for MMU-less microcontrollers, so the integration steps and
the API shape are quite different. This is why lwIP was explicitly
excluded from the runtime numbers in
[../benchmarks/REPORT.md](../benchmarks/REPORT.md) and is reviewed here
qualitatively.

- Upstream:    <https://savannah.nongnu.org/projects/lwip/>
  - Mirror:    <https://git.savannah.nongnu.org/cgit/lwip.git>
  - Mirror:    <https://github.com/lwip-tcpip/lwip>
- License:     Modified BSD (3-clause)
- Version sampled: lwIP 2.2.x, contrib 2.2.x
- Distribution: source tarball or git checkout, plus a separate
  `lwip-contrib` repository that hosts the apps and the OS ports.

## 1. Getting the source

lwIP is distributed as two repositories:

```sh
git clone https://git.savannah.nongnu.org/git/lwip.git
git clone https://git.savannah.nongnu.org/git/lwip/lwip-contrib.git
```

The HTTP server source lives under `src/apps/http/` of the main
repository:

```
src/apps/http/
    httpd.c
    fs.c
    fsdata.c          # generated, see "makefsdata" below
    altcp_proxyconnect.c
src/include/lwip/apps/
    httpd.h
    httpd_opts.h
    fs.h
```

The main repo exposes only the stack and the apps; you must combine it
with one of the OS/architecture *ports* to actually compile.
`lwip-contrib/ports/unix/` provides a Linux/macOS TUN/TAP port that is
the only practical way to *exercise* lwIP on a desktop — it is a
simulator, not a production target.

## 2. Build-time configuration

lwIP is configured by writing a `lwipopts.h` header that you place on
the include path. The file is mandatory; lwIP refuses to compile
without it. A minimal HTTPD-capable configuration looks like:

```c
/* lwipopts.h */
#define NO_SYS                          0     /* use OS threads */
#define LWIP_SOCKET                     1
#define LWIP_NETCONN                    1
#define LWIP_TCP                        1
#define LWIP_ALTCP                      1     /* required for TLS */
#define LWIP_ALTCP_TLS                  1
#define LWIP_ALTCP_TLS_MBEDTLS          1     /* the only bundled TLS */

/* httpd-specific knobs (see httpd_opts.h for the full list) */
#define HTTPD_USE_CUSTOM_FSDATA         0
#define HTTPD_ENABLE_HTTPS              1
#define HTTPD_SERVER_PORT               80
#define HTTPD_SERVER_PORT_HTTPS         443

/* Memory budgets — these *are* the back-pressure mechanism. */
#define MEM_SIZE                        (32 * 1024)
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                32
#define PBUF_POOL_SIZE                  16
```

There is no autoconf, no CMake `find_package(lwip)`. Each port ships
its own build glue. `lwip-contrib/ports/unix/` ships a Makefile that
links the simulator binary against your `lwipopts.h` and your TUN/TAP
device.

### TLS

lwIP's HTTPS support (`HTTPD_ENABLE_HTTPS=1`) goes through the
**altcp_tls** abstraction layer, which today is implemented only on top
of **mbedTLS**. There is no OpenSSL backend. The application must:

1. Build mbedTLS separately (lwIP does not bundle it).
2. Provide certificate, key, and chain to
   `altcp_tls_create_config_server_privkey_cert()`.
3. Pass the resulting `struct altcp_tls_config*` to
   `httpd_inits(...)`.

## 3. Static-content model

The lwIP httpd's defining feature is that **its filesystem is generated
at build time, not read at runtime**. The `makefsdata` tool (in
`src/apps/http/makefsdata/`) walks a `fs/` directory and emits a single
C source file containing every file's bytes plus its precomputed HTTP
header:

```sh
cd lwip/src/apps/http/makefsdata
gcc makefsdata.c -o makefsdata
./makefsdata        # reads ./fs/, writes fsdata.c
```

That `fsdata.c` is then compiled into the firmware image. At runtime,
`fs_open(name)` performs a linear scan of the symbol table baked into
that file, returning a pointer + length. There is no malloc, no
filesystem syscalls, no `read(2)`.

The rationale is that the target is typically a 32 KB MMU-less SoC
with no real filesystem; baking the content in is the only sensible
choice. The cost is that *content is immutable without a reflash*.

## 4. Dynamic content (SSI and CGI)

For non-static responses the httpd offers two extension points:

### Server-side includes (SSI)

You register an array of tag names and a single handler:

```c
static const char *ssi_tags[] = { "uptime", "freemem" };

static u16_t ssi_handler(int iIndex, char *pcInsert, int iInsertLen) {
    switch (iIndex) {
        case 0: return snprintf(pcInsert, iInsertLen, "%lu", uptime_seconds());
        case 1: return snprintf(pcInsert, iInsertLen, "%u",  mem_free());
        default: return 0;
    }
}

http_set_ssi_handler(ssi_handler, ssi_tags, LWIP_ARRAYSIZE(ssi_tags));
```

The httpd scans static `.shtml` files for `<!--#tag-->` markers and
replaces them in flight using `pcInsert`.

### CGI

`http_set_cgi_handlers()` registers an array of `{ url, handler }`
pairs. The handler is invoked for `GET` requests whose path matches,
receives the parsed query parameters, and returns the URL of a static
fsdata page to serve as the response:

```c
static const char *led_cgi(int iIndex, int iNumParams,
                           char *pcParam[], char *pcValue[]) {
    /* mutate state based on params, then return a result page */
    return "/led_ok.shtml";
}

static const tCGI cgi_table[] = { { "/led.cgi", led_cgi } };
http_set_cgi_handlers(cgi_table, LWIP_ARRAYSIZE(cgi_table));
```

CGI is `GET`-only. For `POST`, lwIP exposes a separate three-callback
contract:

```c
err_t  httpd_post_begin(void *connection, const char *uri, ...);
err_t  httpd_post_receive_data(void *connection, struct pbuf *p);
void   httpd_post_finished(void *connection,
                           char *response_uri, u16_t response_uri_len);
```

You implement these as plain C functions; the httpd calls them as the
request body streams in. You stream up to `LWIP_HTTPD_POST_MAX_PAYLOAD_LEN`
bytes via `pbuf` chains.

## 5. Initialisation

For HTTP only:

```c
#include "lwip/apps/httpd.h"
httpd_init();
```

For HTTPS:

```c
#include "lwip/apps/httpd.h"
#include "lwip/altcp_tls.h"

struct altcp_tls_config *cfg =
    altcp_tls_create_config_server_privkey_cert(
        privkey, privkey_len, NULL, 0,
        cert, cert_len);

httpd_inits(cfg);    /* note the trailing 's' for "secure" */
```

Both calls are one-shot: the listening PCB is created, registered with
the TCP layer, and from then on lwIP services requests inside its own
tcpip thread (`NO_SYS=0`) or inside `tcpip_input` polling
(`NO_SYS=1`).

## 6. Memory model and concurrency

- The httpd allocates a `struct http_state` per active connection from
  a fixed-size pool (`MEMP_NUM_HTTPD_CONNS`). When the pool is empty,
  new SYNs are dropped at the TCP layer.
- All response payload comes from either fsdata (read-only flash) or
  the user's static `pcInsert` buffer. There is no `malloc` on the
  response path.
- Headers are fully precomputed by `makefsdata`. Content-Length,
  Content-Type, and Cache-Control are baked in at build time.
- Concurrency: in `NO_SYS=1` mode there is only one execution context
  (the main loop). In `NO_SYS=0` mode all callbacks run on the lwIP
  *tcpip thread*, never on application threads. User code that
  interacts with the httpd from another thread must use the lwIP
  message-passing primitives (`tcpip_callback`).

## 7. What the API looks like end-to-end (NO_SYS=1, simulator port)

```c
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/apps/httpd.h"
#include "netif/tapif.h"

static struct netif netif;

int main(void) {
    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr,  192,168, 0, 2);
    IP4_ADDR(&netmask, 255,255,255,0);
    IP4_ADDR(&gw,      192,168, 0, 1);

    lwip_init();
    netif_add(&netif, &ipaddr, &netmask, &gw, NULL,
              tapif_init, ethernet_input);
    netif_set_default(&netif);
    netif_set_up(&netif);

    httpd_init();    /* registers the listening PCB */

    /* Main loop: drive the stack and timers ourselves. */
    for (;;) {
        tapif_poll(&netif);   /* drains pending packets into lwIP */
        sys_check_timeouts();
    }
}
```

Note: this simulator binary requires running with privileges to open
the TUN/TAP device (`CAP_NET_ADMIN` on Linux), and clients must address
the TUN interface IP, not the host's loopback. This is why lwIP cannot
be loaded into a `wrk -h 127.0.0.1` benchmark the way Mongoose and
http-c can.

## 8. Strengths

- **Lowest possible memory floor.** A working HTTPS server in <50 KB
  RAM is realistic with mbedTLS.
- **No allocator on the request path.** All buffers come from compile-
  time pools sized in `lwipopts.h`.
- **Full TCP/IP stack included.** The library provides everything from
  the Ethernet driver glue up to HTTP; there is no kernel underneath.
- **Permissive BSD licence.**

## 9. Limitations / things to watch

- **Static content baked at build time.** Adding or changing a page
  means re-running `makefsdata` and reflashing. Workable on firmware,
  awkward elsewhere.
- **Routing is via CGI table or SSI tags.** There is no concept of a
  REST router; everything that is not a static file is a CGI URL or an
  SSI tag.
- **POST handling requires three callbacks** plumbed at link time, not
  per-route handlers.
- **TLS is mbedTLS-only** through `altcp_tls`. There is no OpenSSL
  adapter upstream.
- **Linux-side benchmarking is unfair to lwIP.** The TUN/TAP simulator
  in `lwip-contrib/ports/unix` adds context switches and a userspace
  TCP/IP stack on top of the kernel one — a configuration that is
  never used in production. lwIP's real strength only shows on bare
  metal where there is no kernel stack.

## 10. Comparison to http-c (this repository)

| Concern              | lwIP httpd                          | http-c                                |
|----------------------|-------------------------------------|---------------------------------------|
| Target platform      | MMU-less RTOS / bare metal          | Linux / POSIX with epoll              |
| Network stack        | provided by lwIP itself             | the host kernel                       |
| TLS backend          | mbedTLS only (via `altcp_tls`)      | OpenSSL                               |
| Static content       | precompiled `fsdata.c` ROM image    | served from a route handler           |
| Dynamic content      | CGI table + SSI tags, or POST hooks | `(method, path) -> handler` table     |
| Memory model         | compile-time `MEMP_NUM_*` pools     | caller-owned slot array (`storage_t`) |
| Configuration        | `lwipopts.h` macros                 | runtime structs at `http_server_init` |
| Threading            | single tcpip thread                 | single epoll event loop               |
| Where it shines      | 32 KB SoC, no OS, no allocator      | Linux service on commodity hardware   |

The two libraries are *complements*, not competitors: lwIP is what you
reach for when there is no operating system; http-c is what you reach
for when there is one.
