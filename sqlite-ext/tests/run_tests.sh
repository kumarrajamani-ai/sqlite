#!/usr/bin/env bash
# Integration tests for the http SQLite extension.
# Spins up a local echo server so tests don't depend on external network.
set -euo pipefail

cd "$(dirname "$0")/.."

# The stock sqlite3 CLI on macOS is built with SQLITE_OMIT_LOAD_EXTENSION,
# so .load is unavailable. Prefer a Homebrew build (or $SQLITE3) that has it.
SQLITE3="${SQLITE3:-sqlite3}"
if "$SQLITE3" :memory: "PRAGMA compile_options;" 2>/dev/null | grep -q OMIT_LOAD_EXTENSION; then
  if [ -x /opt/homebrew/opt/sqlite/bin/sqlite3 ]; then
    SQLITE3=/opt/homebrew/opt/sqlite/bin/sqlite3
  elif [ -x /usr/local/opt/sqlite/bin/sqlite3 ]; then
    SQLITE3=/usr/local/opt/sqlite/bin/sqlite3
  else
    echo "warning: sqlite3 was built with SQLITE_OMIT_LOAD_EXTENSION and no Homebrew sqlite3 found; tests will fail" >&2
  fi
fi

case "$(uname -s)" in
  Darwin) EXT=http.dylib ;;
  *)      EXT=http.so ;;
esac

if [ ! -f "$EXT" ]; then
  echo "Extension $EXT not found; run 'make' first." >&2
  exit 1
fi

PORT=8935
python3 tests/echo_server.py "$PORT" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# wait for the server to come up
for _ in $(seq 1 50); do
  if curl -s -o /dev/null "http://127.0.0.1:$PORT/status/200"; then
    break
  fi
  sleep 0.1
done

BASE="http://127.0.0.1:$PORT"
FAIL=0

run_sql() {
  "$SQLITE3" -bail -noheader -cmd ".load ./$EXT" ":memory:" "$1"
}

check() {
  local desc="$1" expected="$2" actual="$3"
  if [ "$expected" = "$actual" ]; then
    echo "ok   - $desc"
  else
    echo "FAIL - $desc: expected [$expected] got [$actual]"
    FAIL=1
  fi
}

# 1. http_request_json basic GET status
status=$(run_sql "SELECT json_extract(http_request_json('GET','$BASE/status/204',NULL,NULL,NULL),'\$.status');")
check "http_request_json GET status" "204" "$status"

# 2. http_get convenience wrapper
status=$(run_sql "SELECT json_extract(http_get('$BASE/status/404'),'\$.status');")
check "http_get 404 status" "404" "$status"

# 3. single-call status+body agreement (the whole point of this extension)
row=$(run_sql "
  WITH r AS (SELECT http_request_json('GET','$BASE/get',NULL,NULL,NULL) AS j)
  SELECT json_extract(j,'\$.status') || '|' || json_extract(j,'\$.body') FROM r;
")
status="${row%%|*}"
body="${row#*|}"
check "single request status" "200" "$status"
case "$body" in
  *'"method": "GET"'*) echo "ok   - single request body matches status's request" ;;
  *) echo "FAIL - body did not come from the same request: $body"; FAIL=1 ;;
esac

# 4. http_post sends body once, echoed back
body=$(run_sql "SELECT json_extract(http_post('$BASE/post','{\"a\":1}','application/json'),'\$.body');")
case "$body" in
  *'\"a\":1'*|*'"a": 1'*) echo "ok   - http_post body echoed" ;;
  *) echo "FAIL - http_post body mismatch: $body"; FAIL=1 ;;
esac

# 5. custom headers array is forwarded
hdr=$(run_sql "SELECT json_extract(http_request_json('GET','$BASE/headers',NULL,NULL,'[\"X-Test-Header: hello\"]'),'\$.body');")
case "$hdr" in
  *"hello"*) echo "ok   - custom header forwarded" ;;
  *) echo "FAIL - custom header missing: $hdr"; FAIL=1 ;;
esac

# 6. curl_error is null on success
err=$(run_sql "SELECT json_extract(http_request_json('GET','$BASE/status/200',NULL,NULL,NULL),'\$.curl_error');")
check "curl_error null on success" "" "$err"

# 7. curl_error set + status 0 on connection failure
row=$(run_sql "
  SELECT json_extract(http_request_json('GET','http://127.0.0.1:1','',NULL,NULL),'\$.status') || '|' ||
         (json_extract(http_request_json('GET','http://127.0.0.1:1','',NULL,NULL),'\$.curl_error') IS NOT NULL);
")
case "$row" in
  0\|1) echo "ok   - connection failure reports status 0 and curl_error" ;;
  *) echo "FAIL - unexpected failure result: $row"; FAIL=1 ;;
esac

if [ "$FAIL" -eq 0 ]; then
  echo "All tests passed."
else
  echo "Some tests FAILED."
fi
exit $FAIL
