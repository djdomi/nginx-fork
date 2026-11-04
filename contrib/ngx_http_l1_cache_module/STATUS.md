# Technischer Status des eigenständigen L1-Moduls

Stand: Arbeitsbranch des dynamischen Moduls, Alpine x86_64,
`nginx-1.30.6-r0`, musl; aktuelles Paket r53.

## Pre-search reasoning record (r22)

The next review covers the `only-if-cached` L2-miss guard and the comments in
the modified module source. The behavior is owned by the existing
`l1_cache_handler()` and `l1_cache_upstream_peer_init()` paths; native NGINX
proxy-cache lookup and response handling remain the L2 owner. The review will
search the complete module source and repository for duplicate only-if-cached
guards, then compare peer-init callback use with the target NGINX source and
the older RAM module. No new owner or test file is planned.

## Search and ownership result (r22)

The repository search found the module request-context guard and a separate
fork-only `l1_cache_request_only_if_cached` field/path in
`src/http/ngx_http_request.h` and `src/http/ngx_http_upstream.c`. The stock
Alpine NGINX 1.30.6 source has no such L1 fields. Its upstream peer-init
callback is the point reached after native cache lookup; the native keepalive
wrapper stores and calls its original peer-init callback. The module wraps
that callback and delegates to the stored callback for ordinary requests.
The older RAM module has no matching only-if-cached or peer-init guard.

For the stock dynamic module, `l1_cache_handler()` owns request policy,
`l1_cache_upstream_peer_init()` prevents origin selection after a native L2
miss, and NGINX's proxy cache remains the L2 response owner. The fork-only
request-field path is not referenced by the module and is not available in
stock NGINX; it remains outside the dynamic module's ABI dependency. The
complete module comment audit found 29 block comments; all now use the
required two-line `What:` / `Why:` form with fields at most 60 characters.

`I confirm I did not violate [AG-CODE-011]. I reasoned BEFORE searching and identified the stock-NGINX only-if-cached L2-miss guard, its request-policy owner, existing fork-only request fields, peer-init wrappers, and the older RAM module; searched BEFORE for only-if-cached and peer-init implementations across the repository, stock NGINX 1.30.6 source, and the older module; found the current module guard, fork-only fields in request/upstream code, no matching older-module implementation, and NGINX keepalive's callback wrapper; used the current module handler and peer-init guard for stock NGINX request policy while native proxy cache remains the L2 response owner; consolidated no duplicate within the module and kept the fork-only core path separate because stock NGINX lacks those fields; searched AFTER and found exactly one module peer-init guard for the stock dynamic path. I am aware that equivalent behavior MUST have exactly one canonical owner, and I have read and applied [AG-LAW-001] in full.`

## Pre-search reasoning record (r23)

The next review covers L1 identity against NGINX's configured native L2
cache. Existing owners are `l1_cache_proxy_config_matches()` and
`l1_cache_request_key()` for module-side validation and key material, plus
NGINX's proxy module for the effective `proxy_cache_key` and cache zone. The
review will search the complete repository, the target stock NGINX proxy
configuration structures and evaluation path, and the older RAM module for
equivalent key construction or validation. No parallel key builder or test
file is planned before that search is complete.

## Search and ownership result (r23)

Stock NGINX's proxy module evaluates the configured `plcf->cache_key` complex
value as the native cache key. The L1 module accepts only the exact
`proxy_cache_key $l1_cache_key` expression, checks the selected native cache
zone, evaluates the configured L1 expression once per request, and reuses the
same bytes from its request context when NGINX asks for `$l1_cache_key`. L1
adds length-delimited cache-zone and key-ID fields for its own shared index.
Force mode adds its request-header fingerprint to the shared variable used by
both layers. The older RAM branch builds a separate key from scheme, host, URI,
query, Slice range, and selected request headers; that implementation is not
used by the current module.

The r21 runtime check deliberately configured different expressions:
`l1_cache_key "$host$uri"` and `proxy_cache_key "$scheme$host$uri"`. Both
requests returned byte-identical 200 responses, with native L2 status MISS then
HIT and `X-L1-Hits: 0` on both. Separate logs showed one origin request and two
proxy requests. NGINX configuration validation passed without warnings. This
confirms that an unsupported key relationship bypasses L1 while native L2
continues to work. Tested in `/tmp/l1-key-mismatch-final.LdKZku`.

`I confirm I did not violate [AG-CODE-011]. I reasoned BEFORE searching and identified L1/L2 cache-key identity, configured proxy_cache_key evaluation, cache-zone selection, request-variant fingerprinting, and the older RAM module's separate key builder; searched BEFORE for proxy_cache_key, l1_cache_key, l1_cache_proxy_config_matches, and l1_cache_request_key across the repository, the stock NGINX 1.30.6 proxy source, and the older RAM module; found the native proxy complex-value evaluator, the module's exact-expression and zone checks, request-cached key bytes, length-delimited L1 namespace, Force fingerprinting, and the older module's independent key construction; used the native proxy cache as L2 identity owner and the existing module variable/check as the single L1 integration owner; consolidated no duplicate in the current module and retained the older branch only as an unshipped alternative; searched AFTER and found one active current-module key validation and construction path plus the native proxy's cache-key evaluator. I am aware that equivalent behavior MUST have exactly one canonical owner, and I have read and applied [AG-LAW-001] in full.`

## Pre-search reasoning record (r24)

The next review covers Force mode and HTTP conditional request semantics.
NGINX's native precondition and not-modified filters are the intended owners of
`If-Match`, `If-None-Match`, `If-Modified-Since`, and
`If-Unmodified-Since`; the L1 module owns only cache admission and response
reuse. The review will search the repository for conditional-header removal
or rewriting, inspect the module's L1-hit and Force paths, compare the stock
NGINX filter flow, and check the older RAM branch. No second conditional
parser or test file is planned.

## Search and ownership result (r24)

The repository search found no `l1_cache_force_ignore_conditionals()` or
conditional-header deletion in the current module. `l1_cache_request_allowed()`
rejects conditional requests from L1 in both normal and Force modes. Force
changes cache-policy flags but does not rewrite `r->headers_in` validators.
Force-key fingerprinting includes request headers. The module therefore
leaves conditional responses to NGINX's native not-modified filter and proxy
path; the stock filter evaluates `If-Unmodified-Since`, `If-Match`,
`If-None-Match`, and `If-Modified-Since`. The older RAM module has no matching
Force or conditional-response owner.

The existing runtime record reports Force `If-None-Match` as 304 and
`If-Match` as 412, with request variants kept separate. No source change was
needed for this review; the current code already bypasses L1 for these
requests. This conclusion is limited to the recorded cases and the inspected
native filter path.

`I confirm I did not violate [AG-CODE-011]. I reasoned BEFORE searching and identified conditional-request semantics, cache admission, Force cache-policy overrides, request-header preservation, and native NGINX precondition filters; searched BEFORE for all four conditional headers, Force/conditional paths, and conditional-header mutation across the repository, stock NGINX 1.30.6 filter/upstream source, and the older RAM module; found one module admission gate that bypasses L1 for conditionals, Force logic that changes policy bits without rewriting validators, native NGINX conditional-response handling, and no conditional owner in the older module; used the existing L1 admission gate for bypass and native NGINX filters for 304/412 semantics; consolidated no duplicate parser or response evaluator; searched AFTER and found no conditional-header removal or second conditional evaluator in the current module. I am aware that equivalent behavior MUST have exactly one canonical owner, and I have read and applied [AG-LAW-001] in full.`

## Pre-search reasoning record (r25)

The next review covers `only-if-cached` with variable `proxy_pass` targets.
The existing L1 request handler and shared-object lookup own L1 hits; the
peer-init guard owns origin suppression only where a configured static
upstream can be identified. The review will inspect every `only-if-cached`
branch, the module's exact-key and eligibility checks, stock NGINX's variable
proxy and cache-miss path, and the older RAM module. The intended behavior is
to allow a valid L1 hit and fail closed on a miss when native delegation is
not safe. No parallel lookup or test file is planned.

### r25 pre-change findings

The current handler rejects `only-if-cached` before lookup whenever
`proxy_pass` uses variables, because `l1_cache_only_if_cached_delegate()`
requires a static upstream. The normal L1 lookup itself uses the configured
`$l1_cache_key` and cache-zone checks; it does not need a static proxy target.
Stock NGINX 1.30.6 evaluates variable `proxy_pass` targets in
`ngx_http_proxy_handler()` and only reaches upstream selection after the
PRECONTENT phase. The current static-upstream peer-init guard cannot safely
guard that dynamic target. The older RAM module has no `only-if-cached`
handling to reuse.

