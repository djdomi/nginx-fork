#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 /absolute/path/to/nginx /absolute/path/to/module.so" >&2
    exit 2
fi

nginx_bin=$1
module_so=$2
worker_processes=${RAM_CACHE_WORKERS:-2}
parallel_clients=${RAM_CACHE_PARALLEL_CLIENTS:-100}
case "$worker_processes:$parallel_clients" in
    *[!0-9:]*|:*|*:) echo "worker/client counts must be positive integers" >&2; exit 2 ;;
esac
if [ "$worker_processes" -lt 1 ] || [ "$parallel_clients" -lt 1 ]; then
    echo "worker/client counts must be positive integers" >&2
    exit 2
fi
test_root=$(mktemp -d)
mkdir -p "$test_root/logs"
mkdir -p "$test_root/disk"
mkdir -p "$test_root/disk-fail"
mkdir -p "$test_root/slice"
printf '0123456789abcdefghijklmnopqrst' > "$test_root/slice/slice.bin"
printf 'file-backed response payload\n' > "$test_root/slice/local-file"
head -c 150000 /dev/zero | tr '\000' 'x' > "$test_root/slice/large-file"
head -c 300000 /dev/zero | tr '\000' 'z' > "$test_root/slice/too-large"
head -c 65536 /dev/zero | tr '\000' 'y' > "$test_root/slice/abort.bin"
head -c 131072 /dev/zero | tr '\000' 'w' > "$test_root/slice/worker-kill.bin"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$test_root/test.key" \
    -out "$test_root/test.crt" -days 1 -subj '/CN=localhost' \
    >/dev/null 2>&1
port_origin=18082
port_proxy=18083
port_tls=18443

