# Board storage host fault injection

```powershell
python tests/platform/run_storage.py
python tests/platform/run_storage.py --prove-generation-regression
python tests/platform/run_audio.py
python tests/platform/run_tickets.py
```

`test_storage.c` includes the actual `firmware/src/homehub_storage.c`. A minimal host TKL supplies file, queue, allocation and synchronization failure injection; cJSON entrypoints are inert because configuration parsing is outside these tests. No storage algorithm is rewritten in Python or test C.

`python tests/platform/run_config.py` separately links the installed SDK's real cJSON against that same storage source and host TKL. Six groups verify assembled SR505 defaults without a config or field, explicit PIR true/false, and preserved chat defaults/false. RED and GREEN evidence is saved separately; this does not verify physical input polarity.

The storage suite now has eight groups (the original seven plus ticket propagation):

- Two consecutive rename failures retain the last successful snapshot generation.
- `fsync` failure and `close` failure cannot report successful save.
- Allocation failure and a full snapshot queue return false synchronously, without self-posting a completion event that could deadlock the application consumer. The application shows failure directly.
- A corrupted newest snapshot falls back to the other valid generation.
- WAV byte count, sample rate, channel count and bit depth are checked.
- Tagged snapshot success/failure acknowledgements retain the correct original ticket.

`--prove-generation-regression` compiles a temporary source copy containing the originally observed pre-increment bug. Its snapshot test must fail; the actual production file is never modified. Result logs are `storage-result.txt` and `storage-regression-red-result.txt`.

These are real C tests of board storage control flow. The host filesystem/queue substitutes do not verify Tuya SDK API linking, actual FAT rename durability, SD controller caching, real power loss, concurrency timing, GPIO, microphone or speaker output.

`run_audio.py` also includes the actual `homehub_audio.c` and actual storage source.
Six additional groups check bounded 30-second capture, copying borrowed PCM before
callback return, cancellation and late frames, rejection of an unsupported audio
sample rate, save-sync failure propagation, and speaker write failure. The fake
audio device validates buffer/control flow only; it produces no physical sound
and does not simulate DMA completion or concurrent task scheduling. Its result
is recorded in `audio-result.txt`.

`run_tickets.py` runs only six new persistence-receipt cases. It includes actual
storage source, links actual core source, and automatically extracts the app's
`DeferredReceipt` declaration and five receipt helper functions verbatim from
`homehub_app.c`. The source-slice SHA256 is recorded with the results, so these
are tests of the delivered functions rather than an independently rewritten
state model. Only network dispatch and UI output are captured by test substitutes.

Ticket cases cover successful/failed ACK identity, zero/unrelated ACK rejection,
retained failed receipts and new-ticket retry, stale ACK rejection, queue failure
without release by an ordinary snapshot, shared retry, and restored played-message
polling without replay or a receipt before persistence success. Results are in
`tickets-result.txt`. The snapshot-ticket propagation case is also present in
the standalone storage suite; count it once when reporting unique case groups.