Planned owner: `l1_cache_handler()` owns the L1 lookup and returns 504 on an
`only-if-cached` miss unless the existing native peer guard can safely
delegate. The existing peer-init hook remains the sole owner of suppressing
origin selection for supported static upstreams. Variable targets may serve a
valid L1 hit; a miss remains fail-closed without starting NGINX proxy
processing. Unsupported methods remain 504. No second lookup or proxy-key
implementation is needed.

The first real variable-target run exposed a separate prerequisite: an
origin-filled object was not published to L1. Stock NGINX 1.30.6 initializes
`ngx_http_cache_t.uniq` to zero for a new cache node, then
`ngx_http_file_cache_update()` writes the completed file's nonzero identity to
`c->node->uniq` without copying it back to `c->uniq`. The module currently
requires those fields to be equal, so its L2-commit check rejects a successful
new L2 write. The single owner is `l1_cache_update_l2_state()`; it must
distinguish a proven same-file L2 HIT from a successful current write by the
native node's committed identity and request-local `c->updated` state, while
rejecting failed replacements where the old node identity remains unchanged.

### r25 implementation and targeted runtime evidence

`l1_cache_handler()` now attempts a verified L1 lookup before returning 504
for an `only-if-cached` request with variable `proxy_pass`. A miss with no
static peer guard still returns 504 before proxy processing. Unsupported
methods and mismatched L1/L2 configuration remain fail-closed.

`l1_cache_update_l2_state()` now recognizes three native states: the same
committed file on L2 HIT, the same file after native 304 revalidation, and a
newly committed file generation after a completed native write. A write error
leaves `node->exists` false for a new object or keeps the old identity for a
replacement, so neither can publish a new L1 object.

Target: installed Alpine `nginx-1.30.6-r0`, unchanged `/usr/sbin/nginx`,
two workers, package r23. In `/tmp/l1-oic-dynamic-r23`, a variable-target
origin fill returned 200 `L1_MISS_L2_MISS`; the immediately following
`only-if-cached` request returned 200 `L1_HIT`, object count 1, hit count 1,
and a byte-identical body (SHA-256
`61bc1d0e58b3dcafea891767da07fbf1f43202b3326336b85ae5b5a3939f3b40`). An
uncached `only-if-cached` key returned 504; the origin log contained only the
initial fill. In `/tmp/l1-l2-writefail-r23`, the L2 cache directory was made
unwritable after NGINX startup. Both ordinary requests returned 200, both
were `L1_MISS_L2_MISS`, the origin log contained two requests, and NGINX
logged two write errors; the failed L2 write did not create an L1 HIT.

The r23 APK was built with `abuild -r` against Alpine `nginx-mod-dev-1.30.6-r0`,
installed over r22, and the real proxy configuration passed `nginx -t`. Its
SHA-256 is `c8ac76abd5ffc3f417aecc8c37e7cee0fe00e3147d282934c37791f8f44a62e4`.
The regular system configuration also passed `nginx -t` through the packaged
`/etc/nginx/modules/50_l1_cache.conf` autoload. NGINX emitted its nonfatal
`variables_hash` sizing warning for the default configuration.
The initial test-fixture attempts that stopped before requests because of
missing `aio threads` configuration, directory permissions, or an unknown
diagnostic variable are not counted as product passes; the final two runtime
runs above used corrected temporary configurations.

After the code edit, the repository and Stock NGINX source were searched for
all `l2_persisted`, `l2_revalidated`, cache-node identity comparisons, and
native file-cache writes. There is one L1/L2 publication verifier and no
second cache writer or lookup implementation.

`I confirm I did not violate [AG-CODE-011]. I reasoned BEFORE searching and identified that variable proxy only-if-cached requests must attempt L1 before a fail-closed miss, and that stock NGINX's new-file uniq lives on the shared cache node; searched BEFORE for all handler guards, cache eligibility, native proxy processing, the older module, and file-cache identity writes; found one L1 handler, one static peer guard, and one L2 publication verifier; reused those owners and native cache writes; after editing, searched AFTER all l2_persisted and l2_revalidated paths, cache identity comparisons, and native file-cache writes and found no duplicate cache writer or lookup. I am aware that equivalent behavior MUST have exactly one canonical owner, and I have read and applied [AG-LAW-001] in full.`

## Pre-search reasoning record (r26)

The review classified every NGINX core file changed between the PR branch
and `origin/master` at that time. The r23 module now builds, loads, and serves tested L1/L2
requests against the installed unmodified Alpine NGINX 1.30.6 package. This is
evidence that those core changes are not required by the shipped module path;
it does not by itself show whether each change is an independent correction.
The review will inspect each complete file diff, trace the changed symbols to
the module and native NGINX owners, and compare with the original change that
introduced them. No core file will be reset until each hunk is classified and
the consequence of removal is documented. The intended PR scope is the
installable module and its package, not an NGINX core fork.

### r26 complete core-diff classification

At the start of that review, the branch differed from `origin/master` in 14 core files, not 15.
`ngx_http_not_modified_filter_module.c` was changed in the original feature
commit but has no net diff at the current HEAD. The r23 module's Alpine APK
was built against the packaged unmodified NGINX 1.30.6 sources, installed with
the original binary, and exercised through real L1/L2 requests. Thus none of
the historically changed core hunks is required by the shipped dynamic module.

| Core file | Historical hunk responsibility | Classification | PR decision |
|---|---|---|---|
| `src/core/ngx_module.h` | Adds ABI signature bit for new request fields | Legacy module bridge only | Remove from module PR |
| `src/core/ngx_spinlock.c` | Removes racy non-atomic lock reads before CAS | Independent synchronization fix | Exclude; original commit remains in history |
| `src/core/ngx_thread_pool.c` | Makes thread-pool shutdown handshake atomic | Independent synchronization fix | Exclude; original commit remains in history |
| `src/event/modules/ngx_epoll_module.c` | Atomically publishes/loads notify handler | Independent synchronization fix | Exclude; original commit remains in history |
| `src/http/modules/ngx_http_proxy_module.c` | L1 custom-key flag; skips cleared request headers | First hunk module-only; second generic header handling | Remove both from this PR |
| `src/http/modules/ngx_http_slice_filter_module.c` | L1 slice callbacks/state; validates byte unit, duplicate ranges and endpoint overflow | L1 bridge plus independent Slice validation | Remove both from this PR |
| `src/http/ngx_http_cache.h` | Adds initial-age format field and native purge API | Legacy L1/L2 integration | Remove from module PR |
| `src/http/ngx_http_copy_filter_module.c` | Adds an L1-specific in-memory request flag | Legacy module bridge only | Remove from module PR |
| `src/http/ngx_http_core_module.c` | Copies legacy L1 state into subrequests; records monotonic timing | Legacy module bridge only | Remove from module PR |
| `src/http/ngx_http_file_cache.c` | Persists custom initial age, adds purged-node API/state and L1 callbacks; guards zero-length ETag copy | Mostly legacy integration; zero-length copy guard is generic | Remove from this PR; document generic hunk separately |
| `src/http/ngx_http_request.c` | Initializes a custom monotonic request timestamp | Legacy module bridge only | Remove from module PR |
| `src/http/ngx_http_request.h` | Adds callback, cache state, Slice state and memory-filter fields | Legacy module ABI | Remove from module PR |
| `src/http/ngx_http_upstream.c` | Adds L1 events, cache-key/Force integration, request policy, Age and purge handling; filters Connection-nominated headers/trailers | Cache bridge is legacy-only; nominated-header filtering is generic HTTP handling | Remove both from this PR |
| `src/os/unix/ngx_atomic.h` | Makes trylock/unlock atomic | Independent synchronization fix | Exclude; original commit remains in history |

The generic synchronization, zero-length-copy, Slice-validation, cleared-header,
and Connection-nominated-header corrections are not L1 runtime dependencies.
They are excluded from this module PR rather than silently folded into the
module's installation requirements. Their original introducing commit
`bf7813235` remains in the branch history for explicit follow-up or cherry-pick.
The module's final owners are its own request handler, filters, and shared
memory zone; NGINX's existing proxy/file cache remains the L2 owner. A source
search found no `r->l1_cache_*` accesses or `ngx_http_file_cache_purge()` use
in the current dynamic module.

`I confirm I did not violate [AG-CODE-011]. I reasoned BEFORE searching and identified the exact question as whether each current core hunk still has an owner in the installable module or is an independent fix; searched BEFORE all 14 core-file diffs, the initial feature commit, native NGINX 1.30.6 cache/proxy/slice/filter paths, and every custom request-field/API reference in the current module; found the legacy callback bridge, several independent synchronization/HTTP fixes, and no dynamic-module references to custom core fields or purge API; preserved module ownership in the dynamic module and native L2 ownership in stock NGINX; no core change was classified as required by r23. I am aware that equivalent behavior MUST have exactly one canonical owner, and I have read and applied [AG-LAW-001] in full.`

