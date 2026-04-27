# Code review — `http-c`

_Date: 2026-04-25 17:26 PDT_

## Summary
The library is at a prototype stage. The public API in [samples/main.c](../samples/main.c) is clean, but the implementation has **several real bugs**, **broken concurrency**, and **performance pitfalls**. The server cannot, in its current form, handle multiple requests in parallel.

---

## 1. Critical bugs

### 1.1 Assignment vs. comparison in cancellation handler
[src/http_server.c](../src/http_server.c)
```c
static void on_http_server_run_cancelled(void* user_args)
{
    http_server_t* server = (http_server_t*)user_args;
    server->state == http_server_state_stopped;   // BUG: `==` should be `=`
}
```
Cancellation never takes effect.

### 1.2 `http_response_serialize_to` — missing `else if`
[src/http_response.c](../src/http_response.c)
```c
else if (failed(stream_write(stream, space, NULL)))
{
    result = error;
}
if (failed(stream_write(stream, response->code, NULL)))   // BUG: should be `else if`
{
    result = error;
}
```
The `code` is written unconditionally even after a previous failure (and `result` may be silently overwritten).

### 1.3 `http_server_run` — uninitialized `result`
[src/http_server.c](../src/http_server.c). On the success path the function simply leaves the `while (running)` loop without ever assigning `result`, returning a garbage `result_t`. `http_server_init` also returns `error` from a failure inside the inner loop without `http_endpoint_deinit`.

### 1.4 `socket_deinit` closes fd 0 (stdin)
[deps/common-lib-c/src/socket.c](../deps/common-lib-c/src/socket.c)
```c
if (socket->listen_sd != -1) close(socket->listen_sd);
if (socket->sd != -1)        close(socket->sd);
```
After `memset(0, sizeof(socket_t))` (done in `socket_init` and `socket_accept`), `listen_sd`/`sd` are **0**, not -1. For a client socket, `listen_sd` is left at 0 and gets `close(0)` (stdin) on deinit. Same goes for any path that creates a `socket_t` but doesn't actually open `listen_sd`/`sd`. Initialize these to `-1` explicitly after `memset`.

### 1.5 `socket_read` returns `no_data`, server checks `try_again`
- [deps/common-lib-c/src/socket.c](../deps/common-lib-c/src/socket.c) returns `no_data` for the `SSL_ERROR_WANT_READ`/non-fatal case.
- [src/http_connection.c](../src/http_connection.c) loop is `while (succeeded(result = stream_read(...)))`. `no_data` is classified as success in [niceties.h](../deps/common-lib-c/inc/niceties.h), so the loop spins **busy-looping** when the peer hasn't sent yet.
- [src/http_server.c](../src/http_server.c) compares against `try_again`, which `socket_read` never returns — any genuine retryable condition becomes fatal.

The two layers disagree on the return-code contract. Pick one (`try_again`) and use it consistently; otherwise add a small mapping helper.

### 1.6 Task pool leak — tasks never released
[deps/common-lib-c/src/task.c](../deps/common-lib-c/src/task.c)
- `inner_thread_function` runs `task->function`, then `pthread_exit`. **`release_task` is never called.**
- `release_task` itself has a typo: `tasks->is_reserved = false;` resets `tasks[0]`, not the failing task.
- `reserve_task`'s logic on `sem_post` failure clears `is_reserved` but does not "un-wait" the semaphore (it can't), and the recovery path is incoherent.
- Pool is only `TASK_COUNT = 10`, and `task_continue_with`, `task_wait_any`, `task_wait_all` are stubs that always return null/false.

After 10 connections (or any 10 spawned tasks) the pool is exhausted forever.

### 1.7 Regex routing is broken at runtime
[src/http_server.c](../src/http_server.c)
```c
span_regex_is_match(request.path, server->routes.list[i].path, ...)
```
`span_regex_is_match` requires **both** spans to be null-terminated:
[deps/common-lib-c/src/span.c](../deps/common-lib-c/src/span.c)
```c
else if (!span_is_null_terminated(string) || !span_is_null_terminated(pattern))
    result = invalid_argument;
```
But `request.path` is produced by `span_split` on `' '` from the raw buffer — its last byte is whatever character precedes the space (e.g. `l` of `/index.html`), never `'\0'`. **No request will ever match any route.** Either copy the path into a temp NUL-terminated buffer or drop the null-termination requirement and use `regnexec`/length-based matching.

