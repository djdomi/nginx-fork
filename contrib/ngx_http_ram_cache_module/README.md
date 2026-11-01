# NGINX shared-memory response cache module

This is a generic dynamic HTTP module for this NGINX source tree. It adds an
in-memory L1 response cache in an NGINX shared-memory zone. The configured
`proxy_cache` remains the normal persistent L2 cache; the module does not
replace or write to NGINX's file-cache internals.

## Build

Configure NGINX with the slice module and this dynamic module, then build the
module using the same source tree and build options as the target NGINX binary:

```sh
./auto/configure --with-compat --with-http_slice_module \
  --add-dynamic-module=contrib/ngx_http_ram_cache_module
make -j4 modules
```

The resulting module must be loaded by an ABI-compatible NGINX binary. Verify
it with `nginx -t` and real requests before deployment. Slice support is a build
requirement because the module detects slice subrequests and uses `$slice_range`.

## Configuration

```nginx
load_module modules/ngx_http_ram_cache_module.so;

events { worker_connections 1024; }
http {
    ram_cache_zone l1 512m;
    ram_cache_max_object 8m;
    ram_cache_ttl 300;

    proxy_cache_path /var/cache/nginx levels=1:2 keys_zone=disk:64m;
    server {
        location /downloads/ {
            ram_cache on;
            proxy_cache disk;
            proxy_cache_lock on;
            proxy_cache_valid 200 206 10m;
            proxy_pass http://origin;
        }
    }
}
```

`ram_cache_zone` is a hard shared-memory allocator boundary, including metadata
and allocator overhead. `ram_cache_max_object` cannot exceed 8 MiB. Up to 10%
of the zone is reserved for in-progress response capture. Captures are limited
to 64 KiB slabs and published only after the complete body is validated.
The default TTL is 300 seconds. Admission requires a live NGINX cache entry;
L1 expiry is capped by both `ram_cache_ttl` and the L2 `valid_sec`. The L1 zone
is shared by workers and survives a compatible NGINX reload; a full process
restart starts empty.

The cache is opt-in per location (`ram_cache on`). It only serves GETs that do
not carry authorization, cookies, range outside native slice handling, or
conditional headers. It bypasses responses with `Set-Cookie`, `Vary`, content
encoding, `Cache-Control`, `Expires`, unsupported status or response headers,
unknown/oversized bodies, or incomplete/invalid slice metadata. This is a
conservative subset, not a complete RFC 9111 shared-cache implementation.
Responses with unknown freshness or no configured effective NGINX cache
validity are not admitted. L1 hits include `X-RAM-Cache: HIT` and
`X-RAM-Cache-Status: L1_HIT`; misses report L2 status when NGINX exposes it.

Slice subrequests use the native `$slice_range`, require a strong ETag, and
are cached as individual segments. The module does not synthesize range
responses itself; NGINX's slice and range filters retain that responsibility.
Non-slice range and conditional requests bypass L1. The cache key contains the
scheme, normalized Host value, URI, query, slice range, and request headers
(except the consumed slice Range and purge credential). This deliberately
favours isolation over maximum key sharing and may prevent reuse across
requests that NGINX's configured L2 key considers equivalent.

## Purge

PURGE is handled by the L1 module when a shared zone is configured. Define a
random secret of at least 32 bytes in `http` scope, protect the NGINX
configuration file, and restrict network access to PURGE requests:

```nginx
ram_cache_purge_token "replace-with-a-random-secret-of-at-least-32-bytes";
```

Send `X-RAM-Cache-Purge` with the same secret. `PURGE /` clears all L1 entries;
other URIs clear the matching L1 key and return 204. An incorrect or missing
secret returns 403. This operation does not purge L2; use the existing
authorized mechanism for the configured NGINX disk cache separately.

## Metrics and tests

The module exposes `$ram_cache_requests`, `$ram_cache_hits`,
`$ram_cache_misses`, `$ram_cache_bypasses`, `$ram_cache_hit_bytes`,
`$ram_cache_evictions`, `$ram_cache_expirations`,
`$ram_cache_admission_accepted`, `$ram_cache_admission_rejected`,
`$ram_cache_allocation_failures`, `$ram_cache_used_bytes`,
`$ram_cache_building_bytes`, `$ram_cache_object_count`,
`$ram_cache_building_count`, `$ram_cache_l2_hits`, and
`$ram_cache_l2_misses`. `used_bytes` reports accounted object allocation
charges, not exact slab fragmentation or all NGINX shared-zone overhead.

Run the live integration test after building:

```sh
contrib/ngx_http_ram_cache_module/test.sh \
  "$PWD/objs/nginx" "$PWD/objs/ngx_http_ram_cache_module.so"
```

It exercises real NGINX workers, HTTP/HTTPS key separation, file-backed
responses, multi-block objects, native slice assembly and multi-slice Range,
L2 promotion, PURGE authorization and invalidation, expiry, reload, concurrent
hits, bypasses, and metrics. Tests that have not been implemented are listed
in `STATUS.md`.

Set `RAM_CACHE_WORKERS` and `RAM_CACHE_PARALLEL_CLIENTS` to change the live
test's worker and parallel-hit counts. `benchmark.sh` compares warm L2 with no
module, warm L2 with the module disabled, and warm L1 with the module enabled.
The initial results and their limits are recorded in `BENCHMARK.md`; they do
not establish a performance gain.

## Scope and readiness

This module is a functional, tested implementation in the NGINX fork, but it
has not met the supplied production-readiness acceptance criteria. The initial
benchmark is narrow and noisy; sanitizer, fault-injection, cancellation, L2
corruption, restart, the full worker/client matrix, and all RFC and slice edge
cases remain open. It does not integrate with an external admin UI or
deployment system. Review `STATUS.md` before treating it as production-ready.
