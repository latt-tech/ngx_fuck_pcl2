#!/bin/sh
# Functional test for ngx_fuck_pcl2.
#
#   ./run-tests.sh /path/to/nginx
#
# Starts nginx with test.conf in a scratch prefix and checks:
#   * "PCL", "PCL/2" and mixed-case spellings -> 444 (closed, no response)
#   * Mozilla, a missing User-Agent, and a server with "off" -> 200
#   * every dropped request is recorded as 444 in the access log
# Exit status is 0 when every case behaves as expected.

set -u

NGINX=${1:-}
if [ -z "$NGINX" ] || [ ! -x "$NGINX" ]; then
    echo "usage: $0 /path/to/nginx" >&2
    exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=${TMPDIR:-/tmp}/ngx_pcl2_test.$$
rm -rf "$PREFIX"
mkdir -p "$PREFIX/logs" "$PREFIX/conf"
cp "$HERE/test.conf" "$PREFIX/conf/test.conf"

URL_A=http://127.0.0.1:18081/
URL_B=http://127.0.0.1:18082/

ngx() {
    "$NGINX" -c conf/test.conf -p "$PREFIX" "$@" >/dev/null 2>&1
}

start() {
    ngx || return 1
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -s -o /dev/null --max-time 1 -A 'probe' "$URL_A" 2>/dev/null; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

stop() {
    "$NGINX" -c conf/test.conf -p "$PREFIX" -s quit >/dev/null 2>&1
    sleep 0.5
}

# prints the HTTP status, or "closed" when the connection was dropped
# without a response (which is exactly what 444 does)
probe() {
    url=$1
    ua=$2
    if [ "$ua" = "-" ]; then
        out=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -H 'User-Agent:' "$url" 2>/dev/null)
    else
        out=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -A "$ua" "$url" 2>/dev/null)
    fi
    case "$out" in
        000|'') echo closed ;;
        *)      echo "$out" ;;
    esac
}

fails=0

check() {
    desc=$1
    want=$2
    got=$3
    if [ "$got" = "$want" ]; then
        echo "ok    $desc -> $got"
    else
        echo "FAIL  $desc: expected $want, got $got"
        fails=$((fails + 1))
    fi
}

if ! start; then
    echo "FAIL  nginx did not come up (see $PREFIX/logs/error.log)"
    stop
    exit 1
fi

check 'UA "PCL/2.11.3"'           closed "$(probe $URL_A 'PCL/2.11.3')"
check 'UA "PCL2/whatever"'        closed "$(probe $URL_A 'PCL2/whatever')"
check 'UA "Pcl"'                  closed "$(probe $URL_A 'Pcl')"
check 'UA "pcl"'                  closed "$(probe $URL_A 'pcl')"
check 'UA "x/PCL/2"'              closed "$(probe $URL_A 'x/PCL/2')"
check 'UA "Mozilla/5.0"'          200 "$(probe $URL_A 'Mozilla/5.0')"
check 'UA "curl/8.4.0"'           200 "$(probe $URL_A 'curl/8.4.0')"
check 'no User-Agent'             200 "$(probe $URL_A -)"
check 'server with "off"'         200 "$(probe $URL_B 'PCL/2')"

stop

blocked=$(grep -c ' 444 ' "$PREFIX/logs/access.log" 2>/dev/null || true)
check 'access log has 5 x 444'    5   "$blocked"

rm -rf "$PREFIX"

if [ "$fails" -eq 0 ]; then
    echo "all checks passed"
    exit 0
fi

echo "$fails check(s) failed"
exit 1