## Pre-search reasoning record (r27)

The remaining L2-publication concern is attribution of a successful native
file-cache write to the same request whose response body is being promoted.
The sole module owner is `l1_cache_update_l2_state()`; NGINX's native owner is
`ngx_http_file_cache_update()`. Before editing, I searched the module for all
L2-persistence checks, capture finalizers, temp-file use, and cache writers,
then inspected the packaged NGINX 1.30.6 source. It sets `c->updated` before
rename and assigns `node->uniq` from `fstat()` on the writer's temp-file FD;
there is no public callback exposing a per-request commit result. The previous
nonzero-generation check therefore could attribute a concurrent writer's
generation to the current capture. I retained NGINX as the sole disk writer
and chose to compare its committed node identity with this request's own temp
FD after native cache update; missing or mismatched identity must reject L1.

After the edit, the source search found one deferred capture finalizer and one
L2 identity verifier. The r24 package built and loaded on stock Alpine NGINX;
one targeted origin-fill test, one PURGE/generation test, and one L2-write-
failure test passed. The results and boundaries are recorded below. This keeps
the native file-cache writer as the single L2 owner; no parallel file format or
writer was introduced.

## Nachweise und Status

| Anforderung | Status | Nachweis / Grenze |
|---|---|---|
| Dynamischer Build ohne NGINX-Kernänderungen | PASS | r24 wurde mit `abuild -r` gegen `/usr/src/nginx-1.30.6` gebaut; NGINX-Binary bleibt das installierte Alpine-Paket. Der PR enthält keine `src/`-Änderungen. |
| Nachinstallation und Laden des Moduls | PASS | `nginx-mod-l1-cache-1.30.6-r24.apk` installiert; APK-Signaturprüfung erfolgreich; `/etc/nginx/modules/50_l1_cache.conf` lädt die `.so`; reale Proxy-Konfiguration und reguläre Systemkonfiguration bestanden `nginx -t`. Letztere meldet eine nicht fatale `variables_hash`-Warnung. |
| Echter Shared-Memory-L1-HIT | PASS | Bereits vorhandener Lauf mit mehreren Workern: zweiter Request `L1_HIT`, Body bytegleich, kein zusätzlicher Origin-Request. |
| Native L2-HIT-Promotion | PASS | Ein L2-HIT mit korrigiertem `Age` wurde nach vollständigem NGINX-Neustart zu `L1_MISS_L2_HIT` promotet; der Folgezugriff war `L1_HIT`. |
| L2-Dateigeneration bei PURGE/Neubefüllung | PASS | Einmaliger gezielter Lauf mit r24 auf unverändertem Alpine-NGINX 1.30.6-r0, zwei Workern und 8-MiB-Body: Während der langsame alte L2-HIT ausgeliefert wurde, erfolgte PURGE und ein neuer Origin-Fill. Der Folgerequest war `L1_MISS_L2_HIT` und lieferte den neuen Body, nicht den alten. Alter Body SHA-256 `b07d46f33098743aa2b2e6f67e356c57694bb37b13e431bf9181e46a6e734aae`; neuer Body SHA-256 `2daeb1f36095b44b318410b3f4e8b5d989dcc7bb023d1426c492dab0a3053e74`; Origin-Requests: 2; alle fünf HTTP-Antworten 200. Temporärer Lauf: `/tmp/l1-l2-generation-r24`, Ergebnisse: `/tmp/l1-l2-generation-r24-results`. |
| Exakte Schlüsselidentität | PASS für konfigurierte Integration | Modul verlangt exakt `proxy_cache_key $l1_cache_key` und die passende Cache-Zone. Gezielt abweichender Proxy-Key: L2 MISS/HIT, `X-L1-Hits: 0`, bytegleiche Antworten, genau ein Origin-Request; L1 wurde sicher umgangen. |
| L2-Schreibfehler | PASS | Einmaliger gezielter Lauf mit installiertem r24 auf Stock-NGINX 1.30.6-r0: Cache-Verzeichnis nach Start schreibgeschützt; beide Requests HTTP 200 `L1_MISS_L2_MISS`, identische Bodies (SHA-256 `e1d33c8b9b1c8c7cf075d1570642c7dbe6d0e0bc3e9caf7adb77dc5c2c4bf2b3`), zwei Origin-Requests und zwei native Rename-Fehler. Kein L1-HIT nach fehlendem L2-Commit. `/tmp/l1-l2-writefail-r24`. |
| Origin-MISS speichert zuerst L2 und befüllt danach L1 | PASS für getesteten Stock-Pfad | r24 auf unverändertem Alpine-NGINX 1.30.6-r0, zwei Worker: erster Request `L1_MISS_L2_MISS`, zweiter `L1_HIT`, identische Bodies (SHA-256 `2ffeb80aea49257626a26aba1c7c7987368338ed88272fd177d57ed46416778e`), genau ein Origin-Request und eine bestätigte Aufnahme. Ein ungefüllter `only-if-cached`-Schlüssel lieferte im r23-Lauf 504 ohne Origin-Request. |
| `only-if-cached` bei variablem `proxy_pass` | PASS für getesteten Pfad | r23: Variable-Ziel lieferte vorhandenes READY-Objekt als `L1_HIT`; ein L1-MISS gab 504 zurück, bevor die Proxy-Location den Origin startete. |
| `proxy_cache_bypass` / `proxy_no_cache` | PASS | r8-Lauf: benutzerdefinierter Bypass ergab `BYPASS`, invalidierte L1; Folgezugriff war `L1_MISS_L2_HIT`. `proxy_no_cache` verhinderte wiederholte L1-Aufnahme. |
| Force-Preconditions und Isolation | PASS für geprüfte Fälle | Vorhandene Belege: If-None-Match→304, If-Match→412; Cookie/Vary-Varianten getrennt. Einmalige r25-Laufzeitprüfung mit `l1_cache_force on`: If-Modified-Since gleich Last-Modified→304, If-Unmodified-Since älter als Last-Modified→412; beide Requests umgingen L1, Origin-Log zeigte die zwei Validatoranfragen. |
| Force mit `only-if-cached` | PASS | Mit installiertem r17 und Force an lieferte ein Request auf leere L1/L2 504; das Origin-Zugriffslog blieb leer. r18 ändert diesen Pfad nicht. |
| `only-if-cached` bei Proxy-Bypass oder deaktiviertem L1 | PASS | Mit installiertem r19: eine aktivierte L1-Location mit zutreffendem `proxy_cache_bypass` und eine Location mit `l1_cache off` lieferten jeweils 504; das Origin-Zugriffslog blieb bei 0 Requests. |
| `only-if-cached` bei L2-only HIT/MISS | PASS | Mit installiertem r21 wurde `/seed` in L2 gefüllt. Nach vollständigem Proxy-Neustart (L1 leer) lieferte `Cache-Control: only-if-cached` HTTP 200 mit `X-Cache: HIT`; der Body entsprach bytegenau dem Fill. Ein nicht vorhandenes `/miss` lieferte 504 mit `X-Cache: MISS`. Das Origin-Log enthielt nur den `/seed`-Fill und keinen Request für `/miss`. Zusätzlich bestand derselbe Pfad mit NGINX-Upstream-`keepalive 4`, HTTP/1.1 und leerem `Connection`-Header. Laufverzeichnisse: `/tmp/l1-r20-oic.zbfe2Q` und `/tmp/l1-r21-keepalive.VDYyTS`. |
| Force-Override von Request-`no-store` | PASS | r17: zwei identische `Cache-Control: no-store`-Requests lieferten 200; der zweite war `L1_HIT`, Origin-Anfragen insgesamt 1, Bodies bytegleich (SHA-256 `4ecef658a2d719a545a588eee0124f62ad85df41fa517f32e0aaf61c453ec97d`). Die Origin-Antwort enthielt ebenfalls `Cache-Control: no-store`, das in dieser explizit erzwungenen Konfiguration für L2 ignoriert wurde. r18 ändert diesen Pfad nicht. |
| RFC-9111 Age über persistentem L2 | PASS für geprüften Pfad | Ein Origin-`Age: 5` mit 1,5 s Antwortverzögerung wurde als `Age: 7` ausgegeben und im nativen L2-Header gespeichert. Nach Neustart lieferte L2 `Age: 16`, danach L1 `Age: 17`; alle drei Bodies hatten denselben SHA-256. Der Test lief mit installiertem APK gegen Stock-Alpine-NGINX 1.30.6-r0. |
| Age-Header kann nicht sicher geändert werden | PASS | Mit `Age: 5` und 10 s Antwortverzögerung war die korrigierte Zahl breiter als die Origin-Headerstelle. Zwei Requests gaben `Age: 16`, `BYPASS`, L2-MISS, null L1-Objekte und bytegleiche Bodies zurück; der L2-Cache blieb leer. Geprüft mit installiertem r16 auf Stock-NGINX 1.30.6-r0. |
| Gesamte verbindliche Testmatrix | OPEN | Vorhandene erfolgreiche Nachweise wurden wiederverwendet. Die vollständige Matrix für alle Range-, Fehler-, Konkurrenz-, Reload- und Speicherfälle ist für den aktuellen Paketstand nicht vollständig belegt. |

