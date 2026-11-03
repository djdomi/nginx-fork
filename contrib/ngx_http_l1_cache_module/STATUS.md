# Technischer Status des eigenständigen L1-Moduls

Stand: Arbeitsbranch auf Basis `c59350be6`, Alpine x86_64,
`nginx-1.30.6-r0`, musl.

## Nachweise und Status

| Anforderung | Status | Nachweis / Grenze |
|---|---|---|
| Dynamischer Build ohne NGINX-Kernänderungen | PASS | `abuild -r` baut das Modul gegen `/usr/src/nginx-1.30.6`; NGINX-Binary bleibt das installierte Alpine-Paket. |
| Nachinstallation und Laden des Moduls | PASS | `nginx-mod-l1-cache-1.30.6-r21.apk` installiert; `/etc/nginx/modules/50_l1_cache.conf` lädt die `.so`; Paketbuild und installiertes System bestanden `nginx -t`. |
| Echter Shared-Memory-L1-HIT | PASS | Bereits vorhandener Lauf mit mehreren Workern: zweiter Request `L1_HIT`, Body bytegleich, kein zusätzlicher Origin-Request. |
| Native L2-HIT-Promotion | PASS | Ein L2-HIT mit korrigiertem `Age` wurde nach vollständigem NGINX-Neustart zu `L1_MISS_L2_HIT` promotet; der Folgezugriff war `L1_HIT`. |
| L2-Dateigeneration bei PURGE/Neubefüllung | PASS | Mit r18 lieferte ein langsamer Client noch den alten 8-MiB-L2-Body aus, während der native L2-Schlüssel extern gepurgt und mit einer neuen Origin-Version neu befüllt wurde. Der Folgezugriff war `L1_MISS_L2_HIT` mit dem neuen Body, nicht `L1_HIT` mit dem alten. HTTP: prime/purge/refresh/final jeweils 200; Origin-Anfragen: 2; alter Body SHA-256 `a82389c100e7139972d596d025b5dabf0be38165d61bd93bf609018eda7b38fa`, neuer Body `bbaa115f96618af1795a163c141952220f6402261f03093eecd0488d3584c82a`. |
| Exakte Schlüsselidentität | PASS für konfigurierte Integration | Modul verlangt `proxy_cache_key $l1_cache_key` und passende Cache-Zone; falsche Schlüssel/Zonen führen zu L1-BYPASS. |
| L2-Schreibfehler | PASS | Vorhandener Lauf mit nicht beschreibbarem L2-Verzeichnis: Antwort blieb bytegleich; kein READY-L1-Objekt wurde veröffentlicht. |
| `proxy_cache_bypass` / `proxy_no_cache` | PASS | r8-Lauf: benutzerdefinierter Bypass ergab `BYPASS`, invalidierte L1; Folgezugriff war `L1_MISS_L2_HIT`. `proxy_no_cache` verhinderte wiederholte L1-Aufnahme. |
| Force-Preconditions und Isolation | PASS für geprüfte Fälle | Vorhandener Lauf: If-None-Match→304, If-Match→412; Force-Varianten mit Cookie/Vary wurden getrennt. |
| Force mit `only-if-cached` | PASS | Mit installiertem r17 und Force an lieferte ein Request auf leere L1/L2 504; das Origin-Zugriffslog blieb leer. r18 ändert diesen Pfad nicht. |
| `only-if-cached` bei Proxy-Bypass oder deaktiviertem L1 | PASS | Mit installiertem r19: eine aktivierte L1-Location mit zutreffendem `proxy_cache_bypass` und eine Location mit `l1_cache off` lieferten jeweils 504; das Origin-Zugriffslog blieb bei 0 Requests. |
| `only-if-cached` bei L2-only HIT/MISS | PASS | Mit installiertem r21 wurde `/seed` in L2 gefüllt. Nach vollständigem Proxy-Neustart (L1 leer) lieferte `Cache-Control: only-if-cached` HTTP 200 mit `X-Cache: HIT`; der Body entsprach bytegenau dem Fill. Ein nicht vorhandenes `/miss` lieferte 504 mit `X-Cache: MISS`. Das Origin-Log enthielt nur den `/seed`-Fill und keinen Request für `/miss`. Laufverzeichnis: `/tmp/l1-r20-oic.zbfe2Q`. |
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

## NGINX-Kernänderungen im Arbeitsbranch

`git diff master...HEAD -- src` zeigt aktuell 14 geänderte Kerndateien. Der dynamische Modulbuild und die Laufzeitprüfung oben belegen, dass die folgenden Fork-Erweiterungen keine Voraussetzung für das installierte Modul sind. Das Modul verwendet jedoch private NGINX-Strukturen (`ngx_http_cache_t`, Proxy-Loc-Conf und Upstream-Status); deshalb ist das Paket exakt an die NGINX-Version und ABI gebunden.

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

Getestetes Artefakt: `alpine/dist/nginx-mod-l1-cache-1.30.6-r21.apk` (SHA-256
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