### 1.8 Uninitialized `http_response_t` passed to handler
[src/http_server.c](../src/http_server.c)
```c
http_response_t response;
server->routes.list[i].handler(&request, ..., &response, ...);
http_connection_send_response(&connection, &response);
```
The sample handler does `(void)out_response;` — nothing is set. Then `http_response_serialize_to` writes empty `http_version`, `code`, `reason_phrase`, plus an uninitialized `headers.buffer` (potentially garbage pointer). Even if you never write headers (`used_size == 0`) the wire output is malformed. The framework should at minimum initialize `response` to a 500 default, and require the handler to return a status / pre-fill the response.

### 1.9 `http_headers_parse` doesn't actually parse
[src/http_headers.c](../src/http_headers.c)
```c
headers->used_size = span_get_size(raw_headers); // TODO: get the size up to crlf crlf or end.
```
So `used_size` includes any body bytes that happened to be in the receive buffer. Subsequent serialization re-emits them as if they were headers.

### 1.10 No body handling
- `http_request_t`/`http_response_t` have no body field at all.
- Server stops at `\r\n\r\n` and never reads `Content-Length` bytes. POST handlers cannot see the body.
- `http_request_serialize_to` / `http_response_serialize_to` never emit a body either.

### 1.11 Per-request `regcomp`
`span_regex_is_match` calls `regcomp`/`regfree` for every match. Compile each route's pattern **once** at `http_server_add_route` and reuse the `regex_t`.

### 1.12 Fixed 512-byte receive buffer, on the stack
[src/http_server.c](../src/http_server.c)
```c
uint8_t raw_buffer[512];
```
- 512 bytes is barely enough for headers; a typical browser request exceeds that easily and parsing will fail silently (no `\r\n\r\n` ever appears within 512 bytes, the loop continues until the read fails).
- There is no overflow / 431 (request header too large) handling.
- Buffer should be configurable and per-connection (see threading discussion below).

### 1.13 Connection lifecycle / error handling
[src/http_server.c](../src/http_server.c)
- The `while (true)` "current connection" loop only `break`s on `receive_result != try_again` (which it never is — see 1.5). With non-retryable errors there's no graceful close path. With success it does **not** loop back to read another request — it falls through, but only via missing keep-alive logic; there are TODOs in the source acknowledging this.
- No reading of `Connection: close`. No timeout. A client that opens the socket and writes nothing pins the worker forever.
- `http_connection_send_response` failure has a TODO comment but no actual close — connection state becomes inconsistent.

### 1.14 Unused variable `raw_headers`
Both `http_request_serialize_to` and `http_response_serialize_to` declare `span_t raw_headers;` and never use it. Just compile noise — but indicates dead code.

---

## 2. Concurrency: server cannot serve in parallel

Today the architecture is:

```
main thread -> accept() -> handle one connection to completion -> accept() -> ...
```

There is **zero parallelism**. A single slow client (or a TLS handshake) blocks every other client. `http_server_run_async` only moves the same blocking loop to a worker thread; it doesn't add concurrency.

### Recommended changes
1. **Spawn a worker per accepted connection** (or use a thread pool / `epoll`).
   - `socket_accept_async` already exists. The right pattern:
     - Main thread does `accept`.
     - For each accepted `http_connection_t`, dispatch to a worker that runs the request/response loop and closes the connection.
2. **Fix the task pool first** (1.6). It's currently unsafe to spawn threads from it.
3. **Per-connection buffer** — move the `raw_buffer` into a per-connection allocation owned by the worker. The current design with a single 512-byte stack buffer becomes a data race the moment you parallelize.
4. **Use non-blocking accept + a small fixed pool**, or `pthread_create_detached` per connection until you add a real pool.
5. **Listening socket options**: `SO_REUSEADDR` / `SO_REUSEPORT` are not set on `listen_sd`. Add `SO_REUSEADDR` so restarts don't fail with `EADDRINUSE`. Backlog of 5 in `listen()` is small — make it configurable, e.g. `SOMAXCONN`.
6. **TLS handshake** happens inline inside `socket_accept` — that means a slow/malicious client can stall the accept loop. Put the handshake in the worker, not in the accept thread.
7. **No graceful shutdown**: there is no way to stop the server after `http_server_run`. Cancellation flag is broken (1.1) and `accept` is blocking; you'd need to `shutdown(listen_sd, SHUT_RD)` from another thread or use an interruptible accept (`pselect`/`poll`).
8. **Signals**: `SIGPIPE` is not ignored. A peer disconnect during `SSL_write`/`send` will kill the process. Add `signal(SIGPIPE, SIG_IGN)` (or `SO_NOSIGPIPE`/`MSG_NOSIGNAL`).