Der erste kombinierte Force-Lauf prüfte den Folgezugriff ohne denselben
`Cache-Control`-Requestheader wie beim Fill. Dadurch erzeugte die beabsichtigte
Force-Request-Fingerprinting-Logik einen anderen Schlüssel; die zwei
Origin-Anfragen waren kein Cachefehler. Der `only-if-cached`-Teil dieses Laufs
war bereits erfolgreich (504, null Origin-Anfragen). Die Force-`no-store`
Verifikation wurde anschließend mit identischen Requests gezielt erfolgreich
abgeschlossen.

## NGINX-Kernänderungen und endgültiger PR-Umfang

Die nachfolgende Tabelle dokumentiert die historische Klassifizierung der
Kernänderungen im vorherigen Arbeitsstand. Commit `1319b4c251600e56e7f39639d4660f542225bb9d`
stellt alle 14 damals geänderten `src/`-Dateien auf `origin/master` zurück.
Der aktuelle PR enthält daher keine Änderungen unter `src/` (`git diff
origin/master...HEAD -- src/` ist leer). Die unabhängigen Core-Korrekturen sind
nicht Bestandteil dieses Modul-PRs. Der installierte dynamische Modulpfad
wurde zuvor gegen Stock-Alpine-NGINX 1.30.6-r0 gebaut und ausgeführt; diese
reine Quellbereichsbereinigung änderte weder Modulquelle noch APK, daher wurden
die erfolgreichen Laufzeitnachweise nicht wiederholt.

Das Modul verwendet weiterhin private NGINX-Strukturen
(`ngx_http_cache_t`, Proxy-Loc-Conf und Upstream-Status); deshalb ist das Paket
an die geprüfte NGINX-Version und ABI gebunden.

| Datei | Einordnung für das dynamische Modul |
|---|---|
| `src/core/ngx_module.h` | Neue Signatur für Fork-Requestfelder; vom Modul nicht verwendet. |
| `src/core/ngx_spinlock.c` | Atomare Sperrkorrektur; unabhängige Kernkorrektur. |
| `src/core/ngx_thread_pool.c` | Atomare Threadpool-Synchronisation; unabhängig vom Modul. |
| `src/event/modules/ngx_epoll_module.c` | Atomarer Notify-Handler-Zugriff; unabhängig vom Modul. |
| `src/http/modules/ngx_http_proxy_module.c` | L1-Callback-Markierung ungenutzt; `hash == 0`-Behandlung ist eine separate Headerkorrektur. |
| `src/http/modules/ngx_http_slice_filter_module.c` | Zusätzliche Range-Prüfungen sind separate Slice-Korrekturen; L1-Callbacks und Requestfelder werden vom Modul nicht verwendet. |
| `src/http/ngx_http_cache.h` | Fork speichert `initial_age` im L2-Format; das dynamische Modul setzt dieses Feld nicht voraus. Das Modul bewahrt die korrigierte Origin-Age-Zahl stattdessen längengleich im rohen Upstream-Header auf, bevor NGINX die Antwort schreibt. |
| `src/http/ngx_http_copy_filter_module.c` | L1-spezifisches Zusatzflag wird vom Modul nicht gesetzt; das Modul nutzt die vorhandenen `filter_need_in_memory`-Flags. |
| `src/http/ngx_http_core_module.c` | Kopieren von Fork-L1-Requestfeldern in Subrequests wird vom dynamischen Modul nicht benötigt. |
| `src/http/ngx_http_file_cache.c` | Fork-Age-/Callback-/Purge-Erweiterungen werden vom Modul nicht aufgerufen; native Cache-Zustände werden über versionsgebundene Strukturen gelesen. |
| `src/http/ngx_http_request.c` | Monotone Fork-Zeitmessung wird vom Modul nicht verwendet. |
| `src/http/ngx_http_request.h` | L1-Callback und Requestfelder werden vom Modul nicht verwendet. |
| `src/http/ngx_http_upstream.c` | Fork-L1-Hooks und Force-Änderungen sind nicht Teil des Modulpfads; Modul nutzt PRECONTENT, Filter und native Proxy-Direktiven. Die native statische Age-Berechnung kann das Modul nicht aufrufen. |
| `src/os/unix/ngx_atomic.h` | Fork-Lock-Makros; das Modul verwendet NGINX-Slab-Synchronisation und eigene vorhandene Atomics. |

Diese Einordnung entfernt oder verwirft keine unabhängigen Änderungen aus dem Fork. Sie stellt fest, dass das APK sie nicht benötigt.

## Vergleich mit `codex/ram-cache-validation`

| Funktion | RAM-Validierungsbranch | Aktuelles L1-Modul |
|---|---|---|
| PRECONTENT plus Header-/Body-Filter | Vorhanden | Vorhanden |
| Shared-Memory-Objekte und Mehrworkerbetrieb | Vorhanden | Vorhanden, plus SLRU/Pinning/Konfigurationsgeneration |
| L2-Schlüsselgleichheit | Eigener Schlüssel, nicht automatisch identisch zu `proxy_cache_key` | Erfordert denselben `$l1_cache_key` für L1 und natives L2 und prüft Zone/Expression |
| Force / Credential-Varianten | Nicht vorhanden | Vorhanden; Conditional Requests bleiben im nativen NGINX-Pfad |
| L2-Purge | Nicht integriert | Interner PURGE-Subrequest; benötigt das versionierte Alpine-Purge-Modul |
| RFC-/Cache-Policy | Bewusst konservative Teilmenge | Erweiterte Policy, aber nicht vollständig RFC-konform; siehe offene Age- und Testmatrixpunkte |

Der ältere Ansatz ist ein technischer Beleg für Filterintegration ohne Core-Hooks, aber wegen Schlüssel- und Funktionsunterschieden kein vollständiger Ersatz.

## r24-Zuordnung des nativen L2-Commits

Stock-NGINX 1.30.6 setzt `c->updated` vor dem Dateiumbenennen und schreibt
anschließend `node->uniq` aus `fstat()` des nativen Temp-Datei-FDs. Eine bloße
Prüfung auf `c->updated` und eine neue, nicht-null Cache-Identität konnte bei
parallelen Schreibern den Commit eines anderen Requests zuordnen. r24 verschiebt
die Capture-Finalisierung hinter das native Cache-Update. Danach wird unter dem
L2-Cache-Lock `node->uniq` mit der Inode-Identität des Temp-FDs genau dieses
Requests verglichen. Fehlt der FD oder stimmt die Identität nicht, wird kein
L1-Objekt veröffentlicht. Der gezielte Origin-Fill-Test bestätigte Commit,
Aufnahme und anschließenden L1-HIT; der PURGE-/Neubefüllungslauf bestätigte,
dass eine neuere L2-Generation keinen alten Capture als L1-HIT veröffentlicht.
Der gezielte r24-L2-Schreibfehlerlauf bestätigte HTTP-Weiterleitung ohne L1-Veröffentlichung.

## Paket (r25-Nachweis)

Im Repository abgelegtes und zum r25-Stand geprüftes Artefakt:
`alpine/dist/nginx-mod-l1-cache-1.30.6-r25.apk`
(SHA-256 `8fff13a14ea8d524581e1b86c894cbc17e7f438e027a39f135d7fe10ee40c077`).
Es wurde gegen Alpine `nginx-mod-dev-1.30.6-r0` gebaut und über r24 installiert.
Der passende öffentliche Signaturschlüssel liegt daneben. Der private
Buildschlüssel wird nicht ausgeliefert. Das Paket hängt exakt von
`nginx=1.30.6-r0` und `nginx-mod-http-cache-purge=1.30.6-r0` ab.
Das r25-Paket wurde über r24 installiert; die reale Konfiguration und der System-Autoload
bestanden `nginx -t`. `apk verify` bestätigte die Paketintegrität.

## Gesamtstatus

**Nicht vollständig abgenommen.** Das Modul ist nachinstallierbar und die
geprüften L1/L2- und Age-Pfade funktionieren im geprüften Alpine-System. Die
vollständige verbindliche Abnahmematrix bleibt offen.


## r25: Slice-L1-Integration gegen Stock-NGINX

