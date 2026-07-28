/*
** SQLite loadable extension for making HTTP requests via libcurl.
**
** Core function:
**   http_request_json(method, url, body, content_type, headers)
**     -> JSON object: {"status": <int>, "body": <text>, "curl_error": <text|null>}
**
** Convenience wrappers (http_get, http_post, http_head) all funnel through
** the same do_http_request() helper below, so each SQL call performs
** exactly one network request. Callers extract $.status / $.body /
** $.curl_error from that single JSON result with json_extract() instead of
** calling separate status()/body() functions -- two separate calls would
** silently fire two independent HTTP requests, which is wrong for
** non-idempotent methods (POST, PUT, DELETE, ...).
**
** headers is an optional JSON array of "Key: Value" strings, e.g.
**   '["Authorization: Bearer xyz", "X-Custom: 1"]'
*/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>

/* ---- growable buffer for curl write callback ---- */
struct curl_buf {
  char *data;
  size_t len;
  size_t cap;
};

static int buf_init(struct curl_buf *b) {
  b->cap = 4096;
  b->len = 0;
  b->data = (char *)malloc(b->cap);
  if (!b->data) return 1;
  b->data[0] = '\0';
  return 0;
}

static size_t buf_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  struct curl_buf *b = (struct curl_buf *)userdata;
  size_t add = size * nmemb;
  if (b->len + add + 1 > b->cap) {
    size_t newcap = b->cap;
    while (newcap < b->len + add + 1) newcap *= 2;
    char *nd = (char *)realloc(b->data, newcap);
    if (!nd) return 0; /* signals error to curl */
    b->data = nd;
    b->cap = newcap;
  }
  memcpy(b->data + b->len, ptr, add);
  b->len += add;
  b->data[b->len] = '\0';
  return add;
}

/* Append a JSON-escaped string (without surrounding quotes) to a sqlite3_str. */
static void json_escape_append(sqlite3_str *s, const char *text, sqlite3_int64 n) {
  for (sqlite3_int64 i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    switch (c) {
      case '"':  sqlite3_str_appendall(s, "\\\""); break;
      case '\\': sqlite3_str_appendall(s, "\\\\"); break;
      case '\n': sqlite3_str_appendall(s, "\\n"); break;
      case '\r': sqlite3_str_appendall(s, "\\r"); break;
      case '\t': sqlite3_str_appendall(s, "\\t"); break;
      default:
        if (c < 0x20) {
          char tmp[8];
          sqlite3_snprintf(sizeof(tmp), tmp, "\\u%04x", c);
          sqlite3_str_appendall(s, tmp);
        } else {
          sqlite3_str_appendchar(s, 1, (char)c);
        }
    }
  }
}

/* Performs a single HTTP request and returns a malloc'd (via sqlite3_str)
** JSON object string: {"status":N,"body":"...","curl_error":null|"..."}.
** Caller owns the returned sqlite3_free-able string. NULL on OOM. */
static char *do_http_request(
  sqlite3 *db,
  const char *method,
  const char *url,
  const char *body, sqlite3_int64 blen,
  const char *ctype,
  const char *headers_json,
  int *pOutLen
) {
  CURL *curl = curl_easy_init();
  if (!curl) return NULL;

  struct curl_buf resp;
  if (buf_init(&resp)) {
    curl_easy_cleanup(curl);
    return NULL;
  }

  struct curl_slist *slist = NULL;
  if (ctype) {
    char *h = sqlite3_mprintf("Content-Type: %s", ctype);
    if (h) { slist = curl_slist_append(slist, h); sqlite3_free(h); }
  }
  if (headers_json) {
    sqlite3_stmt *pStmt = NULL;
    const char *sql = "SELECT value FROM json_each(?1)";
    if (sqlite3_prepare_v2(db, sql, -1, &pStmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(pStmt, 1, headers_json, -1, SQLITE_STATIC);
      while (sqlite3_step(pStmt) == SQLITE_ROW) {
        const char *hv = (const char *)sqlite3_column_text(pStmt, 0);
        if (hv) slist = curl_slist_append(slist, hv);
      }
    }
    sqlite3_finalize(pStmt);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "sqlite-http/1.0");
  if (slist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

  if (strcasecmp(method, "HEAD") == 0) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  }

  if (body) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)blen);
  }

  CURLcode rc = curl_easy_perform(curl);

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  sqlite3_str *out = sqlite3_str_new(db);
  sqlite3_str_appendall(out, "{\"status\":");
  {
    char numbuf[32];
    sqlite3_snprintf(sizeof(numbuf), numbuf, "%ld", status);
    sqlite3_str_appendall(out, numbuf);
  }
  sqlite3_str_appendall(out, ",\"body\":");
  if (rc == CURLE_OK) {
    sqlite3_str_appendchar(out, 1, '"');
    json_escape_append(out, resp.data, (sqlite3_int64)resp.len);
    sqlite3_str_appendchar(out, 1, '"');
  } else {
    sqlite3_str_appendall(out, "null");
  }
  sqlite3_str_appendall(out, ",\"curl_error\":");
  if (rc != CURLE_OK) {
    const char *emsg = curl_easy_strerror(rc);
    sqlite3_str_appendchar(out, 1, '"');
    json_escape_append(out, emsg, (sqlite3_int64)strlen(emsg));
    sqlite3_str_appendchar(out, 1, '"');
  } else {
    sqlite3_str_appendall(out, "null");
  }
  sqlite3_str_appendall(out, "}");

  *pOutLen = sqlite3_str_length(out);
  char *outstr = sqlite3_str_finish(out);

  if (slist) curl_slist_free_all(slist);
  free(resp.data);
  curl_easy_cleanup(curl);

  return outstr;
}

