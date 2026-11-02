# Version note

The standalone capture console examples below describe jmod 2.0. In jmod 2.1 use native `sniffxxx [parameters] --host-only` and `stopscan`; see [native-sniff.md](native-sniff.md).

# jmod 2.0

Firmware version: `1.18.0-jmod2.0`. Upstream and extension releases remain independent. Protocol framing remains Host_Connection v1; new capabilities are additive.

## Visibility and selection

WLAN and BLE have separate offline/unnamed switches. Counts show the number excluded by each enabled switch. Offline exclusions are counted first, so unnamed exclusions do not double count the same device. WLAN includes APs and unicast stations; stations without a learned name are unnamed. A device that later advertises its name becomes visible. Already learned names survive subsequent nameless reports. Completed observation-window rules for offline status remain unchanged.

Right-click an AP/STA row, or use its detail-panel selection button, to stage it. Confirm selection sends explicit selected=true to the device. Cancel selection sends selected=false. These are not toggle commands. Up to 64 identities per user operation, identified by MAC rather than possibly stale indices. Partial completion is reported and never retried automatically. Select changes the target list only; it does not start a radio operation.

Indices are the device's real zero-based `list -a` AP / `list -c` STA indices, never frontend sort indices. Passive combined-discovery records originally live outside those lists. Confirming an absent record imports it into the device list; selecting an absent STA also creates its missing AP association as an unselected, unnamed AP. Unknown security remains Unknown. The serialized service validates identity, bounds, radio/file/capture/OTA exclusions, and memory limits before changes.

Selection snapshots are fetched at connection and after selection/list-changing commands or scan completion. Device events also report CLI changes from the other endpoint. No selection timer or periodic list polling is added. Snapshot pages contain at most 128 entries, with a fingerprint to reject source/index changes between pages. Existing `select -a/-c` toggle semantics are unchanged. During a scan, confirmation is refused by firmware; stop scanning first.

BLE marking is browser-local, keyed by address plus address type, bounded to 1,000 marks and 64 staged items. It does not claim to modify a firmware BLE target list. Both WLAN and BLE support selected-only filtering.

## Device overview

Visible overview: every 2 seconds when idle, every 8 seconds during manual/radio/capture/file activity. Leaving overview, hiding the document, closing the page, or disabling overview stops periodic device-metric requests. Multiple clients share one device request schedule; another visible overview may still request data. A connection performs one initial read-only state sample for validation. Expired clients are removed after 25 seconds. Browser lease messages query no USB data themselves.

Binary command waits continue draining serial and scheduling low-frequency typed telemetry. The same firmware executor serializes operations, so a command that truly blocks that executor cannot be preempted by this change. Ordinary legacy text sessions retain their manual-input exclusion to avoid injecting a query into a raw input prompt.

## Host-only PCAP

First start an existing passive scan supported by Host_Connection: `scancombined`, `sniffbeacon`, `scanall`, or `sniffraw`. Then in the Dashboard console:

```
capture start --host-only
capture status
capture stop
stopscan
```

`capture` is a Dashboard console command backed by typed Host_Connection operations, not a raw terminal PCAP stream. `capture start` without the flag retains the previous behaviour. `--host-only` requires `capture-host-only-v1`; no speculative flag is sent to older firmware.

The flag takes effect at the explicit capture-start boundary. It discards pending local PCAP bytes and detaches local PCAP output for the rest of that scan. Existing bytes saved before the flag remain. Stop, disconnect, data-link failure, or session expiry cannot resume local PCAP saving. Starting a new ordinary scan restores the persistent SavePCAP setting; the flag never modifies it. Other log files are not suppressed.

The device Wi-Fi callback copies to a preallocated 32-slot queue and returns without filesystem or USB writes. Host_Connection sends complete records: stream/epoch/sequence, device-boot microsecond timestamp, original/captured lengths, RSSI/channel, and raw bytes. Frames use COBS plus CRC32; raw payloads never use Base64, JSON, console logs or SSE.

In dual-link mode UART carries control and native USB carries records. In single-link mode the existing arbiter multiplexes them. The Mac reader validates records and queues complete PCAP records to an independent 128-item sink. A slow disk/subscriber cannot block USB; losses are counted separately. Timestamp values are device-relative, not wall time. Frame sequence gaps are transport/queue observations, not proof of wireless reception loss.

The capture response/status gives `capture_id`, `path`, counters and `host_only`. Files are in a private Mac temporary directory `marauder-host-connection-*`, and are bounded to 256 MiB each; the backend retains four capture entries before removing the oldest completed entry on a later start. Download completed captures from `/api/capture/download/<capture_id>`; independent live PCAP subscribers use `/api/capture/stream/<capture_id>`. Save required captures before starting additional ones. The new flag removes the board's storage limit, not the Mac's existing sink limits.

## Console and files

Clear logs clears the backend console ring shared by open Dashboard clients. Sequence numbers stay monotonic, preventing SSE/poll races from restoring old lines. It does not clear observations, capture files or a terminal program's history. The file breadcrumb now has a visible bounded path field; file operations are unchanged.
