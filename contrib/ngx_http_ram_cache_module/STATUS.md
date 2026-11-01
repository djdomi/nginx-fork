# Implementation and verification status

## Scope

This is a standalone, generic NGINX HTTP module in the NGINX fork. The module
is named `ngx_http_ram_cache_module`; LANcache-NG-specific build, runtime,
control-plane, and UI integration are outside this repository's scope. No
issue is a prerequisite for this work.

The supplied target listed NGINX 1.31.9 in this environment. The tested binary
was built from this checkout with GCC 14.2.0 and OpenSSL 3.5.7 using:

```text
--with-compat --with-debug --with-http_ssl_module --with-http_v2_module
--with-http_slice_module
--add-dynamic-module=/workspace/nginx-fork/contrib/ngx_http_ram_cache_module
```

The active dynamic module built and loaded in that binary. This verifies this
build only; it is not a general ABI compatibility matrix.

## Implemented behavior

- A dynamic module adds a `PRECONTENT` lookup and response header/body filters.
  It uses NGINX shared memory and the slab allocator, with two byte-accounted
  SLRU queues, 64 KiB maximum slabs, an 8 MiB configured-object ceiling, and a
  10% in-progress capture reservation ceiling.
- Complete response bodies are copied into shared memory. A `READY` object is
  visible only after capture completion, L2 freshness validation, and a
  generation check. Lookup uses a full key comparison after scanning the
  queues; it does not rely on a hash-only identity.
- Response objects are pinned while being sent. Purge and expiry remove them
  from lookup immediately and defer freeing pinned objects until cleanup.
  Allocation and capture failures fall through to NGINX's normal response.
- New objects enter probation; repeat hits promote them to protected, whose
  accounted bytes are capped at 80% of the shared-zone size. Allocation
  pressure evicts unpinned probation objects first, then protected objects.
- L1 freshness is bounded by the configured TTL and the NGINX file-cache
  `valid_sec`. Admission requires `r->cache` and a still-valid L2 entry at body
  completion. The ordinary NGINX proxy path remains responsible for L2 and
  origin requests.
- HTTP and HTTPS, Host, URI, query, slice range, and request headers form the
  L1 key. Authorization, cookies, conditional requests, and non-slice Range
  requests bypass L1. The deliberately broad request-header key avoids merging
  requests that may differ in representation, but does not guarantee identity
  with a site's configured `proxy_cache_key`.
- Native slice subrequests are admitted as individual segments only when the
  slice range and body length validate and a strong ETag is present. Native
  NGINX slice/range processing constructs the client response.
- Token-protected targeted and full L1 PURGE are implemented. PURGE does not
  invalidate NGINX's disk cache. Runtime counters and response status headers
  are exposed.

## Executed verification

The following commands completed successfully against the checkout:

```text
make -j4 modules
contrib/ngx_http_ram_cache_module/test.sh \
  "$PWD/objs/nginx" "$PWD/objs/ngx_http_ram_cache_module.so"
```

The test executes the real NGINX binary with two workers and verifies:

- `nginx -t`, module load, ordinary MISS and subsequent L1 HIT.
- A 150,000-byte file-backed response across multiple 64 KiB body blocks and
  byte-for-byte response comparison.
- Shared hits observed from at least two worker PIDs. The live integration
  suite passed for these worker/client pairs: 1/1, 2/10, 4/50, and 8/100.
- Distinct HTTP and HTTPS cache keys.
- Native 4-byte slice caching: eight initial segment requests, no additional
  origin requests on replay, correct body and validators, plus non-aligned,
  suffix, last-short-segment, If-Range, and unsatisfiable Range checks with
  exact body/status/`Content-Range` assertions. In this NGINX 1.31.9 setup,
  multiple client ranges with `slice` return the native full 200 response; the
  test verifies the byte-identical response and that cached slices avoid extra
  origin requests. Standard multipart-206 behavior is not provided by this
  `slice` configuration.
- L2 disk-cache hit followed by L1 promotion, including file-backed output.
- Targeted and full L1 PURGE, wrong-token rejection, preservation of L2, and
  repromotion from L2.
- L1 TTL expiry and repromotion from still-valid L2.
- L1 retention across NGINX reload, 100 concurrent requests to the same cached
  object, metrics variables, and bypass for Range, Cookie, Authorization, and
  conditional requests.

The localhost test requires permission to bind loopback ports in restricted
execution environments. It was run with that permission and passed.

An initial benchmark was also run with three fresh trials per mode, each using
60 warm requests at concurrency 10 against the same 1 MiB payload. It validates
the HIT status before measurement and confirms one origin request per trial.
The measured median throughput was 1,228.099 MiB/s without the module,
1,425.043 MiB/s with the module off, and 1,078.361 MiB/s with L1 on. The
run-to-run spread was wide; these data do not demonstrate a performance gain.
Raw values and the method are in `benchmark-results-2026-11-01.csv` and
`BENCHMARK.md`.

## Not yet verified / not implemented

This implementation is not production-ready. The supplied completion criteria
that remain open include:

- No unit tests for key collisions, state transitions, partial block frees,
  SLRU byte limits, or allocator failures; no automated adversarial key test.
- No full 4-by-4 worker/client cross-product, sanitizer, race detector, or
  long-running test. The tested pairs are 1/1, 2/10, 4/50, and 8/100. Parallel
  requests all hit the same warmed object; different-object load is not yet
  covered. Cancellation,
  upstream abort, worker death during fill, concurrent eviction under load,
  memory exhaustion, and failure injection are untested.
- No exhaustive Range/slice matrix: suffix and multiple ranges, 416, changed
  ETag or total length, missing Content-Range, upstream 200 instead of 206,
  last short segment, changed content during fill, and cancellation are open.
- HEAD, conditional reuse, RFC-complete Age/freshness/warning handling,
  `Vary`, stale revalidation, request cache directives, and many HTTP caching
  semantics intentionally bypass or remain unsupported.
- The lookup is a linear scan over SLRU lists. Key construction includes
  request headers rather than sharing the configured L2 key policy. The
  logical used-byte metric does not report exact slab fragmentation; free
  bytes, pinned count, lock contention, and origin-request counters are not
  exposed. There is no external metrics endpoint.
- NGINX reload is tested with the same structure and configuration. Reloads
  changing key semantics, zone layout, limits, or slice policy have not been
  tested. Full process restart/L2 repromotion is not tested.
- No L2 purge integration, admin UI, health checks, production image, setup,
  or update integration exists here. PURGE is an L1-only primitive; access
  control beyond its secret token must be supplied by the NGINX deployment.
- The benchmark does not cover cold cache, mixed load, large transfer scaling,
  random ranges, eviction, CPU, RAM, disk I/O, context switches, or event-loop
  impact. It uses a small loopback client and has substantial run-to-run noise;
  use it only as a reproducible initial measurement, not a capacity result.
- No full installation test, platform/ABI matrix, or NGINX upstream test-suite
  run was performed.

Consequently, the code demonstrates a functioning native shared-memory L1
cache and selected L1/L2/slice paths, but does not satisfy the supplied
production acceptance criteria. Do not deploy it as production-ready until the
open items are implemented and verified.
