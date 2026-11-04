# Technischer Status des eigenständigen L1-Moduls

Stand: Arbeitsbranch auf Basis `c59350be6`, Alpine x86_64,
`nginx-1.30.6-r0`, musl.

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

## Nachweise und Status

| Anforderung | Status | Nachweis / Grenze |
|---|---|---|
| Dynamischer Build ohne NGINX-Kernänderungen | PASS | `abuild -r` baut das Modul gegen `/usr/src/nginx-1.30.6`; NGINX-Binary bleibt das installierte Alpine-Paket. |
| Nachinstallation und Laden des Moduls | PASS | `nginx-mod-l1-cache-1.30.6-r23.apk` installiert; `/etc/nginx/modules/50_l1_cache.conf` lädt die `.so`; Paketbuild, echte Proxy-Konfiguration und reguläre Systemkonfiguration bestanden `nginx -t`. Letztere meldet eine nicht fatale `variables_hash`-Warnung. |
| Echter Shared-Memory-L1-HIT | PASS | Bereits vorhandener Lauf mit mehreren Workern: zweiter Request `L1_HIT`, Body bytegleich, kein zusätzlicher Origin-Request. |
| Native L2-HIT-Promotion | PASS | Ein L2-HIT mit korrigiertem `Age` wurde nach vollständigem NGINX-Neustart zu `L1_MISS_L2_HIT` promotet; der Folgezugriff war `L1_HIT`. |
| L2-Dateigeneration bei PURGE/Neubefüllung | PASS | Einmaliger gezielter Lauf mit installiertem r23 auf unverändertem Alpine-NGINX 1.30.6-r0, zwei Workern und 8-MiB-Body: Während der langsame alte L2-HIT ausgeliefert wurde, erfolgte PURGE und ein neuer Origin-Fill. Der Folgerequest war `L1_MISS_L2_HIT` und lieferte den neuen Body, nicht den alten. Alter Body SHA-256 `38f9d816627d9dffa5f1d41d176c890d0e02a555cb31a5982bbdcd27816d84be`; neuer Body SHA-256 `2daeb1f36095b44b318410b3f4e8b5d989dcc7bb023d1426c492dab0a3053e74`; Origin-Requests: 2; alle fünf HTTP-Antworten 200. Temporärer Lauf: `/tmp/l1-l2-generation-r23-oneoff`, Ergebnisse: `/tmp/l1-l2-generation-r23-results`. |
| Exakte Schlüsselidentität | PASS für konfigurierte Integration | Modul verlangt exakt `proxy_cache_key $l1_cache_key` und die passende Cache-Zone. Gezielt abweichender Proxy-Key: L2 MISS/HIT, `X-L1-Hits: 0`, bytegleiche Antworten, genau ein Origin-Request; L1 wurde sicher umgangen. |
| L2-Schreibfehler | PASS | r23-Lauf mit nach dem Start schreibgeschütztem L2-Verzeichnis: zwei 200-Antworten blieben `L1_MISS_L2_MISS`, zwei Origin-Requests und zwei native Schreibfehler; kein L1-HIT. |
| Origin-MISS speichert zuerst L2 und befüllt danach L1 | PASS für getesteten Stock-Pfad | r23 auf unverändertem Alpine NGINX 1.30.6-r0: variabler `proxy_pass`, erfolgreicher L2-MISS und folgender bytegleicher `L1_HIT`; ein ungefüllter `only-if-cached`-Schlüssel lieferte 504 ohne Origin-Request. |
| `only-if-cached` bei variablem `proxy_pass` | PASS für getesteten Pfad | r23: Variable-Ziel lieferte vorhandenes READY-Objekt als `L1_HIT`; ein L1-MISS gab 504 zurück, bevor die Proxy-Location den Origin startete. |
| `proxy_cache_bypass` / `proxy_no_cache` | PASS | r8-Lauf: benutzerdefinierter Bypass ergab `BYPASS`, invalidierte L1; Folgezugriff war `L1_MISS_L2_HIT`. `proxy_no_cache` verhinderte wiederholte L1-Aufnahme. |
| Force-Preconditions und Isolation | PASS für geprüfte Fälle | Vorhandener Lauf: If-None-Match→304, If-Match→412; Force-Varianten mit Cookie/Vary wurden getrennt. |
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

## Paket

Aktuelles getestetes Artefakt: `alpine/dist/nginx-mod-l1-cache-1.30.6-r23.apk`
(SHA-256 `c8ac76abd5ffc3f417aecc8c37e7cee0fe00e3147d282934c37791f8f44a62e4`).
Es wurde gegen Alpine `nginx-mod-dev-1.30.6-r0` gebaut und über r22 installiert.
Die frühere r21-Prüfung bleibt als historischer Nachweis erhalten:
`alpine/dist/nginx-mod-l1-cache-1.30.6-r21.apk` (SHA-256
`a7585ed1fe61565d549678bbfca4b713d03069b3b1718e20cde875377db07be3`).
Der passende öffentliche Signaturschlüssel liegt daneben. Der private
Buildschlüssel wird nicht ausgeliefert. Der r21-Build wurde gegen die
installierten Alpine-Quellen für NGINX 1.30.6-r0 gebaut und durch `nginx -t`
geprüft. Das Paket hängt exakt von
`nginx=1.30.6-r0` und `nginx-mod-http-cache-purge=1.30.6-r0` ab.
Nach Installation des r21-Pakets auf dem vorhandenen Alpine-System war auch
`sudo nginx -t` erfolgreich. Der gezielte Laufzeittest bestätigte den L2-only
HIT und 504 ohne Origin-Zugriff bei L2-MISS.
`apk verify alpine/dist/nginx-mod-l1-cache-1.30.6-r21.apk` meldete `OK` mit
dem im Repository enthaltenen und auf dem System vertrauenswürdigen Schlüssel.

## Gesamtstatus

**Nicht vollständig abgenommen.** Das Modul ist nachinstallierbar und die
geprüften L1/L2- und Age-Pfade funktionieren im geprüften Alpine-System. Die
vollständige verbindliche Abnahmematrix bleibt offen.
