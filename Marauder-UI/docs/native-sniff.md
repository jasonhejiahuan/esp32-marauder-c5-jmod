# Native sniff streaming — 1.18.0-jmod2.1

Dashboard console syntax:

```text
sniffprobe --host-only
sniffdeauth --host-only
sniffpmkid -c 6 --host-only
sniffsae --host-only
sniffbeacon --host-only
sniffraw --host-only
sniffpwn --host-only
sniffpinescan --host-only
sniffmultissid --host-only
scanall --host-only
scancombined --host-only
stopscan
```

Without `--host-only`, the existing native command and SavePCAP policy remain unchanged. With the flag, the backend starts the native mode and a PCAP sink in one typed request. `stopscan` ends the binary stream at an acknowledged frame boundary and stops the scan. There is no separate user-facing `capture` command. Existing HTTP capture endpoints and wire CAPTURE_START/STOP remain available for older clients; they are backend operations, not console syntax.

PMKID retains native `-c <channel>`, `-d`, and `-l` semantics. No parameter is inserted automatically. `-d` explicitly requests the native active operation; hardware regression tests only use passive options. `-l` requires an existing selected AP list. Existing firmware-selected AP filtering is retained, including its effect without `-l`. Channel hopping follows the existing ChanHop setting; `-c` does not introduce a new fixed-channel policy.

Bluetooth commands (`sniffbt`, `sniffskim`) are not Wi-Fi PCAP sources and reject this flag. A plain serial terminal has no binary sink; it rejects `--host-only` before changing radio or storage. Old firmware is rejected before any speculative command or binary request.

## Transport and filtering

Host_Connection `SNIFF_START=0x404`, command channel, three bytes: mode, flags, channel. Mode: raw=1, beacon=2, probe=3, deauth=4, pmkid=5, sae=6, pwn=7, pinescan=8, multissid=9, scanall=10, scancombined=11. Flags are channel-present=1, deauth=2, selected-list=4; only PMKID accepts flags. Channel is zero when absent. Unknown values, trailing bytes, and unsupported channels fail. Advertised capability: `native-sniff-stream-v1`; protocol framing stays v1.

Firmware invokes the existing WiFiScan service directly; no CLI string is reconstructed. Existing radio/storage/BLE/OTA exclusions still apply. Success returns the normal capture stream descriptor (stream, epoch, snaplen, clock). Failures are explicit; ambiguous timeouts are not retried. The backend stages at most 32 early records if the data link precedes the control reply.

Specialized sniff modes use the exact existing native PCAP acceptance point. A hook before the SavePCAP setting and Buffer batching enqueues accepted frames immediately into the existing bounded capture queue; no local file is created, including no empty PCAP header. The flag is a scoped file-open policy, not a persistent settings change. Suppression remains for that scan after stream loss. A subsequent ordinary scan restores the configured local-save behavior. Existing local files are not deleted.

RX still only validates/copies/enqueues without waiting; native parsing runs on the existing executor. The hook bypasses Buffer::save entirely. Reception timestamps are recorded in the RX slot; transport encoding happens outside RX. Raw and discovery modes retain their direct capture queue path. Specialized modes inherit native processing time, native filtering and native quirks; this does not claim a new universal 802.11 parser or identical latency to raw capture. PCAP uses DLT 105, device-boot microseconds, and omits the four-byte FCS.

Loss before native parsing is reported by the boot-cumulative `legacy_queue_drops` diagnostic. Capture sequence starts at native acceptance before enqueue, exposing subsequent capture queue losses; it cannot identify which discarded unparsed frames would have matched the native filter. Device queue, USB framing, backend and sink losses retain separate counters. Raw records never enter JSON, Base64 or SSE. Existing byte/queue/file limits are unchanged.

Downloads: `/api/capture/download/<capture_id>` after `stopscan`; stream endpoint remains `/api/capture/stream/<capture_id>`. The backend owns the Mac PCAP path reported by the command/API; browser reloads do not restart capture.
