# NGINX shared-memory response cache module

This is a generic dynamic HTTP module for this NGINX source tree. It adds an
in-memory L1 response cache in an NGINX shared-memory zone. The configured
`proxy_cache` remains the normal persistent L2 cache; the module does not
replace or write to NGINX's file-cache internals.

## Build

Configure NGINX with the slice module and this dynamic module, then build the
module using the same source tree and build options as the target NGINX binary:

```sh
./auto/configure --with-compat --with-threads --with-http_ssl_module \
  --with-http_slice_module \
  --add-dynamic-module=contrib/ngx_http_l1_cache_module
make -j4 modules
```

The resulting module must be loaded by an ABI-compatible NGINX binary. Verify
it with `nginx -t` and real requests before deployment. Slice support is a build
requirement because the module detects slice subrequests and uses `$slice_range`.

## Configuration

```nginx
load_module modules/ngx_http_l1_cache_module.so;

events { worker_connections 1024; }
http {
    aio threads;
    l1_cache_zone l1 512m;
    l1_cache_key_id downloads-v1;
    l1_cache_max_object 8m;
    l1_cache_ttl 300;

    proxy_cache_path /var/cache/nginx levels=1:2 keys_zone=disk:64m;
    server {
        location /downloads/ {
            l1_cache on;
            l1_cache_force off;
            proxy_cache disk;
            proxy_cache_lock on;
            proxy_cache_valid 200 206 10m;
            proxy_pass http://origin;
        }
    }
}
```

`l1_cache_zone` is a hard shared-memory allocator boundary, including metadata
and allocator overhead. `l1_cache_key_id` is required whenever any location
enables L1; NGINX rejects such a configuration without a nonempty identity.
A configured zone enables L1 by default in each
location; use `l1_cache off` to disable it for a location. `aio threads` must
be enabled in the HTTP, server, or location context inherited by every enabled
location, and NGINX must be built with `--with-threads`. `l1_cache_max_object`
defaults to 8 MiB; `l1_cache_block_size` defaults to 64 KiB;
`l1_cache_protected_percent` defaults to 80; and
`l1_cache_building_percent` defaults to 10. These values, the zone size, and
the 300-second default `l1_cache_ttl` are configurable. In-progress captures
are bounded by the building percentage and published only after the complete
body is validated. Admission requires a live, successfully installed NGINX
cache entry; response capture is published only during request cleanup after
NGINX completes its cache-file update. L1 expiry is capped by both
`l1_cache_ttl` and the L2 `valid_sec`. The L1 zone is shared by all workers and
survives a compatible NGINX reload; a full process restart starts it empty.
On reload, allocator-relevant L1 settings are compared field by field in
shared memory; a changed value advances the generation, cancels in-progress
captures, and invalidates old objects before new workers use the zone.

Set `l1_cache_key_id` in `http` scope and change it when the meaning of the
configured `proxy_cache_key`, origin identity, or Slice policy changes without
changing the L1 zone layout. The value is compared byte for byte, limited to
128 bytes, and changing it invalidates L1 entries and cancels old-generation
fills on reload. Persistent NGINX L2 files remain available for promotion.
Keep the value stable for ordinary compatible reloads so READY L1 objects
survive them.

The default policy is a conservative RFC-oriented subset. It serves complete
GET 200 responses and validated native Slice 206 segments; HEAD can reuse
metadata from a complete GET object. It bypasses authorization, cookies,
conditional requests, non-Slice ranges, `Set-Cookie`, `Vary`, content encoding,
unsupported status or response headers, unknown or oversized bodies, and
incomplete or invalid slice metadata. Without force mode it also rejects
request and response cache directives that prohibit reuse. Responses without
an effective NGINX L2 validity are not admitted. This is not a complete
implementation of every RFC 9111 feature; cases without validated semantics
bypass L1.