---

## 3. Client side

`http_endpoint_connect` + `http_connection_receive_response` mirror the server path, so the same bugs apply to clients:
- 1.5 (busy-loop on `no_data`) — clients waiting for a response will spin a CPU.
- 1.10 (no body) — clients can't read response payloads.
- 1.4 (deinit closes fd 0) — every client connection close potentially closes stdin.
- The client has no support for chunked encoding, redirects, retries, or timeouts.
- `socket_connect` doesn't clean up the SSL context on handshake failure (`TODO: destroy SSL components`) — leak.
- `getaddrinfo` requires `client->remote.hostname` to be NUL-terminated, but it's a `span_t`. Same null-termination assumption as the regex bug.

---

## 4. Sub-modules (`common-lib-c`)

- **`socket.c`** is full of `printf` (TODOs say "remove"). Mix of `printf` and `log_error` is inconsistent. Also leaks `client_cert` and `str` on error paths.
- **`task.c`** — see 1.6. `task_continue_with`, `task_wait_any`, `task_wait_all` are unimplemented. The pool semaphore is used as a mutex; use a real `pthread_mutex_t` or atomic CAS.
- **`span_regex_is_match`** allocates `regex_t` per call — this is the wrong abstraction for routing. A `compiled_pattern_t` separate from "string match" would let callers compile once.
- **`stream_t`**: `inner_stream` is `void*` but several call sites cast `(socket_t*)stream` directly, which only works because the struct begins with the function pointers. Brittle; rely on `inner_stream` consistently.
- **Includes**: many headers do not have proper include guards or rely on transitive includes (e.g. `span.h` uses `strlitlen` and `niceties.h`'s macros without including all required system headers like `<string.h>` for `memset`).

---

## 5. API design suggestions (separate from bugs)

- `http_server_config_t` should accept `backlog`, `max_connections`, `request_buffer_size`, `keep_alive_timeout_ms`.
- Route registration: `http_server_add_route` should accept the compiled regex and the count of expected captures, and return a route id.
- Handler signature should let the user **return** a status code and the framework set sensible defaults; alternately provide `http_response_set_status(...)`, `http_response_set_body(...)`, `http_response_add_header(...)` helpers — currently the user has to know about the buffer-management contract of `http_headers_t`.
- `MAX_SERVER_ROUTE_COUNT 10` is small; either make it configurable at compile time or use a small dynamic array.
- `http_endpoint_t` exposes its `socket_config_t` as a stored member and `http_endpoint_init` for the **client** path doesn't actually create a socket — confusing dual semantics. Split into `http_client_endpoint_t` / `http_server_endpoint_t`.

---

## 6. Top fixes, in priority order

1. Fix the assignment bug in cancellation (1.1) and the missing `else if` in response serialization (1.2).
2. Initialize `socket_t` fds to `-1` in `socket_init`/`socket_accept` (1.4).
3. Unify `try_again` semantics between `socket_read` and the HTTP layer (1.5) and stop the busy-loop.
4. Fix the regex null-termination contract (1.7) and pre-compile patterns at `add_route` time (1.11).
5. Initialize `http_response_t` to a sane default before invoking the handler (1.8).
6. Make `http_headers_parse` actually delimit at `\r\n\r\n` (1.9), and add a body field/length to request and response (1.10).
7. Repair the task pool: release tasks on completion, fix `release_task`, replace the semaphore-as-mutex (1.6).
8. Restructure the run loop to dispatch each accepted connection to a worker, with per-connection buffers, `SO_REUSEADDR`, `SIGPIPE` ignored, configurable backlog, and a real shutdown path (Section 2).
9. Implement keep-alive properly: read `Connection`/`Content-Length`, loop on the same connection until close.
10. Replace `printf`s with the existing `logging_simple` calls and clean up TODOs.

Until 1–7 are addressed the server will not correctly serve a single real HTTP request; until 8–9 are addressed it cannot serve them in parallel.
