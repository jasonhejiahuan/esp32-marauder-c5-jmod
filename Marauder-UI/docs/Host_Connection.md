# Host_Connection v1

JASON Studio · board scope: Waveshare ESP32-C5-WIFI6-KIT-N32R8-U and Seeed Studio XIAO ESP32-C5, custom Marauder v1.18.0.

XIAO advertises exactly one native USB control endpoint, feature bits 15 and one diagnostic endpoint. It does not advertise or implement UART baud negotiation or data-link binding. Waveshare retains its optional second endpoint and those operations.

## Ownership and compatibility

`StorageService` validates and executes both `StorageCommands` and typed binary operations. The application loop is the serialised service executor; the transport task reads/frames/writes only. `HostConnectionClient` uses bounded pending futures, never a second reader. `Bridge` owns each physical endpoint and dispatches before UTF-8 decoding. The frontend and Python HTTP/SSE service remain in place.

Modes: LEGACY_CLI (old text/Base64 file adapter); SINGLE_LINK (all channels multiplexed on one endpoint); DUAL_LINK (UART control/console/telemetry, native USB file/capture plus management). Opening/discovery do not stop scans, format storage or reset pins. USB serial/JTAG is still the physical transport: this is not USB MSC, a network interface, encryption or authentication. Ordinary terminals require Dashboard disconnect and exclusive ownership.

`protocolinfo --machine <id>` retains the old fields and adds `Host_Connection`: version, device base MAC, endpoint, services, features, limits and roles. No speculative binary is sent without this capability or a validated framed recovery notice. Old Dashboard clients continue using CLI; old firmware uses LegacyCliClient.

## Frame

`COBS(header || payload || CRC32) || 00`. All integer fields are little endian, explicitly encoded; signed fields use two's complement. Header offsets:

| offset | field | bytes |
|---|---|---:|
|0|version = 1|1|
|1|channel|1|
|2|kind|1|
|3|flags = 0 (reject unknown)|1|
|4|operation|2|
|6|payload length|2|
|8|session ID|4|
|12|request ID|4|
|16|per-direction, per-endpoint sequence|4|

CRC-32/ISO-HDLC, polynomial 0x04C11DB7, reflected 0xEDB88320, init/xorout FFFFFFFF; covers unencoded header+payload. `123456789` → CBF43926, matches zlib.crc32. Empty COBS → 01; 00 → 0101; 11220033 → 0311220233. Maximum payload 3072, raw 3096, encoded accumulator 3109 bytes (plus delimiter on TX). Fragmented/coalesced reads are supported. Bad CRC/version/flags/length/COBS is discarded, oversize discards through next delimiter. A malformed binary frame never falls through to CLI.

Channels: 0 urgent/session; 1 requests/results; 2 capture; 3 file data; 4 telemetry; 5 console/logs. Kinds: 1 REQUEST, 2 RESULT, 3 EVENT, 4 DATA, 5 ERROR. ERROR payload is a length-prefixed string. Strings are u16 byte length + UTF-8 bytes; paths <=160 bytes, CLI <=2048, no NUL. Trailing payload is rejected by typed parsers. Reserved fields are not silently repurposed.

Sessions use random nonzero u32 IDs. Host requests start at 1; wire sequences start at 1 and increase in dispatch order separately on each link/direction. Urgent requests can overtake unsent ordinary requests: firmware retains a 64-ID replay window and 16 terminal result cache. Duplicated known request/payload can replay the result, changed payload is rejected, evicted/uncertain requests are indeterminate. IDs must be renewed through a new session before u32 exhaustion. No exactly-once promise across reset. Capture stream IDs and transfer IDs are random nonzero u32 scoped to session; capture sequence starts at 0 and ends the stream explicitly before its produced counter would wrap; routing epoch changes on transitions. Gaps count dropped records, not proven over-air losses.

## Negotiation / recovery

1. Correlated `protocolinfo --machine <id>`.
2. `host_connection upgrade 1 <32 hex nonce>`.
3. `@HOST_CONNECTION READY <nonce> <session>` is final plaintext; no prompt/log follows.
4. HELLO frame and HELLO RESULT establish BINARY_ACTIVE. No operational requests before this result.