The L1 admission check parses response `Cache-Control` directive names rather
than searching for substrings. Quoted extension values may contain commas or
text such as `no-cache` without being mistaken for a directive. Malformed
directive syntax bypasses L1 admission. For requests without force mode, any
request `Cache-Control` or `Pragma` header bypasses L1 and L2 reads. A request
`no-store` directive also prevents the native L2 cache write; other request
directives may still allow the fetched response to refresh L2. Credentialed
requests (`Authorization`, `Proxy-Authorization`, `Cookie`, or `Cookie2`) also
bypass L2 reads and writes in this conservative mode. This request policy is
enforced by the NGINX integration because `proxy_cache_bypass` alone does not
interpret these directives. `l1_cache_force on` explicitly overrides it and
fingerprints request headers to keep credential variants separate.

Successful responses to unsafe methods (2xx or 3xx) invalidate cached data;
failed responses leave existing entries usable. With NGINX's built-in cache
key, NGINX invalidates the matching L1 and L2 object. With a custom
`proxy_cache_key`, native Slice segments, or force mode, the module invalidates
the selected L1 and L2 zones because arbitrary key and variant layouts cannot
be mapped safely to one object.

The request directive `Cache-Control: only-if-cached` may use a fresh L1 or
native L2 hit. If neither cache can provide a usable hit, NGINX returns 504
without contacting the origin. Combinations with other request cache
directives are conservatively treated as unsatisfied and return 504; a forced
location deliberately permits the normal origin fallback. Asynchronous L2
file reads complete through NGINX's normal thread-AIO path before the hit or
miss decision is made.

On responses, exact `no-store`, `private`, `no-cache`, `must-revalidate`, and
`proxy-revalidate` Cache-Control directives prevent L2 storage even if
`proxy_ignore_headers Cache-Control` is configured. The revalidation directives
are conservatively excluded because native NGINX stale-serving configuration
can otherwise return them without revalidation. An unterminated quoted
Cache-Control value also prevents L2 storage. `l1_cache_force on` is the
explicit override for these protections.
For non-forced locations, explicitly ignoring Cache-Control, Expires,
X-Accel-Expires, Set-Cookie, or Vary disables L1/L2 reuse and storage in that
location. This also avoids serving an older disk entry whose response policy
cannot be checked before the cache lookup. Use `l1_cache_force on` to opt into
that override deliberately.

`l1_cache_force on` on a location overrides request and response cache
directives, including `no-cache`, `private`, and `no-store`, and uses
`l1_cache_ttl` as the corresponding L2 and L1 validity. It also permits
credentialled requests, `Set-Cookie`, and `Vary` responses. To keep these
variants separate, NGINX adds a digest of request headers to the disk-cache
key and the full length-delimited header set to the L1 key. The disk-key
fingerprint uses SHA-256; credential values are not added by this mechanism to
the persistent cache key. Enabling force mode requires NGINX to be built with
OpenSSL. L1 stores and
replays `Vary`, `Set-Cookie`, and otherwise unsupported end-to-end response
fields, preserving repeated fields in their original order within a 4 KiB
metadata limit. NGINX processes named `Vary` fields natively; `Vary: *` uses the
complete request-header fingerprint. Connection-specific and proxy-specific
fields and `X-Accel-*` control fields still bypass L1. Upstream fields named
by `Connection` are removed by NGINX before forwarding, disk-cache storage, or
L1 admission. During an active HTTP/1.1 protocol upgrade, NGINX preserves the
`Upgrade` response field for its tunnel handshake and creates the downstream
`Connection: upgrade` field itself.
Force mode removes `If-Match`, `If-None-Match`, `If-Modified-Since`, and
`If-Unmodified-Since` before cache and origin processing, and disables
NGINX's conditional response filter for that request. These four preconditions
are therefore ignored deliberately. `Range` and `If-Range` retain their
normal NGINX semantics; non-Slice ranges, unsupported encodings, incomplete
responses, unvalidated Slice data, and size-limit failures still bypass L1.
Configure force mode only for resources whose shared reuse and stored cookies
are intentional.
Other module options are
`l1_cache_zone`, `l1_cache_key_id`, `l1_cache_max_object`, `l1_cache_block_size`,
`l1_cache_protected_percent`, `l1_cache_building_percent`,
`l1_cache_purge_token`, and the per-location `l1_cache` switch.