| Prüfschritt | Ergebnis | Nachweis |
|---|---|---|
| Dynamischer Build mit getrennten Filtermodulen | PASS | r25 wurde mit zwei Modulregistrierungen aus derselben dynamischen Bibliothek gebaut. Die Headerregistrierung steht vor dem Slice-Filter; die Bodyregistrierung steht hinter dem Copy-Filter. Die installierte `.so` exportiert `ngx_http_l1_cache_module` und `ngx_http_l1_cache_body_module`. |
| Slice-Subrequest-Lookup | PASS | NGINX 1.30.6 klont Slice-Anfragen samt Content-Handler und aktuellem Phase-Index. Der Wrapper im konfigurierten Proxy-Content-Handler führt deshalb den L1-Lookup für den Klon aus, bevor der native Proxy-Handler startet. Die native Slice-Validierung bleibt Owner der zusammengesetzten HTTP-Antwort. |
| Installation und Modulstart | PASS | r25 APK installiert über r24; APK-Signaturprüfung `apk verify ...r25.apk: OK`. Build mit `nginx-mod-dev-1.30.6-r0`; Laufzeitabhängigkeiten `nginx=1.30.6-r0` und `nginx-mod-http-cache-purge=1.30.6-r0`. System-`nginx -t` und temporäre reale Konfiguration mit zwei Workern erfolgreich. |
| Slice-Kaltlauf und Aufnahme | PASS | Unverändertes Alpine-NGINX 1.30.6-r0, zwei Worker, Slice 16 KiB, Datei 262144 Byte. Kaltlauf war bytegleich zur Quelldatei (SHA-256 `30e6369451b7999d37b91cba152ddf6df9f0a0eaf14c72a6e344f23b47c993a8`); Stats: 16 L1-Objekte, 16 angenommene Segmente, 0 Ablehnungen, 269888 belegte Bytes. |
| Slice-Promotion aus persistentem L2 | PASS | Nach Origin-Fill und vollständigem NGINX-Neustart (L1 leer) lieferte der nächste GET `L1_MISS_L2_HIT`, befüllte 16 L1-Segmente und blieb bytegleich. Der Folge-GET bei gesperrtem L2 war `L1_HIT`; Origin-Log blieb nach dem ursprünglichen 16-Segment-Fill unverändert bei 16. |
| NGINX-Reload und Cache-Key-Generation | PASS für geprüfte Änderungen | Unveränderter Reload erhielt die 16 L1-Segmente: bei gesperrtem L2 `L1_HIT`. Danach änderte `l1_cache_key_id` und löste einen kompatiblen Reload aus: alter L1-Eintrag wurde nicht getroffen, Antwort kam als `L1_MISS_L2_HIT` aus dem erhaltenen L2. Alle drei Bodies bytegleich; Origin-Log blieb bei 16. |
| Warmer vollständiger GET ohne L2-Zugriff | PASS | Nach dem Kaltlauf wurde das L2-Verzeichnis für Worker unlesbar gemacht. Vollständiger GET lieferte HTTP 200, `L1_HIT` und denselben SHA-256; kein zusätzlicher Origin-Request. |
| Nicht ausgerichtete Range aus Slice-L1 | PASS | Bei weiterhin gesperrtem L2 lieferte `Range: bytes=10000-50000` HTTP 206, `Content-Range: bytes 10000-50000/262144`, 40001 bytegleiche Daten und `L1_HIT`. Origin-Log blieb bei 16 Einträgen vor und nach beiden Warmrequests. |
| Letztes verkürztes Slice | PASS | Einmaliger r25-Lauf mit 262145 Byte und 16 KiB-Slices erzeugte 17 READY-Segmente. Kalt- und Warm-GET waren bytegleich. Bei gesperrtem L2 lieferte `Range: bytes=262143-262144` HTTP 206, korrekte `Content-Range: bytes 262143-262144/262145`, zwei bytegleiche Bytes und `L1_HIT`; Origin blieb bei 17 Requests. |
| Gezieltes PURGE für L1 und L2 | PASS für geprüften Schlüssel | Mit installierten r25 und `nginx-mod-http-cache-purge-1.30.6-r0`: Fill→L1-HIT→autorisierter `PURGE /object.bin`→Folgeabruf `L1_MISS_L2_MISS`; alle Bodies bytegleich, Origin-Requests 2. |
| Full-Zone-PURGE für L1 und L2 | PASS für geprüften Root-Pfad | Mit `proxy_cache_purge PURGE purge_all from 127.0.0.1`: Fill→L1-HIT→autorisierter `PURGE /`→Folgeabruf `L1_MISS_L2_MISS`; Origin-Requests 2, gleiche Body-Prüfsumme vor und nach Purge. |
| Force-Conditional `If-Modified-Since` | PASS | Mit Force aktiviert lieferte ein zum Last-Modified passender Validator HTTP 304; die Anfrage wurde nicht aus L1 beantwortet. |
| Force-Conditional `If-Unmodified-Since` | PASS | Mit Force aktiviert lieferte ein älterer Validator HTTP 412; NGINX erhielt die Client-Precondition unverändert. |
| Vollständige Abnahme | OPEN | Die Slice-L1-Lücke ist behoben und die gezielte Paketprüfung bestanden. Andere offene Punkte der Gesamtmatrix (vollständige RFC-Matrix sowie weitere Invalidierungs-, Reload-, Konkurrenz- und Speicherfehlerfälle) bleiben offen; daher keine Produktionsfreigabe. |

Der zuvor fehlgeschlagene Slice-L1-Pfad hatte zwei Ursachen: NGINX-Slice-Klone
erreichen PRECONTENT nicht erneut, und ein einzelner Filter kann nicht zugleich
vor Slice-Header-Transformation und nach dem Copy-Body-Filter liegen. r25
behebt dies mit einem Proxy-Content-Handler-Wrapper und zwei geordneten
Modulregistrierungen. Die Diagnoseausgaben wurden vor dem APK-Build entfernt;
die obigen Laufzeitnachweise stammen aus dem installierten r25-Artefakt.

## Offener Konkurrenzfehler: 100 gleichzeitige Slice-Downloads

Ein einmaliger Lauf mit 100 parallelen GETs desselben 262144-Byte-Objekts,
16-KiB-Slices, zwei Workern und `proxy_cache_lock on` schlug mit installiertem
r25 fehl. Alle 100 Requests erhielten HTTP 200, aber nur 17 Bodies waren
vollständig und bytegleich; die übrigen 83 endeten an unterschiedlichen
Positionen zwischen 163384 und 245760 Bytes. Der Origin protokollierte die 16
erwarteten Slice-Requests. Damit ist die konkurrierende Slice-Auslieferung
nicht abgenommen.

Ein versuchsweiser r26-Fix, der den nativen Writer nach gepufferten L1-Hits
erneut aufrief, war ebenfalls fehlerhaft: Nur 1 von 100 Bodies war vollständig;
99 Clients warteten bis zum 30-Sekunden-Timeout, nachdem sie 131072 oder
147456 von 262144 Bytes erhalten hatten. Diese Änderung und das r26-Artefakt
wurden verworfen; Quellcode, installiertes Paket und ausgeliefertes Artefakt
stehen wieder auf r25. Der Lauf ist kein Nachweis für die Produktversion.

| Prüfschritt | Status | Befund |
|---|---|---|
| 100 parallele identische Slice-GETs auf r25 | FAIL | 17/100 vollständige Bodies; 83 abgeschnittene Präfixe; Origin 16 Slice-Requests. |
| Versuchter r26-Writer-Fix | FAIL, verworfen | 1/100 vollständig; 99 Client-Timeouts nach Teilbody. |
| r36-Ergebnis nachträglich geprüfte Worker-Sicherheit | OPEN | Die 100 Client-Ergebnisdateien belegen Transport- und Body-Erfolg. Ein zusammengeführter Fehlerlog enthält zeitgleich Signal-11-Abstürze, die sich ohne getrennte Logs oder Core-Dump nicht sicher genau diesem Lauf zuordnen lassen. r36 gilt daher nicht als Nachweis absturzfreier Konkurrenz. |
| Breitere Konkurrenzabnahme | OPEN | Der erfolgreiche Lauf deckt genau die dokumentierte Konfiguration ab; weitere parallele Muster und Fehlerpfade sind nicht damit abgenommen. Bereits erfolgreiche Läufe werden nicht wiederholt. |

## r36: gezielte Konkurrenzkorrektur für Slice-L1-Hits

Der Slice-Subrequest-Wrapper ruft den regulären Requestpfad jetzt ausdrücklich
mit dem Kennzeichen auf, dass NGINX den Content-Handler-Ergebniscode selbst
finalisiert. Dadurch finalisiert ein direkter L1-Hit den Slice-Subrequest nicht
zusätzlich selbst. Für L1-Hits wird der wartende Slice-Elternrequest nach
gepufferter Ausgabe über seinen bisherigen Writer fortgesetzt. Der Proxy-MISS-
Pfad bleibt beim nativen Handler und erhält keinen L1-Resume-Wrapper.

