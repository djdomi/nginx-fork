#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 /absolute/path/to/nginx /absolute/path/to/module.so" >&2
    exit 2
fi

nginx_bin=$1
module_so=$2
requests=${RAM_CACHE_BENCH_REQUESTS:-60}
concurrency=${RAM_CACHE_BENCH_CONCURRENCY:-10}
runs=${RAM_CACHE_BENCH_RUNS:-3}
root=$(mktemp -d)
port_origin=18182
port_proxy=18183
active_trial=
mkdir -p "$root/logs" "$root/data"
head -c 1048576 /dev/urandom > "$root/data/bench"

case "$requests:$concurrency:$runs" in
    *[!0-9:]*|:*|*:|*::*|0:*|*:0:*|*:*:0)
        echo "request, concurrency, and run counts must be positive integers" >&2
        exit 2
        ;;
esac

cleanup() {
    if [ -n "$active_trial" ] && [ -f "$active_trial/nginx.pid" ]; then
        "$nginx_bin" -p "$active_trial/" -c nginx.conf -s quit \
            >/dev/null 2>&1 || true
        sleep 1
    fi
    rm -rf "$root"
}
trap cleanup EXIT HUP INT TERM

printf 'mode,run,requests,concurrency,wall_seconds,MiB_per_second,p50_ms,p95_ms,p99_ms,origin_requests\n'

run_trial() {
    mode=$1
    run=$2
    trial="$root/$mode-$run"
    active_trial=$trial
    mkdir -p "$trial/logs" "$trial/data" "$trial/disk"
    ln -s "$root/data/bench" "$trial/data/bench"

    {
        if [ "$mode" != baseline ]; then
            printf 'load_module %s;\n' "$module_so"
        fi
        printf 'worker_processes 2;\npid %s/nginx.pid;\nerror_log stderr notice;\n' "$trial"
        cat <<EOF
events { worker_connections 256; }
http {
EOF
        if [ "$mode" != baseline ]; then
            cat <<EOF
    ram_cache_zone l1 64m;
    ram_cache_max_object 8m;
    ram_cache_ttl 300;
EOF
        fi
        cat <<EOF
    proxy_cache_path $trial/disk levels=1:2 keys_zone=disk:4m;
    server {
        listen 127.0.0.1:$port_origin;
        access_log $trial/origin.log;
        location / { root $trial/data; }
    }
    server {
        listen 127.0.0.1:$port_proxy;
        location = /bench {
EOF
        if [ "$mode" = l1 ]; then
            printf '            ram_cache on;\n'
        elif [ "$mode" != baseline ]; then
            printf '            ram_cache off;\n'
        fi
        cat <<EOF
            proxy_cache disk;
            proxy_cache_key \$scheme\$proxy_host\$uri;
            proxy_cache_lock on;
            proxy_cache_valid 200 1m;
            add_header X-Disk-Cache \$upstream_cache_status always;
            proxy_pass http://127.0.0.1:$port_origin;
        }
    }
}
EOF
    } > "$trial/nginx.conf"

    "$nginx_bin" -t -p "$trial/" -c nginx.conf >/dev/null
    "$nginx_bin" -p "$trial/" -c nginx.conf
    curl --noproxy '*' --fail --silent -D "$trial/seed.h" \
        "http://127.0.0.1:$port_proxy/bench" -o /dev/null
    curl --noproxy '*' --fail --silent -D "$trial/warmup.h" \
        "http://127.0.0.1:$port_proxy/bench" -o /dev/null
    if [ "$mode" = l1 ]; then
        rg -qi '^X-RAM-Cache: HIT' "$trial/warmup.h"
    else
        rg -qi '^X-Disk-Cache: HIT' "$trial/warmup.h"
    fi

    curl_config="$trial/curl.conf"
    sample_file="$trial/samples.txt"
    : > "$curl_config"
    i=0
    while [ "$i" -lt "$requests" ]; do
        i=$((i + 1))
        printf 'url = "http://127.0.0.1:%s/bench"\noutput = "/dev/null"\nwrite-out = "%%{http_code} %%{time_total} %%{size_download}\\n"\n' \
            "$port_proxy" >> "$curl_config"
    done

    start_ns=$(date +%s%N)
    curl --noproxy '*' --config "$curl_config" --parallel \
        --parallel-immediate --parallel-max "$concurrency" \
        --connect-timeout 5 --fail --silent --show-error > "$sample_file"
    end_ns=$(date +%s%N)
    wall=$(awk -v start="$start_ns" -v end="$end_ns" \
        'BEGIN { printf "%.6f", (end - start) / 1000000000 }')
    awk -v mode="$mode" -v run="$run" -v expected="$requests" \
        -v concurrency="$concurrency" -v wall="$wall" \
        -v origin_log="$trial/origin.log" '
        $1 == 200 { n++; times[n] = $2; bytes += $3 }
        END {
            if (n != expected) {
                printf "expected %d successful samples, got %d\n", expected, n > "/dev/stderr"
                exit 1
            }
            for (i = 2; i <= n; i++) {
                value = times[i]; j = i - 1
                while (j > 0 && times[j] + 0 > value + 0) {
                    times[j + 1] = times[j]; j--
                }
                times[j + 1] = value
            }
            p50 = int(n * 0.50 + 0.999999)
            p95 = int(n * 0.95 + 0.999999)
            p99 = int(n * 0.99 + 0.999999)
            origin = 0
            while ((getline line < origin_log) > 0) origin++
            printf "%s,%s,%d,%d,%.6f,%.3f,%.3f,%.3f,%.3f,%d\n", \
                mode, run, n, concurrency, wall, bytes / 1048576 / wall, \
                times[p50] * 1000, times[p95] * 1000, times[p99] * 1000, origin
        }
    ' "$sample_file"

    origin_requests=$(wc -l < "$trial/origin.log")
    if [ "$origin_requests" -ne 1 ]; then
        echo "$mode run $run contacted origin $origin_requests times after warmup" >&2
        exit 1
    fi

    "$nginx_bin" -p "$trial/" -c nginx.conf -s quit >/dev/null 2>&1
    sleep 1
    active_trial=
}

for mode in baseline off l1; do
    run=1
    while [ "$run" -le "$runs" ]; do
        run_trial "$mode" "$run"
        run=$((run + 1))
    done
done