Slice subrequests use the native `$slice_range`, require a strong ETag, and
are cached as individual segments. The module does not synthesize range
responses itself; NGINX's slice and range filters retain that responsibility.
If a Slice-enabled request receives a full `200` instead of a validated `206`,
NGINX keeps its native fallback response and the module refuses L1 admission;
the first slice key must not identify a full object.
In the default mode, non-Slice ranges and conditional requests bypass L1. The
four conditional validators listed above are ignored in force mode; `Range`
and `If-Range` are not. The L1 key is the
byte-for-byte concatenation of the actual NGINX `r->cache->keys` fields
generated by the configured `proxy_cache_key`, namespaced by the effective
NGINX proxy-cache zone. The zone namespace matters because two independent L2
zones may safely use identical key bytes for different origins or policies.
The hash is only an index and full key bytes are compared on lookup. L1 does
not invent a second URL normalization rule. Within each zone, the L2 key
therefore remains the identity authority: include scheme, host/origin, query,
and `$slice_range` as required by the deployment's L2 policy. In force mode,
the request-header variant is appended to both cache identities before the
native disk-cache lookup.

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
l1_cache_purge_token "replace-with-a-random-secret-of-at-least-32-bytes";
```

Send `X-L1-Cache-Purge` with the same secret. `PURGE /` invalidates all entries
in the selected L1 and NGINX proxy-cache zones; other URIs invalidate the
matching keys and cancel an in-progress L1 fill. L2 nodes are marked under the
native cache-zone lock so new requests cannot hit them, and NGINX's cache
manager removes their files. A fill already in progress cannot republish a
purged L2 object. Unrelated entries remain usable for targeted PURGE. Both
forms return 204. An incorrect or missing secret returns 403.

## Metrics

The module exposes `$l1_cache_requests`, `$l1_cache_hits`,
`$l1_cache_misses`, `$l1_cache_bypasses`, `$l1_cache_hit_bytes`,
`$l1_cache_evictions`, `$l1_cache_expirations`,
`$l1_cache_admission_accepted`, `$l1_cache_admission_rejected`,
`$l1_cache_allocation_failures`, `$l1_cache_used_bytes`,
`$l1_cache_building_bytes`, `$l1_cache_object_count`,
`$l1_cache_building_count`, `$l1_cache_l2_hits`, `$l1_cache_l2_misses`,
`$l1_cache_free_bytes`, `$l1_cache_pinned_count`,
`$l1_cache_lock_contention`, and `$l1_cache_origin_requests`.
`$l1_cache_bypasses` counts requests excluded by policy and error fallbacks;
the shared error-fallback marker prevents counting one request more than once.
`used_bytes` reports accounted object allocation charges, not exact slab
fragmentation or all NGINX shared-zone overhead. `free_bytes` reports the
remaining logical admission budget, `pinned_count` counts distinct objects
with active response references, and `origin_requests` counts upstream
connection attempts.
Admission counters record only responses that reach the L1 admission callback.
For a Slice response, NGINX validates `Content-Range` first; a response that
the native Slice filter rejects changes neither admission counter.
If a same-key response reaches admission after another request has already
published that object, it is counted as rejected before a duplicate body is
reserved. An active same-key capture is rejected the same way.

## Scope

This is an opt-in native NGINX L1 object cache in front of the existing
file-based proxy cache. The module uses conservative admission by default;
`l1_cache_force on` explicitly overrides the documented cache-policy checks.
This documentation does not claim complete RFC 9111 feature coverage.