cleanup() {
    if [ -f "$test_root/nginx.pid" ]; then
        kill -QUIT "$(cat "$test_root/nginx.pid")" >/dev/null 2>&1 || true
    fi
    sleep 1
    rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

cat > "$test_root/nginx.conf" <<EOF
load_module $module_so;
worker_processes $worker_processes;
pid nginx.pid;
error_log stderr notice;
events { worker_connections 256; }
http {
    ram_cache_zone l1 16m;
    ram_cache_max_object 256k;
    ram_cache_ttl 10;
    ram_cache_purge_token 0123456789abcdef0123456789abcdef;
    proxy_cache_path $test_root/disk levels=1:2 keys_zone=disk:4m;
    proxy_cache_path $test_root/disk-fail levels=1:2 keys_zone=disk_fail:2m;
    log_format origin '\$uri \$http_range';
    server {
        listen 127.0.0.1:$port_origin;
        access_log $test_root/origin.log origin;
        location = /object { default_type text/plain; return 200 "origin payload\\n"; }
        location = /scheme { default_type text/plain; return 200 "plain origin\\n"; }
        location = /no-store {
            default_type text/plain;
            add_header Cache-Control no-store always;
            return 200 "origin no-store\\n";
        }
        location = /set-cookie {
            default_type text/plain;
            add_header Set-Cookie 'session=private' always;
            return 200 "origin cookie\\n";
        }
        location = /query-key {
            default_type text/plain;
            return 200 "\$arg_id";
        }
        location = /expire { default_type text/plain; return 200 "expiring\\n"; }
        location = /abort.bin { root $test_root/slice; limit_rate 32k; }
        location = /worker-kill.bin { root $test_root/slice; limit_rate 32k; }
        location = /slow-origin.bin {
            alias $test_root/slice/abort.bin;
            limit_rate 32k;
        }
        location /l2/ { default_type text/plain; return 200 "origin payload\\n"; }
        location / { root $test_root/slice; }
    }
    server {
        listen 127.0.0.1:$port_proxy reuseport;
        location /l2/ {
            ram_cache off;
            proxy_cache disk;
            proxy_cache_key \$uri;
            proxy_cache_valid 200 1m;
            add_header X-Disk-Cache \$upstream_cache_status;
            proxy_pass http://127.0.0.1:$port_origin;
        }
        location = /metrics {
            ram_cache off;
            default_type application/json;
            return 200 '{"hits":\$ram_cache_hits,"misses":\$ram_cache_misses,"l2_hits":\$ram_cache_l2_hits,"l2_misses":\$ram_cache_l2_misses,"objects":\$ram_cache_object_count,"used_bytes":\$ram_cache_used_bytes,"building":\$ram_cache_building_count,"building_bytes":\$ram_cache_building_bytes,"evictions":\$ram_cache_evictions,"allocation_failures":\$ram_cache_allocation_failures}';
        }
        location = /origin-abort.bin {
            ram_cache on;
            proxy_cache disk;
            proxy_cache_key "\$scheme\$proxy_host\$uri\$is_args\$args";
            proxy_cache_valid 200 1m;
            proxy_read_timeout 200ms;
            add_header X-Worker \$pid always;
            proxy_pass http://127.0.0.1:$port_origin/slow-origin.bin;
        }
        location = /l2-write-fail {
            ram_cache on;
            proxy_cache disk_fail;
            proxy_cache_key \$uri;
            proxy_cache_valid 200 1m;
            proxy_pass http://127.0.0.1:$port_origin/l2/;
        }
        location = /slice.bin {
            ram_cache on;
            slice 4;
            proxy_cache disk;
            proxy_cache_key "\$scheme\$proxy_host\$uri\$is_args\$args\$slice_range";
            proxy_cache_valid 200 206 1m;
            proxy_set_header Range \$slice_range;
            proxy_pass http://127.0.0.1:$port_origin;
        }
        location = /local-file {
            ram_cache off;
            proxy_cache disk;
            proxy_cache_key "\$scheme\$proxy_host\$uri\$is_args\$args";
            proxy_cache_valid 200 1m;
            add_header X-Disk-Cache \$upstream_cache_status;
            proxy_pass http://127.0.0.1:$port_origin;
        }
        location / {
            ram_cache on;
            proxy_cache disk;
            proxy_cache_key "\$scheme\$proxy_host\$uri\$is_args\$args";
            proxy_cache_valid 200 206 1m;
            add_header X-Worker \$pid always;
            proxy_pass http://127.0.0.1:$port_origin;
        }
    }
    server {
        listen 127.0.0.1:$port_tls ssl;
        ssl_certificate $test_root/test.crt;
        ssl_certificate_key $test_root/test.key;
        location = /scheme {
            ram_cache on;
            proxy_cache disk;
            proxy_cache_key "\$scheme\$proxy_host\$uri\$is_args\$args";
            proxy_cache_valid 200 1m;
            proxy_pass http://127.0.0.1:$port_origin/l2/;
        }
    }
}
EOF

"$nginx_bin" -t -p "$test_root/" -c nginx.conf
"$nginx_bin" -p "$test_root/" -c nginx.conf
chmod 500 "$test_root/disk-fail"
curl --noproxy '*' --fail --silent -D "$test_root/miss.h" \
    "http://127.0.0.1:$port_proxy/object" -o "$test_root/miss.body"
curl --noproxy '*' --fail --silent -D "$test_root/hit.h" \
    "http://127.0.0.1:$port_proxy/object" -o "$test_root/hit.body"
cmp "$test_root/miss.body" "$test_root/hit.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/hit.h"
rg -qi '^X-RAM-Cache-Status: L1_HIT' "$test_root/hit.h"
curl --noproxy '*' --silent --head -D "$test_root/head-hit.h" \
    "http://127.0.0.1:$port_proxy/object" -o /dev/null
rg -qi '^X-RAM-Cache-Status: L1_HIT' "$test_root/head-hit.h"
rg -qi '^X-RAM-Cache: HIT' "$test_root/head-hit.h"
rg -qi "^Content-Length: $(wc -c < "$test_root/miss.body" | tr -d ' ')\\r?$" \
    "$test_root/head-hit.h"

# Query parameters remain part of object identity.
for identity in alpha beta; do
    curl --noproxy '*' --fail --silent -D "$test_root/query-$identity-first.h" \
        "http://127.0.0.1:$port_proxy/query-key?id=$identity" \
        -o "$test_root/query-$identity-first.body"
    printf '%s' "$identity" > "$test_root/query-$identity-expected.body"
    cmp "$test_root/query-$identity-expected.body" \
        "$test_root/query-$identity-first.body"
done
curl --noproxy '*' --fail --silent -D "$test_root/query-alpha-hit.h" \
    "http://127.0.0.1:$port_proxy/query-key?id=alpha" \
    -o "$test_root/query-alpha-hit.body"
cmp "$test_root/query-alpha-expected.body" "$test_root/query-alpha-hit.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/query-alpha-hit.h"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/query-beta-first.h"; then
    echo "different query parameter incorrectly reused an L1 object" >&2
    exit 1
fi

# A failed L2 cache-file install must not publish an L1 object. The origin
# response remains available, but every request must continue to miss L1.
printf 'origin payload\n' > "$test_root/expected-l2-write-fail.body"
for attempt in first second; do
    curl --noproxy '*' --fail --silent -D "$test_root/l2-write-fail-$attempt.h" \
        "http://127.0.0.1:$port_proxy/l2-write-fail" \
        -o "$test_root/l2-write-fail-$attempt.body"
    cmp "$test_root/l2-write-fail-$attempt.body" \
        "$test_root/expected-l2-write-fail.body"
    if rg -qi '^X-RAM-Cache: HIT' "$test_root/l2-write-fail-$attempt.h"; then
        echo "response with failed L2 persistence unexpectedly hit L1" >&2
        exit 1
    fi
done
chmod 700 "$test_root/disk-fail"

curl --noproxy '*' --fail --silent -D "$test_root/file-miss.h" \
    "http://127.0.0.1:$port_proxy/local-file" -o "$test_root/file-miss.body"
curl --noproxy '*' --fail --silent -D "$test_root/file-hit.h" \
    "http://127.0.0.1:$port_proxy/local-file" -o "$test_root/file-hit.body"
cmp "$test_root/slice/local-file" "$test_root/file-miss.body"
cmp "$test_root/file-miss.body" "$test_root/file-hit.body"
rg -qi '^X-Disk-Cache: MISS' "$test_root/file-miss.h"
rg -qi '^X-Disk-Cache: HIT' "$test_root/file-hit.h"

curl --noproxy '*' --fail --silent -D "$test_root/large-miss.h" \
    "http://127.0.0.1:$port_proxy/large-file" -o "$test_root/large-miss.body"
curl --noproxy '*' --fail --silent -D "$test_root/large-hit.h" \
    "http://127.0.0.1:$port_proxy/large-file" -o "$test_root/large-hit.body"
cmp "$test_root/slice/large-file" "$test_root/large-miss.body"
cmp "$test_root/large-miss.body" "$test_root/large-hit.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/large-hit.h"

# Objects above the configured cap remain on the ordinary proxy-cache path.
curl --noproxy '*' --fail --silent -D "$test_root/oversize-first.h" \
    "http://127.0.0.1:$port_proxy/too-large" \
    -o "$test_root/oversize-first.body"
curl --noproxy '*' --fail --silent -D "$test_root/oversize-second.h" \
    "http://127.0.0.1:$port_proxy/too-large" \
    -o "$test_root/oversize-second.body"
cmp "$test_root/slice/too-large" "$test_root/oversize-first.body"
cmp "$test_root/oversize-first.body" "$test_root/oversize-second.body"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/oversize-second.h"; then
    echo "object above ram_cache_max_object unexpectedly hit L1" >&2
    exit 1
fi

# Origin cache-control and Set-Cookie responses never enter the shared L1.
for resource in no-store set-cookie; do
    curl --noproxy '*' --fail --silent -D "$test_root/$resource-first.h" \
        "http://127.0.0.1:$port_proxy/$resource" \
        -o "$test_root/$resource-first.body"
    curl --noproxy '*' --fail --silent -D "$test_root/$resource-second.h" \
        "http://127.0.0.1:$port_proxy/$resource" \
        -o "$test_root/$resource-second.body"
    cmp "$test_root/$resource-first.body" "$test_root/$resource-second.body"
    if rg -qi '^X-RAM-Cache: HIT' "$test_root/$resource-second.h"; then
        echo "$resource response unexpectedly hit L1" >&2
        exit 1
    fi
done
rg -qi '^Cache-Control: no-store' "$test_root/no-store-second.h"
rg -qi '^Set-Cookie: session=private' "$test_root/set-cookie-second.h"

# HTTP and HTTPS are distinct resource identities even when Host and URI match.
curl --noproxy '*' --fail --silent -D "$test_root/http-scheme.h" \
    -H 'Host: cache.test' "http://127.0.0.1:$port_proxy/scheme" \
    -o "$test_root/http-scheme.body"
curl --noproxy '*' --fail --silent -k -D "$test_root/https-scheme-miss.h" \
    -H 'Host: cache.test' "https://127.0.0.1:$port_tls/scheme" \
    -o "$test_root/https-scheme-miss.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/http-scheme.h" && {
    echo "initial HTTP request unexpectedly hit RAM cache" >&2
    exit 1
}
rg -qi '^X-RAM-Cache: HIT' "$test_root/https-scheme-miss.h" && {
    echo "HTTPS request incorrectly reused the HTTP RAM object" >&2
    exit 1
}
printf 'plain origin\n' > "$test_root/expected-http-scheme.body"
printf 'origin payload\n' > "$test_root/expected-https-scheme.body"
cmp "$test_root/expected-http-scheme.body" "$test_root/http-scheme.body"
cmp "$test_root/expected-https-scheme.body" "$test_root/https-scheme-miss.body"
curl --noproxy '*' --fail --silent -k -D "$test_root/https-scheme-hit.h" \
    -H 'Host: cache.test' "https://127.0.0.1:$port_tls/scheme" \
    -o "$test_root/https-scheme-hit.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/https-scheme-hit.h"
cmp "$test_root/expected-https-scheme.body" "$test_root/https-scheme-hit.body"

curl --noproxy '*' --fail --silent -D "$test_root/slice-miss.h" \
    "http://127.0.0.1:$port_proxy/slice.bin" -o "$test_root/slice-miss.body"
first_slice_origin_count=$(rg -c '^/slice.bin bytes=' "$test_root/origin.log")
curl --noproxy '*' --fail --silent -D "$test_root/slice-hit.h" \
    "http://127.0.0.1:$port_proxy/slice.bin" -o "$test_root/slice-hit.body"
second_slice_origin_count=$(rg -c '^/slice.bin bytes=' "$test_root/origin.log")
cmp "$test_root/slice/slice.bin" "$test_root/slice-miss.body"
cmp "$test_root/slice-miss.body" "$test_root/slice-hit.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/slice-hit.h"
if [ "$first_slice_origin_count" -ne 8 ] \
    || [ "$second_slice_origin_count" -ne "$first_slice_origin_count" ]; then
    echo "slice response was not assembled from eight cached L1 segments" >&2
    exit 1
fi

curl --noproxy '*' --fail --silent -D "$test_root/slice-range.h" \
    -H 'Range: bytes=2-17' "http://127.0.0.1:$port_proxy/slice.bin" \
    -o "$test_root/slice-range.body"
dd if="$test_root/slice/slice.bin" of="$test_root/slice-range.expected" \
    bs=1 skip=2 count=16 2>/dev/null
cmp "$test_root/slice-range.expected" "$test_root/slice-range.body"
rg -qi '^Content-Range: bytes 2-17/30' "$test_root/slice-range.h"
third_slice_origin_count=$(rg -c '^/slice.bin bytes=' "$test_root/origin.log")
if [ "$third_slice_origin_count" -ne "$first_slice_origin_count" ]; then
    echo "multi-slice Range request unexpectedly contacted origin" >&2
    exit 1
fi
rg -qi '^ETag:' "$test_root/slice-hit.h"
rg -qi '^Last-Modified:' "$test_root/slice-hit.h"
rg -qi '^Accept-Ranges: bytes' "$test_root/slice-hit.h"

# The last segment is shorter than the configured four-byte slice size.
curl --noproxy '*' --fail --silent -D "$test_root/slice-last.h" \
    -H 'Range: bytes=28-29' "http://127.0.0.1:$port_proxy/slice.bin" \
    -o "$test_root/slice-last.body"
dd if="$test_root/slice/slice.bin" of="$test_root/slice-last.expected" \
    bs=1 skip=28 count=2 2>/dev/null
cmp "$test_root/slice-last.expected" "$test_root/slice-last.body"
rg -qi '^Content-Range: bytes 28-29/30' "$test_root/slice-last.h"

# Suffix Range is handled by NGINX; its cached final slices remain reusable.
curl --noproxy '*' --fail --silent -D "$test_root/slice-suffix.h" \
    -H 'Range: bytes=-5' "http://127.0.0.1:$port_proxy/slice.bin" \
    -o "$test_root/slice-suffix.body"
dd if="$test_root/slice/slice.bin" of="$test_root/slice-suffix.expected" \
    bs=1 skip=25 count=5 2>/dev/null
cmp "$test_root/slice-suffix.expected" "$test_root/slice-suffix.body"
rg -qi '^Content-Range: bytes 25-29/30' "$test_root/slice-suffix.h"

# In this NGINX Slice setup, multiple client ranges produce the native full
# response. The L1-backed segment stream must preserve that exact behavior.
curl --noproxy '*' --fail --silent -D "$test_root/slice-multi-range.h" \
    -H 'Range: bytes=0-1,10-11' \
    "http://127.0.0.1:$port_proxy/slice.bin" \
    -o "$test_root/slice-multi-range.body"
rg -qi '^HTTP/1.1 200' "$test_root/slice-multi-range.h"
cmp "$test_root/slice/slice.bin" "$test_root/slice-multi-range.body"
multi_range_origin_count=$(rg -c '^/slice.bin bytes=' "$test_root/origin.log")
if [ "$multi_range_origin_count" -ne "$first_slice_origin_count" ]; then
    echo "multi-range handling unexpectedly fetched extra slices" >&2
    exit 1
fi

# If-Range must use NGINX's normal validator path and must not be an L1 HIT.
slice_etag=$(sed -n 's/^ETag: //Ip' "$test_root/slice-hit.h" | tr -d '\r')
curl --noproxy '*' --fail --silent -D "$test_root/slice-if-range.h" \
    -H 'Range: bytes=2-7' -H "If-Range: $slice_etag" \
    "http://127.0.0.1:$port_proxy/slice.bin" \
    -o "$test_root/slice-if-range.body"
dd if="$test_root/slice/slice.bin" of="$test_root/slice-if-range.expected" \
    bs=1 skip=2 count=6 2>/dev/null
cmp "$test_root/slice-if-range.expected" "$test_root/slice-if-range.body"
rg -qi '^Content-Range: bytes 2-7/30' "$test_root/slice-if-range.h"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/slice-if-range.h"; then
    echo "If-Range request unexpectedly bypassed native validation" >&2
    exit 1
fi

# Unsatisfiable byte ranges retain NGINX's native 416 response.
range_code=$(curl --noproxy '*' --silent -o "$test_root/range-416.body" \
    -w '%{http_code}' -H 'Range: bytes=100-200' \
    "http://127.0.0.1:$port_proxy/slice.bin")
if [ "$range_code" != 416 ]; then
    echo "unsatisfiable Range returned $range_code, expected 416" >&2
    exit 1
fi

curl --noproxy '*' --fail --silent -D "$test_root/l2-miss.h" \
    "http://127.0.0.1:$port_proxy/l2/object" -o /dev/null
curl --noproxy '*' --fail --silent -D "$test_root/l2-hit.h" \
    "http://127.0.0.1:$port_proxy/l2/object" -o /dev/null
rg -qi '^X-Disk-Cache: HIT' "$test_root/l2-hit.h"
sed -i 's/ram_cache off;/ram_cache on;/' "$test_root/nginx.conf"
sed -i '/add_header X-Disk-Cache/d' "$test_root/nginx.conf"
kill -HUP "$(cat "$test_root/nginx.pid")"
sleep 1
curl --noproxy '*' --fail --silent -D "$test_root/l2-promote.h" \
    "http://127.0.0.1:$port_proxy/l2/object" -o /dev/null
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' "$test_root/l2-promote.h"
curl --noproxy '*' --fail --silent -D "$test_root/l2-promoted-hit.h" \
    "http://127.0.0.1:$port_proxy/l2/object" -o /dev/null
rg -qi '^X-RAM-Cache: HIT' "$test_root/l2-promoted-hit.h"
rg -qi '^X-RAM-Cache-Status: L1_HIT' "$test_root/l2-promoted-hit.h"

curl --noproxy '*' --fail --silent -D "$test_root/file-promote.h" \
    "http://127.0.0.1:$port_proxy/local-file" -o "$test_root/file-promote.body"
curl --noproxy '*' --fail --silent -D "$test_root/file-promoted-hit.h" \
    "http://127.0.0.1:$port_proxy/local-file" -o "$test_root/file-promoted-hit.body"
cmp "$test_root/slice/local-file" "$test_root/file-promote.body"
cmp "$test_root/file-promote.body" "$test_root/file-promoted-hit.body"
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' "$test_root/file-promote.h"
rg -qi '^X-RAM-Cache: HIT' "$test_root/file-promoted-hit.h"

# PURGE requires the configured token, removes a key, and leaves disk L2 intact.
purge_url="http://127.0.0.1:$port_proxy/l2/object"
purge_code=$(curl --noproxy '*' --silent -o /dev/null -w '%{http_code}' \
    -X PURGE -H 'X-RAM-Cache-Purge: wrong-token' "$purge_url")
if [ "$purge_code" != 403 ]; then
    echo "unauthorized PURGE returned $purge_code, expected 403" >&2
    exit 1
fi
purge_code=$(curl --noproxy '*' --silent -o /dev/null -w '%{http_code}' \
    -X PURGE -H 'X-RAM-Cache-Purge: 0123456789abcdef0123456789abcdef' \
    "$purge_url")
if [ "$purge_code" != 204 ]; then
    echo "authorized PURGE returned $purge_code, expected 204" >&2
    exit 1
fi
curl --noproxy '*' --fail --silent -D "$test_root/purge-promote.h" \
    "$purge_url" -o /dev/null
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' "$test_root/purge-promote.h"
curl --noproxy '*' --fail --silent -D "$test_root/purge-hit.h" \
    "$purge_url" -o /dev/null
rg -qi '^X-RAM-Cache: HIT' "$test_root/purge-hit.h"

# A root PURGE invalidates every L1 object while preserving L2 fallback.
purge_code=$(curl --noproxy '*' --silent -o /dev/null -w '%{http_code}' \
    -X PURGE -H 'X-RAM-Cache-Purge: 0123456789abcdef0123456789abcdef' \
    "http://127.0.0.1:$port_proxy/")
if [ "$purge_code" != 204 ]; then
    echo "full PURGE returned $purge_code, expected 204" >&2
    exit 1
fi
curl --noproxy '*' --fail --silent -D "$test_root/full-purge-promote.h" \
    "http://127.0.0.1:$port_proxy/l2/object" -o /dev/null
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' \
    "$test_root/full-purge-promote.h"

# Expired L1 entries are never hits; a still-valid L2 object can repopulate L1.
curl --noproxy '*' --fail --silent -D "$test_root/expire-miss.h" \
    "http://127.0.0.1:$port_proxy/expire" -o /dev/null
curl --noproxy '*' --fail --silent -D "$test_root/expire-hit.h" \
    "http://127.0.0.1:$port_proxy/expire" -o /dev/null
rg -qi '^X-RAM-Cache: HIT' "$test_root/expire-hit.h"
sleep 11
curl --noproxy '*' --fail --silent -D "$test_root/expire-again.h" \
    "http://127.0.0.1:$port_proxy/expire" -o /dev/null
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' "$test_root/expire-again.h"

curl --noproxy '*' --fail --silent \
    "http://127.0.0.1:$port_proxy/metrics" -o "$test_root/metrics.body"
rg -q '"hits":[1-9][0-9]*' "$test_root/metrics.body"
rg -q '"l2_hits":[1-9][0-9]*' "$test_root/metrics.body"
rg -q '"objects":[1-9][0-9]*' "$test_root/metrics.body"

# Full purge and the TTL test removed the earlier /object L1 entry.
curl --noproxy '*' --fail --silent -D "$test_root/reload-rebuild.h" \
    "http://127.0.0.1:$port_proxy/object" -o /dev/null
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' \
    "$test_root/reload-rebuild.h"

kill -HUP "$(cat "$test_root/nginx.pid")"
sleep 1
curl --noproxy '*' --fail --silent -D "$test_root/reload.h" \
    "http://127.0.0.1:$port_proxy/object" -o "$test_root/reload.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/reload.h"

i=0
while [ "$i" -lt "$parallel_clients" ]; do
    i=$((i + 1))
    curl --noproxy '*' --fail --silent -D "$test_root/parallel-$i.h" \
        "http://127.0.0.1:$port_proxy/object" -o /dev/null &
done
wait
i=0
while [ "$i" -lt "$parallel_clients" ]; do
    i=$((i + 1))
    rg -qi '^X-RAM-Cache: HIT' "$test_root/parallel-$i.h"
done
seen_workers=$(rg -io '^X-Worker: [0-9]+' "$test_root"/parallel-*.h \
    | sort -u | wc -l)
minimum_seen_workers=1
if [ "$worker_processes" -gt 1 ] && [ "$parallel_clients" -gt 1 ]; then
    minimum_seen_workers=2
fi
if [ "$seen_workers" -lt "$minimum_seen_workers" ]; then
    echo "shared L1 object was not observed from multiple worker processes" >&2
    exit 1
fi

curl --noproxy '*' --fail --silent -D "$test_root/range.h" \
    -H 'Range: bytes=0-3' "http://127.0.0.1:$port_proxy/object" \
    -o "$test_root/range.body"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/range.h"; then
    echo "Range request unexpectedly hit RAM cache" >&2
    exit 1
fi

curl --noproxy '*' --fail --silent -D "$test_root/cookie.h" \
    -H 'Cookie: session=private' "http://127.0.0.1:$port_proxy/object" \
    -o "$test_root/cookie.body"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/cookie.h"; then
    echo "Cookie request unexpectedly hit RAM cache" >&2
    exit 1
fi

curl --noproxy '*' --fail --silent -D "$test_root/auth.h" \
    -H 'Authorization: Bearer private' "http://127.0.0.1:$port_proxy/object" \
    -o "$test_root/auth.body"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/auth.h"; then
    echo "Authorization request unexpectedly hit RAM cache" >&2
    exit 1
fi

curl --noproxy '*' --fail --silent -D "$test_root/conditional.h" \
    -H 'If-None-Match: "client-validator"' \
    "http://127.0.0.1:$port_proxy/object" -o "$test_root/conditional.body"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/conditional.h"; then
    echo "Conditional request unexpectedly hit RAM cache" >&2
    exit 1
fi

# A reduced max-object size invalidates objects captured under the old config.
curl --noproxy '*' --fail --silent \
    "http://127.0.0.1:$port_proxy/large-file?config-max=1" -o /dev/null
curl --noproxy '*' --fail --silent -D "$test_root/config-max-ready.h" \
    "http://127.0.0.1:$port_proxy/large-file?config-max=1" -o /dev/null
rg -qi '^X-RAM-Cache: HIT' "$test_root/config-max-ready.h"
sed -i 's/ram_cache_max_object 256k;/ram_cache_max_object 64k;/' \
    "$test_root/nginx.conf"
kill -HUP "$(cat "$test_root/nginx.pid")"
sleep 1
curl --noproxy '*' --fail --silent -D "$test_root/config-max-reduced.h" \
    "http://127.0.0.1:$port_proxy/large-file?config-max=1" \
    -o "$test_root/config-max-reduced.body"
cmp "$test_root/slice/large-file" "$test_root/config-max-reduced.body"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/config-max-reduced.h"; then
    echo "oversize object from the old configuration survived reload" >&2
    exit 1
fi
sed -i 's/ram_cache_max_object 64k;/ram_cache_max_object 256k;/' \
    "$test_root/nginx.conf"
kill -HUP "$(cat "$test_root/nginx.pid")"
sleep 1

# A reduced TTL applies to already-published objects after reload.
curl --noproxy '*' --fail --silent \
    "http://127.0.0.1:$port_proxy/object?config-ttl=1" -o /dev/null
curl --noproxy '*' --fail --silent -D "$test_root/config-ttl-ready.h" \
    "http://127.0.0.1:$port_proxy/object?config-ttl=1" -o /dev/null
rg -qi '^X-RAM-Cache: HIT' "$test_root/config-ttl-ready.h"
sed -i 's/ram_cache_ttl 10;/ram_cache_ttl 1;/' "$test_root/nginx.conf"
kill -HUP "$(cat "$test_root/nginx.pid")"
sleep 2
curl --noproxy '*' --fail --silent -D "$test_root/config-ttl-reduced.h" \
    "http://127.0.0.1:$port_proxy/object?config-ttl=1" -o /dev/null
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' \
    "$test_root/config-ttl-reduced.h"
sed -i 's/ram_cache_ttl 1;/ram_cache_ttl 10;/' "$test_root/nginx.conf"
kill -HUP "$(cat "$test_root/nginx.pid")"
sleep 1

# Fill beyond the 16 MiB L1 zone and verify byte-correct LRU fallback to L2.
i=0
while [ "$i" -lt 120 ]; do
    i=$((i + 1))
    curl --noproxy '*' --fail --silent \
        "http://127.0.0.1:$port_proxy/large-file?evict=$i" \
        -o "$test_root/evict.body"
    cmp "$test_root/slice/large-file" "$test_root/evict.body"
done
curl --noproxy '*' --fail --silent \
    "http://127.0.0.1:$port_proxy/metrics" -o "$test_root/eviction-metrics.body"
rg -q '"evictions":[1-9][0-9]*' "$test_root/eviction-metrics.body"
rg -q '"allocation_failures":0' "$test_root/eviction-metrics.body"
curl --noproxy '*' --fail --silent -D "$test_root/evicted-l2-promote.h" \
    "http://127.0.0.1:$port_proxy/large-file?evict=1" \
    -o "$test_root/evicted-l2-promote.body"
cmp "$test_root/slice/large-file" "$test_root/evicted-l2-promote.body"
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' \
    "$test_root/evicted-l2-promote.h"

# Optional stress case: overlap many different keys so fills, admission,
# eviction, and ordinary L2 fallback all run under shared-zone pressure.
if [ "${RAM_CACHE_STRESS:-0}" = 1 ]; then
    stress_rounds=${RAM_CACHE_STRESS_ROUNDS:-1}
    case "$stress_rounds" in
        ''|*[!0-9]*|0) echo "RAM_CACHE_STRESS_ROUNDS must be a positive integer" >&2; exit 2 ;;
    esac
    round=0
    while [ "$round" -lt "$stress_rounds" ]; do
        first=$((121 + round * 120))
        last=$((first + 119))
        i=$first
        while [ "$i" -le "$last" ]; do
            (
                curl --noproxy '*' --fail --silent --max-time 60 \
                    "http://127.0.0.1:$port_proxy/large-file?concurrent=$i" \
                    -o "$test_root/concurrent-$i.body" \
                    && cmp "$test_root/slice/large-file" \
                        "$test_root/concurrent-$i.body" \
                    && touch "$test_root/concurrent-$i.ok"
            ) &
            i=$((i + 1))
        done
        wait
        i=$first
        while [ "$i" -le "$last" ]; do
            if [ ! -f "$test_root/concurrent-$i.ok" ]; then
                echo "concurrent multi-key request $i failed or returned corrupt bytes" >&2
                exit 1
            fi
            i=$((i + 1))
        done
        round=$((round + 1))
    done
    curl --noproxy '*' --fail --silent \
        "http://127.0.0.1:$port_proxy/metrics" \
        -o "$test_root/concurrent-stress-metrics.body"
    rg -q '"evictions":[1-9][0-9]*' \
        "$test_root/concurrent-stress-metrics.body"
