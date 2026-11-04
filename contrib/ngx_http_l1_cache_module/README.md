# NGINX shared-memory response cache module

This is a generic dynamic HTTP module for this NGINX source tree. It adds an
in-memory L1 response cache in an NGINX shared-memory zone. The configured
`proxy_cache` remains the normal persistent L2 cache; the module does not
replace or write to NGINX's file-cache internals.

The module processes HTTP requests after NGINX terminates client TLS. It can
also cache responses from an HTTPS upstream. It does not configure transparent
traffic redirection, certificate interception, or client certificate trust;
those remain NGINX and network configuration responsibilities.

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
The module reads NGINX's private `ngx_http_cache_t`, upstream cache-status,
and proxy location-configuration fields to validate native L2 publication,
cache-key expression, and cache-zone selection. Those structures are not a
stable public ABI; the Alpine APK therefore depends on the exact matching
NGINX package version and is built from that version's `nginx-mod-dev` source.

The dynamic library registers two NGINX module entries so the L1 header filter
runs before the native Slice header filter, while the L1 body filter runs after
the Copy filter has moved eligible file-backed data into memory. The configured
proxy content handler is wrapped for Slice clone requests because NGINX clones
them at the content phase, after PRECONTENT has already run. This lets a segment
L1 hit happen before that clone reaches the native proxy cache or origin.

## Alpine APK

The package recipe derives its NGINX version from `NGINX_PKGVER` or the
installed `nginx` package. In a package build environment, pass the exact
version returned by `apk info -a nginx`:

```sh
NGINX_PKGVER=1.30.6-r0 abuild -r
sudo apk add --repository=/path/to/repository nginx-mod-l1-cache
```

The APK pins `nginx` and `nginx-mod-dev` to that package version, installs the
module under `/usr/lib/nginx/modules`, and adds an autoload file under
`/etc/nginx/modules`. Its matching `nginx-mod-http-cache-purge` dependency
provides native L2 purge support. The tested target in this repository is
Alpine x86_64 with `nginx-1.30.6-r0`.

A prebuilt package for that exact target is available at
`alpine/dist/nginx-mod-l1-cache-1.30.6-r54.apk`; its signing key is
`alpine/dist/codex-6ae8bf51.rsa.pub`. Install the public key as trusted, then
install the APK (the configured Alpine repositories must provide the matching
purge module):

```sh
sudo install -m 0644 codex-6ae8bf51.rsa.pub /etc/apk/keys/
sudo apk add ./nginx-mod-l1-cache-1.30.6-r54.apk
sudo nginx -t
```

This artifact is only compatible with the pinned Alpine x86_64 NGINX package
version above. For other NGINX package versions, build a matching APK instead.

## Configuration