HELLO timeout 2500 ms; active lease 12000 ms renewed by valid requests, idle host PING every 2 seconds with 4-second response timeout. Current v1 requires max_payload >=3072 and negotiates file window 1024..8192; backend starts a file with 4096 bytes of credits, replenishes after min(2048, granted_window) verified bytes, including a negotiated 1024-byte window. HELLO optional features u32 is sent only when advertised: file=1, capture=2, telemetry=4, CLI=8, dual=16. ACK reports selected features and operation catalog. Unsupported features are not executed.

Lease loss/disconnect enters RECOVERY, stops transfer/capture production, clears queued traffic and partial decoders, and preserves any already-started frame tail while the link remains connected. It never restores arbitrary binary bytes to the CLI parser. A framed RECOVERY_NOTICE is repeated every 500 ms. The host, after validating it, sends a delimiter and framed CLOSE, receives the nonce, then sends the exact `host_connection cli <nonce>\n` boundary. Firmware replies `@HOST_CONNECTION CLI`. A host failure between CLOSE and that boundary can recover via another framed CLOSE. No unframed escape sequence. Invalid/nonmatching recovery text is discarded.

Successful control HELLO requests one idle-only white RGB flash (128/255, 180 ms). EnableLED applies; existing colour/mode is not overridden and any new LED state cancels feedback immediately. Data-link HELLO does not flash a second flash.

## Typed operations

Notation: s=length-prefixed string, u8/u16/u32/u64 LE, empty means zero payload. Opcodes hex below. Requests use command channel except those explicitly marked control/telemetry.

| op | request | result / event |
|---|---|---|
|01 HELLO (control)|s nonce,u16 max,u32 window,[u32 features]|s device,u16 max,u32 window,u32 lease,u8 endpoint,u8 role,u32 epoch,[u32 features,u8 count,count*u16 ops]|
|02 PING (control)|empty|empty|
|03 CLOSE (control)|empty|s nonce|
|04 BIND_TOKEN (control)|empty|s device,u32 session,16 token bytes,u32 expiry_ms|
|05 BIND (control, data endpoint)|s device,u32 session,16 token bytes|s device,u32 session,u32 epoch|
|06 ROUTE (control)|u8 mode (0 single,1 dual)|u8 mode,u32 epoch|
|07 STATUS / 403 CAPTURE_STATS (control)|empty|bounded metrics described below|
|08 RECOVERY_NOTICE (event)|—|s device,s nonce,u32 last_rx_sequence,u8 endpoint|
|100 CLI_EXEC|s command|console events correlated by request ID, then explicit empty RESULT; ERROR on output overflow|
|200 FILE_OP|u8 operation,s path,s destination,u32 offset,u32 length|u8 op,u32 size,u32 offset,i32 next,u32 total,u32 used,u16 entries,s resulting_path,u16 data_length,data,entries|
|210 FILE_START|s path,u32 initial credits|u32 transfer,u32 size,u32 generation,s path,u32 window|
|211 FILE_CREDIT (control)|u32 transfer,u32 acknowledged_offset,u32 added_bytes|empty|
|212 FILE_CANCEL (control)|u32 transfer|empty|
|213 FILE_RECORD (data, file channel)|—|u32 transfer,u32 offset,u32 generation,u16 length,raw bytes|
|214 FILE_END (event, file channel)|—|u32 transfer,u32 total,32 SHA-256 bytes|
|300 TELEMETRY (telemetry)|empty|u64 uptime_ms,i32 temperature_milliC,20*u32 metrics|
|400 CAPTURE_START|empty or u8 host-only=1|u32 stream,u32 epoch,u16 snaplen,u8 clock=1|
|401 CAPTURE_STOP (control)|empty|empty command acknowledgment; separately ordered capture-channel EVENT described below|
|402 CAPTURE_RECORD (data, capture)|—|u32 stream,u32 sequence,u32 epoch,u64 timestamp_us,u16 original,u16 captured,i8 RSSI,u8 channel,raw bytes|
|500 CONSOLE (event)|—|text bytes; request=0 asynchronous, otherwise CLI_EXEC context|