fi

# Force the origin to stop making progress mid-response. The partial response
# must be discarded, and a later complete L2/L1 fill must use exact bytes.
if curl --noproxy '*' --fail --silent --max-time 5 \
    -D "$test_root/upstream-abort.h" \
    "http://127.0.0.1:$port_proxy/origin-abort.bin" \
    -o "$test_root/upstream-abort.body"; then
    echo "rate-limited upstream did not time out during the partial response" >&2
    exit 1
fi
sed -i 's/proxy_read_timeout 200ms;/proxy_read_timeout 10s;/' \
    "$test_root/nginx.conf"
sed -i '/location = \/slow-origin.bin {/,/}/ s/limit_rate 32k;/limit_rate 0;/' \
    "$test_root/nginx.conf"
kill -HUP "$(cat "$test_root/nginx.pid")"
sleep 1
curl --noproxy '*' --fail --silent -D "$test_root/upstream-recovery.h" \
    "http://127.0.0.1:$port_proxy/origin-abort.bin" \
    -o "$test_root/upstream-recovery.body"
cmp "$test_root/slice/abort.bin" "$test_root/upstream-recovery.body"
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_MISS' \
    "$test_root/upstream-recovery.h"
curl --noproxy '*' --fail --silent -D "$test_root/upstream-recovered-hit.h" \
    "http://127.0.0.1:$port_proxy/origin-abort.bin" \
    -o "$test_root/upstream-recovered-hit.body"