```nginx
load_module modules/ngx_http_l1_cache_module.so;

events { worker_connections 1024; }
http {
    aio threads;
    l1_cache_zone l1 512m;
    l1_cache_key_id downloads-v1;
    l1_cache_l2_zone disk;
    l1_cache_key "$scheme$host$uri$is_args$args$slice_range";
    l1_cache_max_object 8m;
    l1_cache_max_tracked_requests 4096;
    l1_cache_ttl 300;

    proxy_cache_path /var/cache/nginx levels=1:2 keys_zone=disk:64m;
    server {
        location /downloads/ {
            l1_cache on;
            l1_cache_force off;
            l1_cache_l2_purge on;
            proxy_cache $l1_cache_l2_zone;
            proxy_cache_key $l1_cache_key;
            proxy_cache_bypass $l1_cache_request_bypass;
            proxy_no_cache $l1_cache_request_no_store;
            proxy_cache_lock on;
            proxy_cache_valid 200 206 10m;
            proxy_cache_purge PURGE from 127.0.0.1;
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
Before response headers, each eligible cache miss also reserves a bounded
invalidation tracker (maximum 4096 by default, configurable with
`l1_cache_max_tracked_requests`). It covers responses that L1 cannot admit.
If the tracker cannot be allocated, that request bypasses both cache tiers.
After the response stream, a background check runs after NGINX's native L2
commit and performs a deferred native PURGE only when one raced with the fill.

Set `l1_cache_key_id` in `http` scope and change it when the meaning of the
cache key, origin identity, or Slice policy changes without changing the L1
zone layout. The value is compared byte for byte, limited to 128 bytes, and
changing it invalidates L1 entries and cancels old-generation fills on reload.
Persistent NGINX L2 files remain available for promotion. Keep the value stable
for ordinary compatible reloads so READY L1 objects survive them.

Every L1-enabled location must set `l1_cache_l2_zone` and `l1_cache_key`.
Use `proxy_cache $l1_cache_l2_zone` and `proxy_cache_key $l1_cache_key` in
that same location. `$l1_cache_key` evaluates the configured cache-key
expression used by both L1 and native L2. It is evaluated in PRECONTENT,
before the proxy content handler creates `$proxy_host` and upstream variables,
so those variables are rejected in the expression. Use request-available
variables that encode the selected origin, scheme, host, URI, query, and slice
as needed; for variable-based upstreams, include the request variable that
selects the origin. L1 additionally scopes the value by
the zone name and `l1_cache_key_id`. In force mode, the variable appends a
length-delimited fingerprint of request headers so the explicit override does
not merge credential or representation variants. Include `$slice_range` in
the expression when using the native Slice module. At request time, L1 checks
that the location's effective `proxy_cache_key` is exactly `$l1_cache_key` and
that its selected `proxy_cache` zone matches `l1_cache_l2_zone`; if either
check fails, the module bypasses L1 and leaves native proxy handling intact.

The request-policy variables must be connected to the native proxy directives
as shown above. They apply request `no-cache`, `no-store`, `Pragma`, and
credential bypass decisions to L2 as well as L1. The Alpine APK depends on
Alpine's version-matched `nginx-mod-http-cache-purge`. Every L1-eligible
location must configure its same-location `proxy_cache_purge` directive;
NGINX rejects such a location without that handler. Locations that do not
match the configured native proxy-cache key bypass L1. Internal purges call the
configured native purge handler directly, while client PURGE requests still
pass through the L1 token check and the add-on's access rules. PURGE of `/`
invalidates every L1 object; the OSS L2 purge module purges the key represented
by that request.

A location with `l1_cache off` still needs same-location L2 purge support if it
uses the same L1 key and L2 zone as an enabled location. Successful unsafe
methods there must invalidate the shared entry too.

For `only-if-cached`, a valid L1 hit is served before proxy processing,
including when `proxy_pass` uses variables. For supported static upstreams,
an L1 miss may continue through NGINX's native L2 cache; a native L2 hit keeps
NGINX's normal response path, and the peer-init guard stops an L2 miss before
origin selection and converts it to 504. A variable `proxy_pass` target has
no safe static peer hook, so an L1 miss returns 504 without starting proxy
processing. Active `proxy_cache_bypass` and unsupported methods also fail
closed. The installed r24 package was verified on Stock NGINX 1.30.6: a
variable-target origin fill produced a byte-identical `L1_HIT`, an uncached
`only-if-cached` request returned 504 without an origin request, and a failed
L2 file write did not publish an L1 object.

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

The module evaluates native `proxy_cache_bypass` predicates before its L1
lookup. A true predicate bypasses L1; after a successful response, the prior
L1 object for that key is invalidated so an older L1 copy cannot hide a
refreshed native L2 entry. Such predicates must use variables available in
PRECONTENT; NGINX rejects an L1-enabled location whose bypass expression refers
to `$proxy_host`, `$proxy_port`, or `$upstream_*`. Native `proxy_no_cache`
predicates remain authoritative for whether an upstream response may be
admitted to L1, matching their native role as write controls.

Successful responses to unsafe methods (2xx or 3xx) invalidate the matching
L1 entry and schedule an internal call to the configured native L2 purge
handler; failed responses leave both cache entries usable. L2 invalidation is
mandatory for an enabled L1 location: `l1_cache_l2_purge` defaults to `on`, and
`off` is rejected during configuration. The same-location
`proxy_cache_purge PURGE ...` directive is required. The internal purge
preserves the original URI, arguments, and request headers used to compute the
cache key. Client PURGE requests remain protected by the L1 token and any
configured access rule. The unsafe request's response is still returned to
the client.

For `Cache-Control: only-if-cached`, a fresh L1 hit is served. On an L1 miss
with a supported static proxy upstream, NGINX may serve a native L2 hit; the
module blocks origin selection and returns 504 on an L2 miss. Variable
`proxy_pass` targets cannot use that static peer guard, so only an L1 hit is
served and all misses return 504 before proxy processing. Disabled L1,
active bypasses, and unsupported methods also return 504. Combinations with
other request cache directives are treated as unsatisfied and return 504 in
normal mode. Force mode may override cache-policy directives but preserves
`only-if-cached` and never contacts the origin on an unserved miss.

The module rejects L1 admission for response policies it does not support.
The native NGINX proxy cache remains the L2 policy owner. Do not set
`proxy_ignore_headers` for cache-control or representation headers in a normal
location: L1-enabled normal locations reject `proxy_ignore_headers` for
`Cache-Control`, `Expires`, `Set-Cookie`, or `Vary`, because these overrides can
make L2 ignore restrictions which L1 correctly enforces. Force locations may
configure those overrides explicitly; their behavior is an intentional policy
override. Other proxy-level cache-policy combinations remain subject to the
documented L1 bypass rules and native NGINX cache behavior.

Age is computed from the origin response delay for a live L1 fill and advanced
for subsequent L1 hits. Before NGINX writes an eligible origin response to its
native L2 cache, the module updates the matching `Age` value in the raw
upstream header buffer without changing its length. L2 hits can then combine
that corrected initial value with the native cache residence time, including
after a full NGINX restart. The cache-key value includes an Age-format marker,
so existing L2 entries written before this behavior are not reused under the
new policy. If the raw `Age` field cannot be identified and updated safely,
the response is forwarded without L1 or L2 admission. A response with no
`Age` uses `Date` for apparent age.

`l1_cache_force on` on a location overrides cache-policy request and response
directives, including `no-cache`, `private`, and `no-store`, but preserves
request semantics such as `only-if-cached` and conditional validators. It uses
the native L2 validity as the upper bound for L1 validity. Set the L2 lifetime
with NGINX's `proxy_cache_valid`; `l1_cache_ttl` can shorten, but does not
extend, that lifetime. To make native L2 ignore origin cache-policy headers in
an explicitly forced location, configure NGINX's `proxy_ignore_headers` for
the intended headers (for example `Cache-Control Expires X-Accel-Expires
Set-Cookie Vary`). Do not use those overrides in normal locations. Force mode also permits
credentialled requests, `Set-Cookie`, and `Vary` responses. To keep these
variants separate, the module appends request-header names and values in
length-delimited form to the configured key used by both L1 and L2. This
includes credential values, which NGINX writes into the cache file's key
metadata. Protect the disk-cache directory accordingly. This implementation
does not hash or redact those values. Enabling force mode requires NGINX to be
built with OpenSSL. L1 stores and
replays `Vary`, `Set-Cookie`, and otherwise unsupported end-to-end response
fields, preserving repeated fields in their original order within a 4 KiB
metadata limit. NGINX processes named `Vary` fields natively; `Vary: *` uses the
complete request-header fingerprint. Connection-specific and proxy-specific
fields and `X-Accel-*` control fields still bypass L1. Upstream fields named
by `Connection` are removed by NGINX before forwarding, disk-cache storage, or
L1 admission. During an active HTTP/1.1 protocol upgrade, NGINX preserves the
`Upgrade` response field for its tunnel handshake and creates the downstream
`Connection: upgrade` field itself.
Force mode does not remove client conditional headers. Conditional requests
bypass L1 and continue through NGINX's normal conditional-response processing,
which can return 304 or 412 as required by the request validators. `Range` and
`If-Range` retain their normal NGINX semantics; non-Slice ranges, unsupported
encodings, incomplete responses, unvalidated Slice data, and size-limit
failures still bypass L1.
Configure force mode only for resources whose shared reuse and stored cookies
are intentional.
Other module options are
`l1_cache_zone`, `l1_cache_key_id`, `l1_cache_max_object`, `l1_cache_block_size`,
`l1_cache_protected_percent`, `l1_cache_building_percent`,
`l1_cache_max_tracked_requests`, `l1_cache_purge_token`, and the per-location
`l1_cache` switch.

Slice subrequests use the native `$slice_range`, require a strong ETag, and
are cached as individual segments. The module does not synthesize range
responses itself; NGINX's slice and range filters retain that responsibility.
If a Slice-enabled request receives a full `200` instead of a validated `206`,
NGINX keeps its native fallback response and the module refuses L1 admission;
the first slice key must not identify a full object.
In the default mode, non-Slice ranges and conditional requests bypass L1. In
force mode, conditional validators are preserved and the module bypasses L1 so
NGINX handles them normally; `Range` and `If-Range` retain their normal NGINX
semantics. The configured `l1_cache_key` expression is the identity input for
both L1 and native L2, and must also be used as `proxy_cache_key`. L1 adds the
configured zone name and `l1_cache_key_id` to its own key. The module verifies
the effective proxy key expression and zone at request time; a mismatch
disables L1 for that request. Include scheme, origin, query, and `$slice_range`
as required by the cache policy. Full key bytes are compared after the
tree-hash lookup.

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

Define a random secret of at least 32 bytes in `http` scope, protect the NGINX
configuration file, and restrict network access to PURGE requests:

```nginx
l1_cache_purge_token "replace-with-a-random-secret-of-at-least-32-bytes";
```

Configure `proxy_cache_purge` as in the example above and send
`X-L1-Cache-Purge` with the same secret. The L1 module invalidates matching RAM
objects and cancels matching L1 captures, then passes the request to Alpine's
version-matched cache-purge module to invalidate L2. If a purge races an active
fill, the module returns `202 Accepted`, bypasses L1/L2 reuse for that key, and
retries the native L2 purge after the in-flight cache write completes. For a
full-zone purge, configure the exact root location with the add-on's `purge_all`
option:

```nginx
location = / {
    l1_cache on;
    proxy_cache $l1_cache_l2_zone;
    proxy_cache_key $l1_cache_key;
    proxy_cache_purge PURGE purge_all from 127.0.0.1;
    proxy_pass http://origin;
}
```

This is an administrative full-zone operation: restrict it to a trusted
network and require the L1 purge token. Other PURGE URIs use exact-key L2
purging. The add-on's response status is not used by the L1 module as proof of
L2 persistence. An incorrect or missing L1 secret returns 403 before the
add-on runs.

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