File operations 1 list,2 stat,3 read,4 rename,5 mkdir,6 trash,7 restore; additive capability `files-delete-v1` enables8 delete and9 emptytrash. Entry: s name,u8 directory,u32 size. Delete accepts only descendants of `/.trash/`; emptytrash accepts only `/.trash`, retaining that root. No overwrite, traversal or renaming directly into trash. Both new operations are mutations: never retry an ambiguous timeout. Deletion is bounded to4096 removed entries and a10-second between-syscall budget; `delete_partial`/`delete_limit_partial` explicitly signal incomplete removal. The Dashboard requires a confirmation and refreshes after failure. No Flash sanitization or cross-reset exactly-once claim. All transport adapters use the same service/activity checks. A file stream holds a lease; scans/BLE/file mutations remain excluded even with two cables. CLI file commands in binary mode must use FILE_OP; OTA and interactive CLI require exclusive legacy handoff.

FILE_END follows file records on the same endpoint queue. Backend validates identity, generation, contiguous offsets, expected size and SHA-256, streams into its own sink, then fsyncs. Credits account for outstanding unacknowledged bytes; cancellation stops new work and closes only its own handles/temp state. No automatic resume; an uncertain source/range starts a new explicit transfer. Mutation timeout/disconnect is indeterminate and is not blindly retried.

Telemetry order: cpu_mhz,cores,revision,reset_reason,heap_total,heap_free,heap_min,heap_largest,psram_total,psram_free,flash_physical,flash_window,app_size,app_slot,storage_total,storage_used,settings_total,settings_used,wifi_mode,ble_active. Unknown temperature INT32_MIN. Display disabled → no telemetry requests; lease PING is separate transport maintenance. Background view uses existing reduced subscription frequency. Binary storage occupancy is cached for 15 seconds while idle and held at its last sample during WLAN/capture activity to avoid flash traversal in the control path; file listing gives current usage when idle.

STATUS: epoch u32,dual u8,active u8; ten u32 stream,produced,device_queue_drops,transmitted,queue,queue_high,callback_max_us,queue_age_max_us,console_drops,request_drops; two endpoint groups of five u32 framing_crc_errors,oversized,partial_writes,queue_drops,transitions; six u32 callback_count,invalid_rx,invalid_length,wifi_mode_error,wifi_mode,promiscuous_error; two u8 promiscuous,data_link_failed; u32 legacy_queue_drops; eight u32 callback histogram counts <=2,5,10,20,50,100,500,infinity microseconds; u32 transport_stack_min,executor_stack_min. Most diagnostic counters are boot cumulative; capture produced/drop/sent/queue high/age reset per stream. Host RTT is measured on one clock. Do not subtract device timestamp from Mac monotonic time.

## Capture and bounded resources

Wi-Fi callback copies into preallocated slots and nonblocking FreeRTOS queues. No USB/FS/logging/encoding/allocation there. FreeRTOS queues support the actual multiple-context consumers/producers; no assumed lock-free SPSC. Live capture bypasses Buffer::save batching. Legacy discovery processing uses a separate pool so the frontend and existing scan logic retain their outputs. Drop-new on pool/queue exhaustion; sequence is assigned before pool acquisition. Capture attaches only to an already-running supported passive receive mode; it never starts a radio operation implicitly.

| resource | limit / overflow |
|---|---|
|firmware request queues|8 ordinary +4 urgent; explicit error/drop counter|
|per endpoint TX|8 urgent,128 normal,2 bulk frames; bounded PSRAM backing; overflow counters/error|
|callback pools|32 capture +32 legacy slots, snaplen2324; drop new|
|TX submission|128 bytes per UART turn; up to4*128 native bytes, arbitrate between complete frames; partial tail retained|
|capture fairness|up to8 capture frames before an available normal frame; urgent always first at boundary|
|backend pending requests|24 ordinary,32 including reserved urgent|
|backend TX bytes|8192 ordinary,12288 including urgent; reject excess|
|file sink queue|16 records; fail transfer on overflow, never block reader|
|transfers|one active,16 retained,6 MiB each; only owned temporary files pruned|
|PCAP sink|128 records; drop new/full or queued >1s; 256 MiB/file; retain last4 per service process|
|live PCAP subscribers|4 clients,128 records each; isolated drop-new on slow client|
|metrics/console|existing bounded snapshots/logs; no per-packet Model lock or SSE packet payload|