cmp "$test_root/slice/abort.bin" "$test_root/upstream-recovered-hit.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/upstream-recovered-hit.h"

# Abort a throttled response before completion; a later full response must be
# complete and may only be cached after its own valid completion.
if curl --noproxy '*' --silent --limit-rate 1k --max-time 0.1 \
    "http://127.0.0.1:$port_proxy/abort.bin" \
    -o "$test_root/abort-partial.body"; then
    echo "throttled client request did not abort as expected" >&2
    exit 1
fi
curl --noproxy '*' --fail --silent -D "$test_root/abort-recovery.h" \
    "http://127.0.0.1:$port_proxy/abort.bin" \
    -o "$test_root/abort-recovery.body"
cmp "$test_root/slice/abort.bin" "$test_root/abort-recovery.body"
curl --noproxy '*' --fail --silent -D "$test_root/abort-recovered-hit.h" \
    "http://127.0.0.1:$port_proxy/abort.bin" \
    -o "$test_root/abort-recovered-hit.body"
cmp "$test_root/slice/abort.bin" "$test_root/abort-recovered-hit.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/abort-recovered-hit.h"

# Kill the proxy worker while it is filling a new, rate-limited L1/L2 object.
# The incomplete body must remain invisible, the master must respawn the worker,
# and a later complete fetch must be byte-correct before becoming an L1 hit.
curl --noproxy '*' --fail --silent -D "$test_root/worker-kill-first.h" \
    --limit-rate 8k "http://127.0.0.1:$port_proxy/worker-kill.bin" \
    -o "$test_root/worker-kill-partial.body" &
