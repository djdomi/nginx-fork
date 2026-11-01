# Initial local benchmark

This is a small reproducible warm-cache comparison, not a production capacity
benchmark. The harness is `benchmark.sh`; it runs the same 1 MiB random payload
through three modes, with fresh NGINX processes and disk-cache directories for
each trial:

- `baseline`: NGINX proxy cache, module not loaded.
- `off`: same NGINX proxy cache, module loaded, `ram_cache off`.
- `l1`: same NGINX proxy cache, `ram_cache on`.

Each trial performs a seed request and a warmup request, verifies the expected
L2 or L1 HIT header, then measures 60 requests at concurrency 10. The origin
log must show exactly one request per trial, confirming that the measured
requests were warm. Each mode was run three times on this environment. The
CSV records raw wall time, aggregate MiB/s, per-request p50/p95/p99 latency,
and origin request count: [raw results](benchmark-results-2026-11-01.csv).

| Mode | Median MiB/s | Range MiB/s | Median p50 ms | Median p95 ms | Median p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline | 1,228.099 | 1,044.568–1,286.560 | 3.751 | 7.696 | 7.797 |
| module off | 1,425.043 | 940.233–1,749.985 | 3.206 | 4.553 | 5.225 |
| L1 on | 1,078.361 | 914.160–1,393.825 | 3.229 | 6.954 | 7.569 |

The run-to-run spread is large. This measurement does not establish a
performance improvement; the L1 median throughput was lower than both
comparison medians. It only measures a warm local loopback path and does not
record NGINX CPU, memory, disk I/O, context switches, cold-cache behavior,
large sequential transfer scaling, random ranges, mixed load, or eviction
pressure. Do not infer production performance from these numbers.

Reproduce with:

```sh
contrib/ngx_http_ram_cache_module/benchmark.sh \
  "$PWD/objs/nginx" "$PWD/objs/ngx_http_ram_cache_module.so"
```

The script defaults to three independent runs, 60 requests per run, and
concurrency 10. `RAM_CACHE_BENCH_RUNS`, `RAM_CACHE_BENCH_REQUESTS`, and
`RAM_CACHE_BENCH_CONCURRENCY` can change those values.