CAPTURE_STOP also emits a capture-channel EVENT behind previously submitted capture frames on the same data endpoint: five u32 values stream, epoch, produced, device_queue_drops, submitted. The producer is disabled, in-progress callback copies complete, unsent slots are discarded and counted, then this boundary is queued. The backend keeps receiving until that event; the control acknowledgment alone does not end the sink. Final gaps include discarded tail records. Invalid/mismatched end records are counted, not accepted as completion. A lost link cannot confirm this boundary and is reported separately. No retransmission or lossless-stop claim.

PCAP uses DLT105 IEEE802_11, no radiotap; timestamp is microseconds since boot, a relative epoch, not UTC. Radio metadata exists on Host_Connection records, not misrepresented as radiotap. Each sink/subscriber receives a valid global header and complete records. USB, device pool, backend queue, sink and subscriber drops are separate. Capture has no retransmission. Raw data never goes into JSON/Base64/SSE in Host_Connection mode.

## HTTP integration (loopback, existing session token)

Use existing `/api/session` token and `Origin`/`X-Desk-Token` for POSTs; do not expose the server publicly.

- POST `/api/connect` `{port,mode}`; mode auto/LEGACY_CLI/SINGLE_LINK.
- POST `/api/transport` `{operation:"bind",port:"<native USB>"}` after UART control connection; `status` and explicit `fallback` available.
- POST `/api/transfers` `{operation:"start",path,transfer_id?}` returns transfer_id. An optional 32-lowercase-hex browser-generated ID is saved before the HTTP request; repeating that ID/path returns the retained job, and a different path is rejected. Same endpoint `status`/`cancel` with transfer_id; GET `/api/transfers` lists retained transfers; GET `/api/transfers/download/<id>` downloads only completed/verified result.
- Lifecycle CREATED → RUNNING → COMPLETED, or CANCELLING → CANCELLED, or FAILED. Browser reload retains transfer ID before the start response; browser only polls backend progress, never requests device chunks.
- POST `/api/capture` `{operation:"start"|"stop"|"status"}`; start attaches to an existing passive scan. GET `/api/capture/stream/<id>` is live binary PCAP; GET `/api/capture/download/<id>` after stopping. A slow HTTP consumer is isolated. `curl --no-buffer http://127.0.0.1:8765/api/capture/stream/<id> | wireshark -k -i -` can be used if Wireshark is installed; installation is not required by the service.
- Existing `/api/files` operations retained; in Host_Connection mode low-level `read` returns octet-stream with metadata headers. Existing UI uses managed transfers. Legacy mode retains text/Base64 internally.
- SSE contains aggregated status only. No UI redesign or new radio buttons.

Dual pairing requires authoritative UART device identity/session plus a one-use16-byte token (5s expiry); native discovery and HELLO must agree before BIND/ROUTE. Port name/order/VID is discovery only. Binding does not silently reroute active streams: pause/stop, drain frame boundary, acknowledged ROUTE, new epoch. Data failure latches, stops production, control stays usable. Fallback is explicit and acknowledged; old queues are discarded only at failed-link boundary; any interruption is visible. Control expiry also stops data production. No claim of lossless migration.

## Build / tests / limitations

Firmware `tools/build_host_connection.py` retains the approved partition table,4MiB OTA slots,6MiB LittleFS,PSRAM and RGB calibration. It uses a clean `.build/Host_Connection` directory and rejects unexpected generated cpp files (a previous build directory contained duplicate stale sources). App-only `tools/package_usb_ota.py` + `tools/usb_update.py` verifies boot/table/config hashes and startup before committing the new slot. Non-app changes still require full backup/migration.