worker_kill_curl_pid=$!
sleep 0.2
worker_kill_pid=$(rg -io '^X-Worker: [0-9]+' \
    "$test_root/worker-kill-first.h" | awk '{print $2}' | head -n 1)
if [ -z "$worker_kill_pid" ]; then
    echo "could not identify proxy worker during partial fill" >&2
    exit 1
fi
kill -KILL "$worker_kill_pid"
if wait "$worker_kill_curl_pid"; then
    echo "client transfer survived killing its proxy worker" >&2
    exit 1
fi
sleep 1
curl --noproxy '*' --fail --silent -D "$test_root/worker-kill-recovery.h" \
    "http://127.0.0.1:$port_proxy/worker-kill.bin" \
    -o "$test_root/worker-kill-recovery.body"
cmp "$test_root/slice/worker-kill.bin" \
    "$test_root/worker-kill-recovery.body"
if rg -qi '^X-RAM-Cache: HIT' "$test_root/worker-kill-recovery.h"; then
    echo "incomplete object survived worker death and became an L1 hit" >&2
    exit 1
fi
curl --noproxy '*' --fail --silent -D "$test_root/worker-kill-hit.h" \
    "http://127.0.0.1:$port_proxy/worker-kill.bin" \
    -o "$test_root/worker-kill-hit.body"