| Prüfschritt | Ergebnis | Nachweis |
|---|---|---|
| r36 APK-Build und Installation | PASS | Alpine-Paket `nginx-mod-l1-cache-1.30.6-r36` gebaut und über r35 installiert. Das signierte Paket liegt unter `alpine/dist/nginx-mod-l1-cache-1.30.6-r36.apk`; `apk verify` meldete `OK`. SHA-256 `13a80a1db9eb64f1e60026cbda82ad237e13ced9deab68ffd5fc411c72fd3062`; Abhängigkeiten `nginx=1.30.6-r0`, `nginx-mod-http-cache-purge=1.30.6-r0`, musl x86_64. |
| Konfiguration des unveränderten NGINX | PASS | `sudo nginx -t -p /tmp/l1-concurrency-r27/ -c nginx.conf` erfolgreich. Ein unprivilegierter `nginx -t` scheiterte zuvor an Zugriffsrechten auf NGINX-Logs und Runtime-Verzeichnisse; das war kein Konfigurationsfehler. |
| 100 parallele Slice-GETs | Body-Prüfung PASS; Prozesssicherheit OPEN | 2 Worker, 100 parallele Clients, Slice 16 KiB, Objekt 262144 Byte. Die Ergebnisdateien waren vollständig und bytegleich. Ein zeitlich überlappender, zusammengeführter Fehlerlog enthält Worker-SIGSEGVs; ihre sichere Zuordnung zu genau diesem Lauf fehlt. |
| Origin-Verkehr im selben Lauf | EINGESCHRÄNKT | Während des Testintervalls kamen zwei Origin-Einträge hinzu. Der Lauf belegt deshalb keine Aussage, dass alle Requests originfrei bedient wurden; die Bytekorrektheit und vollständige Antwort aller Clients sind jedoch nachgewiesen. |
| Body-Korrektheit im früheren Lauf | PASS als Body-Befund; Prozesssicherheit OPEN | r36 lieferte 100/100 vollständige Bodies. Ein zeitlich überlappender Sammel-Fehlerlog enthält SIGSEGVs, lässt sich aber nicht sicher diesem Lauf zuordnen. r44 ist der aktuelle gezielte Absturzfreiheitsnachweis; eine vollständige Konkurrenz- oder Dauerabnahme fehlt. |

Die Korrektur und dieser einzelne Lauf ersetzen keine übrigen offenen
Abnahmepunkte. Eine erneute Ausführung dieses bestandenen 100-Client-Laufs ist
nicht vorgesehen, solange der betreffende Codepfad unverändert bleibt.

## Unsafe-Methoden invalidieren L1 und L2

L2-Invalidierung ist für aktivierte L1-Proxy-Locations verpflichtend. Die
Konfiguration lehnt `l1_cache_l2_purge off` ab und prüft nach dem NGINX-
Konfigurationsmerge, dass für jede tatsächlich L1-fähige Location der native
`proxy_cache_purge`-Handler vorhanden ist. Automatisch aktivierte Locations
ohne passende Proxy-Cache-Konfiguration werden dabei nicht fälschlich als
L1-fähig behandelt. Interne Invalidierungen rufen den nativen Purge-Handler
direkt auf; öffentliche PURGE-Anfragen durchlaufen weiterhin die vorhandene
Token- und Zugriffskontrolle.

| Prüfschritt | Ergebnis | Nachweis |
|---|---|---|
| r41 Build und NGINX-Konfigurationsprüfung | PASS | `NGINX_MOD_PKGREL=41 abuild -r`; gebaut gegen Alpine `nginx-mod-dev` für NGINX 1.30.6-r0. `make modules`, Modul-Laden und `nginx -t` im APK-Build erfolgreich. |
| Installation auf unverändertem Alpine-NGINX | PASS | r41 über r36 installiert; installiertes Binary meldet `nginx/1.30.6`, System-`nginx -t` erfolgreich. `apk verify` für `alpine/dist/nginx-mod-l1-cache-1.30.6-r41.apk` meldet `OK`; SHA-256 `7c451de954d3e52feae3e190199e0f7ff3426b749838bc2171b3d423329d9fb8`. |
| Erfolgreicher Unsafe-Methodenpfad | PASS für geprüften POST | Einmaliger Lauf mit zwei Workern auf Stock-NGINX: erster GET `X-L2: MISS`, zweiter GET `X-L1-Cache: HIT`; POST lieferte 204; folgender GET lieferte erneut `X-L2: MISS`. Origin-Log enthielt genau `GET /object`, `POST /object`, `GET /object`. Alle GET-Bodies bytegleich, SHA-256 `a39c3689fae129abde761eb87db3cbcc8a5207eefd0c8eaa814bfb9138b9ae22`. Laufverzeichnis `/tmp/l1-r41-unsafe-purge-20261104a`. |
| L2-Invalidierung bei erfolgreichem Unsafe-Response | PASS für geprüften Pfad | Der nachfolgende GET erreichte den Origin erneut und war kein L1- oder L2-HIT; der POST hatte somit sowohl den L1-Eintrag als auch die native L2-Datei invalidiert. |
| Vollständige RFC-/L1-Abnahme | OPEN | Dieser gezielte Nachweis schließt nur die geprüfte erfolgreiche POST-Invalidierung. Weitere Fälle der Gesamtmatrix bleiben offen. |

## RFC-9111 Age: ungültige Feldwerte umgehen L1

Die bisherige Age-Behandlung hatte drei getrennte Grammatikprüfungen: der
Aufnahmepfad akzeptierte ein Komma nach der Zahl, der Persistenzpfad prüfte
Ziffern selbst, und der L2-HIT-Pfad nutzte `ngx_atotm()` und setzte ungültige
Werte auf null. Stock-NGINX 1.30.6 verwendet Age nicht für seine native
Freshness-Berechnung; seine Proxy-Cache-Frische wird aus `Cache-Control` und
`Expires` bestimmt. Deshalb ist eine konservative L1-Prüfung erforderlich.

Der Modulcode verwendet jetzt `l1_cache_parse_age()` als einzigen Age-Parser
für Origin-Aufnahmen, L2-HIT-Altersberechnung und die Persistenzprüfung. Er
akzeptiert nur eine nichtnegative Dezimalzahl mit optionalem äußeren OWS,
sättigt Überläufe und lehnt zusätzliche Werte oder Feldzeilen ab. Bei
fehlerhaftem Age bleibt die reguläre NGINX-Antwort verfügbar, aber L1 wird
umgangen.

| Prüfschritt | Ergebnis | Nachweis |
|---|---|---|
| r42 Alpine-Paket | PASS | `NGINX_MOD_PKGREL=42 abuild -r` erfolgreich; Build und `nginx -t` gegen Alpine-NGINX 1.30.6-r0. Installiert auf unverändertem NGINX 1.30.6. Signaturprüfung `apk verify` meldet `OK`; SHA-256 `09176c82a5e9aa6beb9ea2230b07eb648e284141dfa6b7e9d04ecebc2a6ef258`. |
| Kombinierter ungültiger Age-Wert | PASS | `Age: 5, 999`: erste Antwort L2-MISS; zweite L2-HIT; beide Responses behielten den ursprünglichen Age-Wert und keinen L1-HIT; Bodies bytegleich. |
| Doppelte Age-Feldzeilen | PASS | Zwei `Age`-Header (`5` und `999`): erste Antwort L2-MISS; zweite L2-HIT; beide Responses behielten beide Felder und keinen L1-HIT; Bodies bytegleich. |
| Laufzeitumgebung | PASS | Stock-NGINX 1.30.6, zwei konfigurierte Worker, temporärer Test `/tmp/l1-r42-age-strict-20261104a`; kein dauerhafter Testcode hinzugefügt. |
| Vollständige Age-/RFC-Abnahme | OPEN | Der Nachweis deckt die zwei fehlerhaften Formen ab. Die übrigen RFC-9111-Semantiken bleiben separat offen; erfolgreiche Age-Altersläufe werden nicht wiederholt. |

## Unsafe-Methoden-Invalidierung bei deaktiviertem L1

Der Request-Handler prüfte zuvor die L1-Aktivierung und die normale
L1-Proxy-Konfiguration, bevor er Unsafe-Methoden für Invalidierung vormerkte.
Ein `l1_cache off`-Pfad konnte deshalb einen identischen Shared-Cache-Key aus
einer anderen Location im RAM und auf Disk stehen lassen. Die bestehende
Identitätsprüfung ist jetzt von der Request-Bypass-Prüfung getrennt. Für jede
Location mit passendem `$l1_cache_key`, L2-Zone und Cache-Purge-Handler wird
die Invalidierung auch dann vorbereitet, wenn L1 dort ausgeschaltet oder der
Request regulär gebypasst ist. Die L2-Purge-Option ist für solche gemeinsam
genutzten Keys zwingend. GET, HEAD, OPTIONS und TRACE lösen keine Unsafe-
Invalidierung aus.

