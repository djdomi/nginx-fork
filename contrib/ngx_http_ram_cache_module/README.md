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
The default TTL is 300 seconds. Admission requires a live, successfully
installed NGINX cache entry; response capture is published only during request
cleanup after NGINX completes its cache-file update. L1 expiry is capped by
both `ram_cache_ttl` and the L2 `valid_sec`. The L1 zone
is shared by workers and survives a compatible NGINX reload; a full process
restart starts empty.

The cache is opt-in per location (`ram_cache on`). It serves GETs and may
answer HEAD from an already complete GET object, preserving its representation
length and validators without a response body. It bypasses requests with
authorization, cookies, range outside native slice handling, or conditional
headers. It bypasses responses with `Set-Cookie`, `Vary`, content
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
requests that NGINX's configured L2 key considers equivalent. Every field is
length-prefixed, so request-controlled delimiters cannot merge field
boundaries; a full byte comparison determines identity.

## Squid reference

The design was compared with Squid's shared-memory `MemStore` at upstream
revision [`007c4a002f03`](https://github.com/squid-cache/squid/commit/007c4a002f03b8a2c98b54653e94537406c8a75c).
Squid's [`MemStore`](https://github.com/squid-cache/squid/blob/007c4a002f03b8a2c98b54653e94537406c8a75c/src/MemStore.cc)
separates its shared-memory store from disk, checks the publisher, response
eligibility, expected/received size, and contiguity before admission, and aborts
partial writes. Its [`StoreMap`](https://github.com/squid-cache/squid/blob/007c4a002f03b8a2c98b54653e94537406c8a75c/src/ipc/StoreMap.cc)
uses reader/writer locks and deferred freeing for entries still in use. Its
store copies data between worker-local storage and shared pages; this module
instead serves pinned shared-memory blocks through NGINX output chains.

The [Squid v7 `cache_mem` documentation](https://www.squid-cache.org/Versions/v7/cfgman/cache_mem.html)
describes an ideal budget for hot, negative-cached, and in-transit objects, and
explicitly says in-transit demand may exceed that budget. With shared-memory
caching enabled, those in-transit objects remain local to workers. The v7
[`maximum_object_size_in_memory` directive](https://www.squid-cache.org/Versions/v7/cfgman/maximum_object_size_in_memory.html)
rejects objects above its configured limit. Both directives are marked
unavailable in Squid v8, so their v7 defaults and semantics are not treated as
current universal Squid behavior. This module uses an explicit hard shared-zone
limit, a bounded capture reservation, a per-object maximum, and complete-object
publication before L1 lookup.

Squid's heap replacement code includes LFUDA and GDSF scoring based on
frequency, object size, recency, and dynamic aging. The NGINX module follows the
requested byte-accounted two-queue SLRU policy instead; the Squid source is a
reference for admission, shared-memory lifetime, and explicit replacement
tradeoffs, not a claim of algorithm or feature parity. These differences keep
the existing NGINX proxy cache as L2 and its native request path authoritative.

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
`$ram_cache_building_count`, `$ram_cache_pin_count`, `$ram_cache_l2_hits`, and
`$ram_cache_l2_misses`. `used_bytes` reports accounted object allocation
charges, not exact slab fragmentation or all NGINX shared-zone overhead.

Run the live integration test after building:

```sh
contrib/ngx_http_ram_cache_module/test.sh \
  "$PWD/objs/nginx" "$PWD/objs/ngx_http_ram_cache_module.so"
```

It exercises real NGINX workers, HTTP/HTTPS, Host, and query key separation,
HEAD from cached GET metadata, and verifies GET/HEAD L1 hits do not add origin
requests. A URI/query boundary case verifies distinct field identities even
when naive field concatenation would produce the same bytes. It covers
file-backed responses, multi-block objects, native slice
assembly and multi-slice Range,
L2 promotion, PURGE authorization and invalidation, expiry, reload and restart,
client abort recovery, serial and concurrent multi-key eviction pressure,
reload during a pinned response, worker death during both a fill and a pinned
L1 hit, with recovery,
an upstream read timeout during a partial response and recovery, concurrent
hits, bypasses, and metrics. Worker-death recovery also checks that stale
in-progress capture reservations are reclaimed and their counters return to
zero. Reclamation occurs on a later eligible response. Its stress job runs
ten 120-request rounds with distinct keys against the same 16 MiB zone. The
suite also makes the L2 cache directory unwritable and verifies that origin
responses still reach the client without becoming L1 hits. The GitHub workflow
runs the complete worker/client matrix, AddressSanitizer with leak detection,
and ThreadSanitizer.
Tests that have not been implemented are listed in `STATUS.md`.

Set `RAM_CACHE_WORKERS` and `RAM_CACHE_PARALLEL_CLIENTS` to change the live
test's worker and parallel-hit counts. `benchmark.sh` compares warm L2 with no
module, warm L2 with the module disabled, and warm L1 with the module enabled.
The initial results and their limits are recorded in `BENCHMARK.md`; they do
not establish a performance gain.

The fork's `RAM cache module` GitHub Actions workflow builds NGINX and runs the
integration suite across the worker/client matrix on this validation branch.

## Scope and readiness

This module is a functional, tested implementation in the NGINX fork, but it
has not met the supplied production-readiness acceptance criteria. The initial
benchmark is narrow and noisy and does not establish a performance gain. Open
verification and implementation work is listed in `STATUS.md`. The module
does not integrate with an external admin UI or deployment system. Do not
treat it as production-ready.