Board-scoped early MSPI wrappers place cache disable/restore around clock changes in the existing Arduino3.3.6/IDF5.5.2 path; analogous cache protection is present in [Espressif's MSPI source](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_hw_support/mspi/mspi_timing_tuning/mspi_timing_tuning.c). This addresses observed early PSRAM/IllegalInstruction failure; it does not change clock, eFuse or bootloader. Protocol scratch/pools live in PSRAM; atomics/queue metadata remain internal. Build flags and image hashes are saved with artifacts.

Run Python unittest discovery and Node `tests/test_frontend.cjs`; compile firmware `tests/host_connection/codec_test.cpp` with ASan/UBSan for actual production codec/feedback tests. Hardware validation logs and profiling are separate; synthetic tests do not establish USB performance. Results and unresolved claims are in the task's validation report. Heavy traffic can drop capture/legacy observation records; UART control has lower bandwidth than native USB. No guaranteed unchanged latency, zero drops or hardware visual LED confirmation without measurement.

Real-device scripts live in `tools/validate_host_connection_*.py` and require `--run-hardware`; `--control`, `--data`, `--output` select endpoints/artifacts. They only use passive scanning, read existing files, or mutate their own uniquely named empty test directory, left in device trash. `profile_host_connection.py` uses localhost8766 and `/tmp/hc-profile-stage` phase signals open/reload/slow/finish; real browser actions must accompany those labels. These scripts are separate from synthetic unittest discovery.

The standalone firmware updater recognises only a CRC-valid Host_Connection RECOVERY_NOTICE before sending framed CLOSE and the correlated CLI boundary. It does not log uncorrelated binary input. A Dashboard-owned connection is first released through its API and its previous single/dual mode restored after updating. `session_refresh` names the periodic PING maintenance; `lease_renewal` describes the device expiry update.

## Firmware release identity

`esp32_marauder/FirmwareVersion.h` is the single source for upstream and extension release numbers. The complete identifier is `1.18.0-jmod1.1`. Updating upstream to1.19.0 while leaving extensions unchanged produces `1.19.0-jmod1.1`; extension changes advance the jmod number independently (minor for compatible additions, patch for fixes if needed, major for incompatible extension changes). Never compare version strings lexicographically; compare upstream and numeric extension components separately. The combined form is valid SemVer prerelease syntax; it is a custom firmware identifier, not an upstream official release.

`GET_VERSION` opcode0x0009, control channel, empty request returns five u16-length UTF-8 strings: complete version(max64), upstream(max32), extension(max32), build profile(max64), target(max96). Only issue it when listed by HELLO_ACK; discovery advertises `version-v1`. Dashboard reads it once per connection and displays the complete version. Protocol version1 is independent. Legacy discovery `firmware` and CLI `Version:` retain their original upstream meaning for older clients; additive `firmware_version` and `Firmware Build:` expose the custom release. OTA status `build`, package manifest `version`, and the binary response agree. Packaging reads the actual built header and rejects an image missing that exact version string; updater checks the trial-boot version before confirmation.

`LINK_DIAGNOSTICS` opcode0x000A, control channel, empty request returns u8 format1,u8 endpoints2; for native then UART: u8 mode(0CLI/1pending/2active/3recovery),u8 recovery reason(0unspecified/1lease-or-handshake-expired/2physical-USB-detection), then eight u32 last-RX age(ms),RX sequence,TX sequence,received bytes,submitted bytes,frame length,frame offset,completed submitted frames; finally u8 native plugged,u32 native TX free bytes,u32 SOF-missing sample count (optional on earlier builds). This low-frequency failure snapshot is separate from packet output. Submitted bytes/frames are not proof the host received them.

A single SOF-based `HWCDC::isPlugged()` false sample does not terminate a Host_Connection session. On this Mac/C5 it was observed triggering recovery despite valid traffic only1.2s earlier and a connected indication immediately afterwards. Firmware records missing samples and uses the12s protocol lease for disconnect/expiry; the bounded pools remain in effect while awaiting expiry. This avoids confusing a short physical detector fluctuation with confirmed session loss. Espressif's [connection monitor](https://github.com/espressif/esp-idf/blob/v5.5.2/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_connection_monitor.c) bases that hint on host SOF timing; it is not an application-session acknowledgment.

## jmod 2.0 additive operations

Capabilities `selection-v1`, `capture-host-only-v1`. Header/protocol version unchanged.

- `SELECTION_LIST=0x110`, COMMAND request: u8 kind (0 AP, 1 STA), u16 offset, u32 snapshot fingerprint (zero starts a snapshot). Response: u8 kind, u32 fingerprint, u16 total, u16 offset, u16 count, repeated {u16 real index, 6-byte MAC, u8 selected}. Page ≤128, table ≤4096. Changed fingerprints fail rather than mixing pages.
- `SELECTION_SET=0x111`, COMMAND mutating request: u8 kind, u8 selected, 6-byte MAC, u8 channel, i8 RSSI, u8 security (255 unknown), u8 SSID length ≤32, SSID bytes, 6-byte parent AP MAC. Existing identities retain metadata. Missing identities import supplied observed metadata; STA may import an unselected parent. Response: u8 kind, u16 index, 6-byte MAC, u8 selected. Existing bounded mutation deduplication applies.
- `SELECTION_CHANGED=0x112`, COMMAND event, empty payload: fetch an authoritative snapshot at the next owner service boundary. This does not carry per-packet observations.
- `CAPTURE_START=0x400` accepts the old empty payload, or exactly one byte `1` for host-only PCAP. Unknown values and extra bytes fail. Response layout is unchanged. Suppression is tied to the currently open PCAP, persists beyond stream/session closure, and resets when a new file/capture opens. No persistent settings writes.

See [jmod2.md](jmod2.md) for frontend behaviour and PCAP lifecycle/limits.

## Native sniff stream v1 (jmod 2.1)

`SNIFF_START=0x404` is a typed command-channel operation: mode u8, flags u8, channel u8; result matches CAPTURE_START. Discovery advertises `native-sniff-stream-v1`. See [native-sniff.md](native-sniff.md) for modes, validation, native filtering, losses, and unified console syntax. Legacy wire/HTTP capture operations remain compatible; the standalone console command has been removed.


## Discovery registry correction — jmod2.1.1

`scancombined` now registers valid AP/STA observations in the original firmware lists before publishing observations. `sniffbeacon` registers decoded AP metadata on the existing service executor. Neither path selects a device; repeated discovery retains selection and stable indices. Hidden beacons retain a learned name; zero-length names use the MAC in ordinary CLI output. STA relationships update without duplicate links. RF receive, native packet filters, PCAP output and activity exclusions are unchanged.

The existing 4096-entry and internal-memory guards remain. A bounded 128-entry PSRAM table retains observations rejected temporarily by the memory guard (drop newest if full), updates retries by identity, and retries after `stopscan` shuts down the radio. Clear/load discards pending observations so they cannot restore explicitly cleared entries. `@DISC` status includes cumulative `registry_errors` (failed registration attempts, including temporary memory refusals; not the number of currently missing APs). Combined repeated registration is limited to once per second per observation unless metadata changes; no additional Dashboard polling is introduced.

`list -a` lists registered APs, including unselected entries. Deselecting clears only the selection flag. Device RAM lists reset on reboot; the Dashboard can still contain older observations. During an active binary PCAP stream, the existing `stopscan` requirement for ordinary `list` remains. Selection snapshots are still refreshed at explicit selection/stop/CLI-change boundaries.

### UART session speed negotiation (jmod 2.4.0)

Only a verified UART control session advertising operations 11–13 may negotiate.
Native USB and ordinary CLI connections retain their existing transport settings.
Before binding a second link, the idle host measures a 115200 baseline, then tries
2000000, 1500000, 921600 and 460800. Each candidate must echo eight 1024-byte
patterns without corruption, improve median round-trip latency by at least 10%,
and keep its worst measured round trip below the baseline median. These short
probes select a candidate; they do not prove reliability under every workload.

`BAUD_PREPARE` (11) accepts a little-endian uint32 rate and returns uint32 rate,
uint16 switch delay in milliseconds, uint16 trial duration in milliseconds.
`BAUD_PROBE` (12) echoes 1–1536 bytes. `BAUD_COMMIT` (13) accepts and returns the
current uint32 rate. Firmware trials expire after 3000 ms without commit. A lost
prepare reply requires waiting out the possible trial before probing 115200.
A lost commit reply is ambiguous: stop traffic, wait beyond the 12000 ms lease,
restore 115200, and establish a new session. Do not renew the old lease while
waiting. Clean CLI release restores 115200 after the textual recovery boundary.
Busy radios reject negotiation but remain accessible at the default rate.
