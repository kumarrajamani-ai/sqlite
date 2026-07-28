# sqlite-http

A loadable SQLite extension that makes HTTP requests via libcurl, callable
directly from SQL.

## Design

The core function is:

```sql
http_request_json(method, url, body, content_type, headers) -> TEXT (JSON)
```

It performs **one** HTTP request and returns a single JSON object:

```json
{"status": 200, "body": "...", "curl_error": null}
```

Everything else (`http_get`, `http_post`, `http_head`) is a thin wrapper
around this same function, so it's still exactly one network round trip per
SQL call.

### Why not separate `http_status()` / `http_get_body()` functions?

An earlier version of this extension had separate scalar functions for
status and body. Using both in the same query looks natural:

```sql
-- DON'T: this fires the request TWICE
SELECT http_status('https://api.example.com/orders', ...),
       http_body('https://api.example.com/orders', ...);
```

SQLite has no guarantee that two calls to different (or even the same)
non-deterministic function in one query share a result — each call is
independent. For a `GET` that's wasteful; for a `POST`, `PUT`, or `DELETE`
it silently double-submits the request. `http_request_json()` sidesteps
this entirely: call it once, bind the result to a CTE/subquery, and pull
`status`/`body`/`curl_error` out of that single value with `json_extract()`.

```sql
-- DO: one request, extract everything from the same result
WITH r AS (
  SELECT http_request_json('POST', 'https://api.example.com/orders',
                            '{"item":"widget"}', 'application/json', NULL) AS resp
)
SELECT json_extract(resp, '$.status')     AS status,
       json_extract(resp, '$.body')       AS body,
       json_extract(resp, '$.curl_error') AS curl_error
FROM r;
```

## Functions

| Function | Description |
|---|---|
| `http_request_json(method, url, body, content_type, headers)` | Full control. `method` defaults to `'GET'` if NULL. `body`, `content_type`, `headers` may be NULL. `headers` is a JSON array of `"Key: Value"` strings. |
| `http_get(url [, headers])` | `GET` request. |
| `http_post(url, body [, content_type [, headers]])` | `POST` request. `content_type` defaults to `'application/json'`. |
| `http_head(url [, headers])` | `HEAD` request (no response body). |

All functions return the same `{"status", "body", "curl_error"}` JSON shape
and are marked `SQLITE_DIRECTONLY` (they perform I/O with side effects and
so cannot be used inside triggers, views, CHECK constraints, or by
untrusted/authorized-only contexts).

On a connection-level failure (DNS failure, refused connection, timeout,
etc.) `status` is `0`, `body` is `null`, and `curl_error` holds libcurl's
error string. On a normal HTTP response — including 4xx/5xx — `curl_error`
is `null` and `status`/`body` reflect what the server sent.

## Building

Requires libcurl development headers (`curl-config` or `pkg-config
libcurl` must be on `PATH`) and a C compiler. SQLite headers
(`sqlite3.h`, `sqlite3ext.h`) are vendored in `vendor/`.

```bash
make            # builds http.so (Linux) or http.dylib (macOS, host arch)
```

### macOS universal binary (arm64 + x86_64)

```bash
make macos-universal   # produces a lipo'd universal http.dylib
```

## Loading

```sql
.load ./http.dylib      -- or http.so on Linux
-- or, from a driver: sqlite3_load_extension(db, "./http.dylib", "sqlite3_http_init", &err);
```

> **Note:** the `sqlite3` CLI shipped with macOS is built with
> `SQLITE_OMIT_LOAD_EXTENSION` and cannot `.load` anything. Use a build that
> has extension loading enabled, e.g. `brew install sqlite` and run
> `$(brew --prefix sqlite)/bin/sqlite3`.

## Usage examples

```sql
.load ./http.dylib

-- Simple GET
SELECT json_extract(http_get('https://api.example.com/health'), '$.status');

-- POST with a JSON body
SELECT http_post('https://api.example.com/items', '{"name":"widget"}');

-- Custom headers (JSON array of "Key: Value" strings)
SELECT http_request_json(
  'GET',
  'https://api.example.com/me',
  NULL, NULL,
  '["Authorization: Bearer sk-abc123"]'
);

-- Pull status/body/error out of one request
WITH r AS (SELECT http_get('https://api.example.com/widgets/42') AS resp)
SELECT json_extract(resp, '$.status') AS status,
       json_extract(resp, '$.body')   AS body
FROM r
WHERE json_extract(resp, '$.status') = 200;
```

## Testing

Tests spin up a local Python echo server (`tests/echo_server.py`) so they
don't depend on external network access:

```bash
make test
```

Set `SQLITE3=/path/to/sqlite3` to point the test runner at a specific
extension-loading-capable `sqlite3` binary.

## License

MIT — see [LICENSE](LICENSE).