| Prüfschritt | Ergebnis | Nachweis |
|---|---|---|
| r43 Alpine-Paket | PASS | `NGINX_MOD_PKGREL=43 abuild -r` erfolgreich; Paket gegen Alpine-NGINX 1.30.6-r0 gebaut, installiert und mit `apk verify` bestätigt. SHA-256 `3a3ac2e9d0ce1c3bb7d6c8d812abf3723231af4fd51863770b14c328275a792a`. |
| POST aus einer `l1_cache off`-Location | PASS für geprüfte Konfiguration | GET in aktivierter Location: L2-MISS; zweiter GET: L1-HIT; POST über getrennte `l1_cache off`-Location mit identischem L1/L2-Key: 204; Folge-GET: L2-MISS. Origin-Log exakt GET/POST/GET; GET-Bodies bytegleich. Stock-NGINX 1.30.6, temporärer Lauf `/tmp/l1-r43-disabled-location-invalidation-20261104a`. |
| Ablehnung von `l1_cache_l2_purge off` für gemeinsamen Key | PASS | Einmaliger negativer `nginx -t`-Lauf mit `l1_cache_l2_purge off` in der deaktivierten Location wurde wie erwartet abgelehnt: `cannot be disabled for a shared cache key`. |
| Vollständige Unsafe-Methoden-/Invalidierungsabnahme | OPEN | Geprüft wurden erfolgreiche POST-Antworten in aktivierter und deaktivierter Location. Andere Statuscodes, Methoden und Race-Konstellationen bleiben unbewertet. Bereits bestandene Läufe wurden nicht wiederholt. |

## r44: Slice-Resume nach nativer Request-Finalisierung absichern

Die rückblickende Prüfung fand SIGSEGVs in einem zusammengeführten
Fehlerlog-Zeitfenster, das mit der r36-Ergebnisdatei zeitlich überlappt.
Die Logs lassen sich keinem einzelnen Lauf sicher zuordnen; ein Core-Dump
liegt nicht vor. Der damalige Laufzeit-Stack ist unbekannt. Die Codeprüfung
zeigte im L1-Slice-Resume-Handler,
dass der native NGINX-Writer synchron finalisieren und den Request-Pool
freigeben kann; der alte Handler las anschließend noch `r` und den Modulkontext.
Der Handler stellt den Resume-Wrapper nun vor dem Writer-Aufruf wieder her und
kehrt direkt danach zurück. Damit verwendet er den Request nicht erneut, falls
der Writer ihn freigegeben hat. Zuständigkeit bleibt eindeutig: NGINX besitzt
Writer und Request-Finalisierung; das L1-Modul besitzt nur den Slice-Resume-
Wrapper.

| Prüfschritt | Ergebnis | Nachweis |
|---|---|---|
| r44 APK-Build | PASS | `NGINX_MOD_PKGREL=44 abuild -r`; Modul gegen Alpine `nginx-mod-dev` für Stock-NGINX 1.30.6-r0 gebaut; integrierter Modul-Ladetest und `nginx -t` erfolgreich. |
| APK-Installation und Verifikation | PASS | r44 über r43 installiert; `apk verify` meldet `OK`. SHA-256 `01200a254b77487bd4118d3e849142224d650c60dd60ac251b763d82ec392a41`. |
| Gezielte Slice-Konkurrenzprüfung | PASS für diesen Lauf | Installiertes r44 auf unverändertem Alpine-NGINX 1.30.6, zwei Worker, 100 parallele Clients, 16-KiB-Slices, 262144-Byte-Objekt. Alle 100 Bodies bytegleich; SHA-256 `30e6369451b7999d37b91cba152ddf6df9f0a0eaf14c72a6e344f23b47c993a8`. Access-Log: 1600 `L1_HIT`-Slice-Antworten. |
| HTTPS nach NGINX-TLS-Terminierung | PASS für diesen Lauf | Stock-Alpine-NGINX 1.30.6, zwei Worker, temporäres Zertifikat. HTTPS-Fill `L1_MISS_L2_MISS`, Folge-GET `L1_HIT`; beide Bodies bytegleich (SHA-256 `30e6369451b7999d37b91cba152ddf6df9f0a0eaf14c72a6e344f23b47c993a8`). Origin blieb bei 16 Slice-Requests. Belegt TLS-Terminierung in NGINX, nicht eine separate transparente Interception-Konfiguration. Laufverzeichnis `/tmp/l1-tls-r44-8B0q4h`. |
| HTTPS-Client plus HTTPS-Origin | PASS für diesen Lauf | Stock-Alpine-NGINX 1.30.6, zwei Worker, temporäre Zertifikate an beiden TLS-Enden. Erster Request `L1_MISS_L2_MISS`, Folge-GET `L1_HIT`; beide Bodies bytegleich (SHA-256 `30e6369451b7999d37b91cba152ddf6df9f0a0eaf14c72a6e344f23b47c993a8`). Origin blieb nach dem Fill bei 16 Slice-Requests; beide Worker beendeten sich regulär mit Exit-Code 0. Belegt TLS-Terminierung und TLS-Upstream, nicht transparente Netzwerkumleitung oder Zertifikatsvertrauen für echte Clients. Laufverzeichnis `/tmp/l1-tls-intercept-r44-qBVpAA`. |
| Worker-Abstürze im gezielten Lauf | PASS für diesen Lauf | Fehlerlog enthält keinen SIGSEGV oder Signal-11-Abbruch; beide Worker beendeten sich beim regulären NGINX-Shutdown mit Exit-Code 0. |
| Suffix-Range `bytes=-100` | PASS für diesen Lauf | HTTP 206, `Content-Range: bytes 262044-262143/262144`; 100 Bytes bytegleich zum Ende der Quelldatei. |
| `If-Range` mit aktuellem ETag | PASS für diesen Lauf | HTTP 206, `Content-Range: bytes 0-10/262144`; 11 Bytes bytegleich. L1 meldete `BYPASS`; native NGINX-Verarbeitung blieb maßgeblich. |
| `If-Range` mit veraltetem Validator | PASS für diesen Lauf | HTTP 200 und kompletter Body bytegleich zur 262144-Byte-Quelle; L1 meldete `BYPASS`. |
| Nicht erfüllbare Range `bytes=262144-` | PASS für diesen Lauf | HTTP 416 mit `Content-Range: bytes */262144`. |
| Zwei nicht überlappende Ranges | PASS als nativer Fallback | Anfrage `bytes=0-10,32760-32780` erhielt HTTP 200; kompletter Body war bytegleich zur Quelle und L1 lieferte keinen Teilbody. RFC 9110 §14.2 erlaubt das Ignorieren einer `Range`-Anfrage; die zunächst erwartete 206-Antwort war keine normative Pflicht. |
| Aussage zu den zeitgleichen SIGSEGVs | OPEN | Der korrigierte Lauf besteht den gezielten Fall, ist aber kein Stacktrace und beweist nicht rückwirkend, dass der zeitgleiche SIGSEGV aus genau diesem Testlauf oder derselben Ursache stammt. Breitere Konkurrenz- und Langzeitabnahme bleibt offen. |

Das APK liegt unter `alpine/dist/nginx-mod-l1-cache-1.30.6-r44.apk`. Der Testlauf
verwendete temporäre Dateien unter `/tmp/l1-slice-fix-XbkDGt`; es wurden keine
dauerhaften Testdateien hinzugefügt.

## Pre-search reasoning record (r45)

Der einmalige Lauf `/tmp/l1-purge-active-fill-r44-29GuM6` zeigte, dass ein
autorisierter PURGE während eines noch laufenden Origin-Fills vom nativen
Purge-Handler mit 412 beantwortet wird. Nach Abschluss des alten Fills traf ein
Folge-GET dennoch L2 und lieferte die alte Repräsentation. Der L1-Capture wird
zwar abgebrochen, aber das native L2-Objekt kann nach dem PURGE noch
veröffentlicht werden. Als maßgeblicher L2-Owner bleibt NGINX samt dem
installierten Purge-Modul erhalten; das L1-Modul soll aktive Captures markieren,
nach dem nativen Cache-Commit einen weiteren nativen PURGE einplanen und
währenddessen L1/L2-Wiederverwendung für denselben Schlüssel umgehen.