static void result_from_request(
  sqlite3_context *ctx,
  const char *method, const char *url,
  const char *body, sqlite3_int64 blen,
  const char *ctype, const char *headers_json
) {
  if (!url || url[0] == '\0') {
    sqlite3_result_error(ctx, "url is required", -1);
    return;
  }
  int len = 0;
  char *json = do_http_request(sqlite3_context_db_handle(ctx),
                                method, url, body, blen, ctype, headers_json, &len);
  if (!json) {
    sqlite3_result_error_nomem(ctx);
    return;
  }
  sqlite3_result_text(ctx, json, len, sqlite3_free);
}

static const char *text_arg(sqlite3_value *v) {
  return v && sqlite3_value_type(v) != SQLITE_NULL ? (const char *)sqlite3_value_text(v) : NULL;
}

/* http_request_json(method, url, body, content_type, headers) */
static void fn_http_request_json(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *method = argc > 0 ? text_arg(argv[0]) : NULL;
  const char *url    = argc > 1 ? text_arg(argv[1]) : NULL;
  const char *body   = argc > 2 ? text_arg(argv[2]) : NULL;
  sqlite3_int64 blen  = argc > 2 ? sqlite3_value_bytes(argv[2]) : 0;
  const char *ctype  = argc > 3 ? text_arg(argv[3]) : NULL;
  const char *headers = argc > 4 ? text_arg(argv[4]) : NULL;
  result_from_request(ctx, method ? method : "GET", url, body, blen, ctype, headers);
}

/* http_get(url [, headers]) */
static void fn_http_get(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *url = argc > 0 ? text_arg(argv[0]) : NULL;
  const char *headers = argc > 1 ? text_arg(argv[1]) : NULL;
  result_from_request(ctx, "GET", url, NULL, 0, NULL, headers);
}

/* http_post(url, body [, content_type [, headers]]) */
static void fn_http_post(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *url   = argc > 0 ? text_arg(argv[0]) : NULL;
  const char *body  = argc > 1 ? text_arg(argv[1]) : NULL;
  sqlite3_int64 blen = argc > 1 ? sqlite3_value_bytes(argv[1]) : 0;
  const char *ctype = argc > 2 ? text_arg(argv[2]) : "application/json";
  const char *headers = argc > 3 ? text_arg(argv[3]) : NULL;
  result_from_request(ctx, "POST", url, body, blen, ctype, headers);
}

/* http_head(url [, headers]) */
static void fn_http_head(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *url = argc > 0 ? text_arg(argv[0]) : NULL;
  const char *headers = argc > 1 ? text_arg(argv[1]) : NULL;
  result_from_request(ctx, "HEAD", url, NULL, 0, NULL, headers);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_http_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
) {
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  int rc = sqlite3_create_function(db, "http_request_json", 5,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, 0, fn_http_request_json, 0, 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_create_function(db, "http_get", 1,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, 0, fn_http_get, 0, 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_create_function(db, "http_get", 2,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, 0, fn_http_get, 0, 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_create_function(db, "http_post", 2,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, 0, fn_http_post, 0, 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_create_function(db, "http_post", 3,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, 0, fn_http_post, 0, 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_create_function(db, "http_post", 4,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, 0, fn_http_post, 0, 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_create_function(db, "http_head", 1,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, 0, fn_http_head, 0, 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_create_function(db, "http_head", 2,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, 0, fn_http_head, 0, 0);

  return rc;
}