cmp "$test_root/slice/worker-kill.bin" "$test_root/worker-kill-hit.body"
rg -qi '^X-RAM-Cache: HIT' "$test_root/worker-kill-hit.h"
curl --noproxy '*' --fail --silent \
    "http://127.0.0.1:$port_proxy/metrics" -o "$test_root/worker-kill-metrics.body"
rg -q '"building":0,"building_bytes":0' \
    "$test_root/worker-kill-metrics.body"

# Pin an L1 object during a slow client transfer, PURGE it concurrently, and
# verify that the live response completes before the object is freed.
curl --noproxy '*' --fail --silent -D "$test_root/pinned-transfer.h" \
    --limit-rate 32k "http://127.0.0.1:$port_proxy/abort.bin" \
    -o "$test_root/pinned-transfer.body" &
pinned_curl_pid=$!
sleep 0.2
purge_code=$(curl --noproxy '*' --silent -o /dev/null -w '%{http_code}' \
    -X PURGE -H 'X-RAM-Cache-Purge: 0123456789abcdef0123456789abcdef' \
    "http://127.0.0.1:$port_proxy/abort.bin")
if [ "$purge_code" != 204 ]; then
    echo "PURGE during pinned transfer returned $purge_code, expected 204" >&2
    exit 1
fi
wait "$pinned_curl_pid"
cmp "$test_root/slice/abort.bin" "$test_root/pinned-transfer.body"
curl --noproxy '*' --fail --silent -D "$test_root/pinned-purged-promote.h" \
    "http://127.0.0.1:$port_proxy/abort.bin" \
    -o "$test_root/pinned-purged-promote.body"