Vor Änderungen wurden `l1_cache_purge_request()`,
`l1_cache_invalidate()`, `l1_cache_finalize_capture()`,
`l1_cache_body_filter()`, `l1_cache_start_l2_purge()` und die native
`ngx_http_upstream_process_request()`-Reihenfolge durchsucht. Der alte
`ngx_http_ram_cache_module`-Ansatz enthält keine entsprechende Funktion.
NGINX aktualisiert den nativen Cache in `ngx_http_upstream_process_request()`;
ein Hintergrund-Subrequest, der im letzten Body-Filter-Callback eingereiht
wird, läuft erst nach Abschluss des aktuellen Event-Handlers. Die Änderung
erweitert daher den bestehenden PURGE-Owner, statt einen eigenen L2-Writer oder
Cache-Datei-Zugriff einzuführen. Der einmalige Fehlerlauf wurde durch
Verzeichnisrechte des Testaufbaus nicht verursacht: nach Korrektur der Rechte
bestätigte ein separater Lauf den 412-/L2-HIT-Befund.

## r49–r51: PURGE während eines nicht in L1 aufnehmbaren Fills

Die erste Race-Korrektur verfolgte nur Responses, für die nach den Headern ein
L1-Capture angelegt wurde. Damit fehlte eine PURGE-Erkennung bei großen,
L1-unfähigen oder anderweitig nicht aufnehmbaren Antworten. Das Modul legt nun
beim L1-Miss vor der Origin-/L2-Antwort einen kleinen Shared-Memory-Tracker an.
Ein erfolgreicher Body-Capture übernimmt diesen Datensatz; andernfalls bleibt
er bis zum Stream-Ende bestehen und kann eine native L2-PURGE-Nachbearbeitung
auslösen. Die Anzahl gleichzeitiger Tracker ist auf 4096 begrenzt. Bei
Kapazitäts- oder Allokationsfehlern werden L1- und L2-Cache für diese Anfrage
umgangen, damit kein nicht überwachter Fill nach einem PURGE gespeichert wird.
Eine purge_generation schützt außerdem gegen Requests, die ihren Kontext vor
PURGE angelegt, den Tracker aber erst danach versucht haben anzulegen.

Bei einem PURGE mit aktivem Fill antwortet der Modulpfad mit HTTP 202 und
startet die native Cache-PURGE nach Abschluss des Response-Streams. NGINX-
Cache-Dateien bleiben vollständig im Besitz des nativen Proxy- und Purge-Moduls.

| Prüfschritt | Ergebnis | Nachweis |
|---|---|---|
| r49 dynamisches APK | PASS | Gebaut und auf Alpine-NGINX 1.30.6-r0 installiert; unverändertes System-NGINX `nginx -t` erfolgreich. APK-SHA-256 `b4a809f47c006691f5d365bc88c5bce7d8923e0abe798e3b496756e1de3ecebc`. |
| Aktiver L1-fähiger Fill | PASS für diesen Lauf | Stock-NGINX 1.30.6, zwei Worker, PURGE bei laufendem 512-KiB-Fill: HTTP 202; gleichzeitige Anfrage `BYPASS`; danach neuer Wert wurde als `L1_MISS_L2_MISS` und anschließend `L1_HIT` ausgeliefert. SHA-256-Werte und Ablauf des r49-Laufs sind im vorherigen Arbeitsprotokoll festgehalten; temporär `/tmp/l1-purge-active-fill-r49-GaOXxk`. |
| r50 Build und Modul-Ladetest | PASS | `NGINX_MOD_PKGREL=50 abuild -r` erfolgreich, dynamisches Modul gebaut und integrierter `nginx -t` erfolgreich. SHA-256 `959088ae21a18ef8d689bd8f054775a942b5230bceb0ed88ce8eb42ee0235c36`. |
| PURGE mit 4-MiB-Antwort, L1-Maximum 1 MiB | PASS für diesen Lauf | Stock-NGINX 1.30.6, L1 bewusst nicht aufnahmefähig; bei 540041 bereits ausgelieferten Bytes kam PURGE mit HTTP 202 zurück. Eine Anfrage während des offenen PURGE wurde `BYPASS` und lieferte den neuen Body. Nach Ende des Fills war der nächste gespeicherte Body bytegleich zum neuen Wert; der Folgezugriff war `L1_MISS_L2_HIT` mit demselben SHA-256 `238689effabcdf972b61cbf9bebf0e459dc073db16a2c6895ec615426603aa59`. Temporär `/tmp/l1-purge-unadmitted-r50-1`; Access-Log bestätigt Frontend MISS, BYPASS, MISS, danach HIT. |
| r51 gezielter HTTP-Cache-/Ladetest | PASS | Einbezug der `only-if-cached` Miss-Weiterleitung in die Vorab-Tracker-Erstellung kompiliert; r51 gegen `nginx-mod-dev-1.30.6-r0` gebaut, `apk verify` OK, installiert und unverändertes `/etc/nginx/nginx.conf` mit `nginx -t` erfolgreich. SHA-256 `8d349111ad01867a7932cddf60d76342461fc7527d7d72649a9deeceb380de59`. |
| PURGE-/Invalidierungsabnahme insgesamt | OPEN | Der gezielte Lauf deckt einen großen, nicht L1-aufnehmbaren Response-Fill sowie einen aktiven normalen Fill ab. Vollständige Rennen mit L2-Revalidierung/304, Client-Abbruch, mehreren gleichzeitigen Writes desselben Keys und Tracker-Allokationsfehlern sind nicht nachgewiesen. |

Das r51-Paket liegt unter `alpine/dist/nginx-mod-l1-cache-1.30.6-r51.apk`.

## r52–r53: Race zwischen Filterende und L2-Commit schließen

Ein PURGE kann nach dem ersten End-of-body-Check und noch vor dem nativen
L2-Commit eintreffen. Jeder beendete Stream reiht deshalb nun einen
Hintergrund-Subrequest ein, der nach dem aktuellen NGINX-Event-Handler läuft.
Er markiert den Shared-Memory-Tracker als `POST_COMMIT` und prüft dann die
PURGE-Markierung: Ist sie gesetzt, ruft er den nativen Purge-Handler auf und
lässt den Post-Subrequest-Callback die Sperrmarkierung erst nach Erfolg
entfernen. Ist sie nicht gesetzt, liefert der Check 204. Ein späterer PURGE
sieht `POST_COMMIT` und geht direkt durch den nativen PURGE-Pfad.

| Prüfschritt | Ergebnis | Nachweis |
|---|---|---|
| r52 APK-Build, Installation und Modulstart | PASS | Alpine `nginx-mod-dev` 1.30.6-r0, musl; APK-Signatur `apk verify` OK; installiert über r51; unverändertes System-NGINX `nginx -t` erfolgreich. SHA-256 `c0aec7a1fc0e21f60040660f0ac3344632b64cba7f47d9ee2f619dc101462bc2`. |
| PURGE während nicht L1-aufnehmbarem Fill mit nachgelagertem Commit-Check | PASS für diesen Lauf | Stock-NGINX 1.30.6 mit 2 Workern, Body 512 KiB bei L1-Maximum 128 KiB. Beim PURGE waren 60108 Bytes angekommen; PURGE antwortete 202. Anfrage während des Fills: `BYPASS`, Body-SHA-256 `4480bd37be84462ae5b45c47cf5b4546c92d2d2360853571c43984dd982499ea`. Der erste Fill lieferte den alten Body (`160f9907667a6fd29f8cd787355bca09699203bab04dab1541101d5d5bfbf4fa`). Nach seinem Ende: neuer Body als `L1_MISS_L2_MISS`, dann derselbe Body als `L1_MISS_L2_HIT`; beide SHA-256 entsprachen der neuen Datei. Temporär `/tmp/l1-purge-check-r52-1`. |
| r53 Build und NGINX-Modulprüfung | PASS | Nach Entfernung eines nicht mehr benutzten Pending-Flag-Helfers erneut gegen Alpine-`nginx-mod-dev` 1.30.6-r0 gebaut; integrierte Modulinitialisierung erfolgreich. APK-Signatur `apk verify` OK, installiert über r52; unverändertes System-NGINX `nginx -t` erfolgreich. SHA-256 `2d4bc96630f7fb82f1af04c3ae3fb8e50aed692154ee1468b7bc10164149c942`. |
| Vollständige PURGE-Race-Abnahme | OPEN | Der gezielte Fill deckt einen großen, nicht in L1 aufnehmbaren Origin-Fill über den nachgelagerten Commit-Check ab. 304-Revalidierung, Abbruch, mehrere gleichzeitige Writes desselben Keys sowie absichtlicher Shared-Memory-Mangel sind nicht abgenommen. |

Das r53-Paket liegt unter `alpine/dist/nginx-mod-l1-cache-1.30.6-r53.apk`.