cmp "$test_root/slice/abort.bin" "$test_root/pinned-purged-promote.body"
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' \
    "$test_root/pinned-purged-promote.h"

# Reload while a client is pinned to an L1 body. The old worker must retain its
# reference until the response is complete while the new workers start.
curl --noproxy '*' --fail --silent -D "$test_root/reload-pinned.h" \
    --limit-rate 32k "http://127.0.0.1:$port_proxy/abort.bin" \
    -o "$test_root/reload-pinned.body" &
reload_pinned_pid=$!
sleep 0.2
kill -HUP "$(cat "$test_root/nginx.pid")"
sleep 1
wait "$reload_pinned_pid"
cmp "$test_root/slice/abort.bin" "$test_root/reload-pinned.body"

# A full NGINX restart discards L1 but preserves the persistent proxy cache.
curl --noproxy '*' --fail --silent \
    "http://127.0.0.1:$port_proxy/object" -o /dev/null
curl --noproxy '*' --fail --silent -D "$test_root/restart-ready.h" \
    "http://127.0.0.1:$port_proxy/object" -o /dev/null
rg -qi '^X-RAM-Cache: HIT' "$test_root/restart-ready.h"
kill -QUIT "$(cat "$test_root/nginx.pid")"
sleep 1
"$nginx_bin" -p "$test_root/" -c nginx.conf
sleep 1
curl --noproxy '*' --fail --silent -D "$test_root/restart-promote.h" \
    "http://127.0.0.1:$port_proxy/object" -o /dev/null
rg -qi '^X-RAM-Cache-Status: L1_MISS_L2_HIT' \
    "$test_root/restart-promote.h"
curl --noproxy '*' --fail --silent -D "$test_root/restart-hit.h" \
    "http://127.0.0.1:$port_proxy/object" -o /dev/null
rg -qi '^X-RAM-Cache: HIT' "$test_root/restart-hit.h"

echo "RAM cache integration checks passed"
