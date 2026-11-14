# Tuya backend source tests

`python tests/chat_cloud/run_tests.py` compiles and executes the actual
`firmware/src/homehub_chat_cloud.c`, with deterministic SDK/RTOS/audio fakes.
The tests do not copy the backend state machine. The fake mutex checks ensure
blocking SDK calls and every audio sink call occur without the cloud lock.
`red-result.txt`, `offline-red-result.txt`, and `ai-loss-red-result.txt` record
the observed behavioral failures before their corresponding implementations.
Additional review regressions are recorded in `independent-review-red-result.txt`,
`callback-barrier-red-result.txt`, `sdk-upload-stop-red-result.txt`, and
`network-local-quiesce-red-result.txt`.

Offline memory regressions are recorded in `offline-memory-red-result.txt`
and `net-start-red-result.txt`. The tests assert that an unauthorized device
with no SSID or HTTP configuration initializes only the KV needed to inspect
its license, keeps cancellation asynchronous on a PSRAM worker stack, and
skips TLS, register center, timers, work queues, LwIP, netmgr, IoT, player, and
audio FIFO allocation. The bounded FIFO is allocated in PSRAM only when the
authorized AI service is initialized; allocation failure cannot become READY.
The microphone producer submits 640 bytes every 20 ms. The FIFO now has 384
slots with four reserved for ordered controls: about 7.6 seconds of PCM backlog,
compared with 0.8 seconds previously. `fifo-budget-red.txt` and
`fifo-budget-green.txt` record actual-backend tests injecting 300 microphone
frames during a six-second synchronous START or upload stall. They verify all
bytes and timestamps, submission at the data limit, and cancellation during the
stall. This covers the modeled pause, not sustained slow network throughput.
One worker-side budget log per completed/failed turn reports queue high water
and maximum START/PCM call times without logging audio, text, or session IDs.
The actual HTTP source is also compiled to verify that empty/incomplete/invalid
HTTP configuration creates no queue or worker, while valid HTTP configuration
does not depend on an explicit SSID. These checks do not measure native heap
on the board or prove that FatFs recording saves have recovered.

`kv-compat-red-result.txt` records the failed KV-compatibility and safe-log
regressions before the repair. The runner extracts the public KV parameters
from the installed SDK's `your_chat_bot` example into an ignored generated
header. The actual backend's `tal_kv_init` arguments must match that example;
the test output never contains either parameter. Authorization diagnostics
are checked to contain only return status and field lengths, and a failed
read must not inspect any returned field pointers. This test uses synthetic
licenses and does not prove that a real board has recoverable credentials.

`auth-cli-red-result.txt` records the missing authorization-shell behavior.
Only a successful KV initialization followed by an absent or malformed license
may enable UART0 plus the SDK authorization commands. A valid stored license
keeps that channel closed even if product configuration is missing. Disabled
chat and KV errors do not initialize the shell; the full system/filesystem CLI
is never initialized. UART and command-registration failures are logged as
return codes and leave local-only operation and asynchronous quiescence intact.
These tests do not operate a serial port or import any device credentials.

`python tests/chat_cloud/check_arm_api.py` checks this translation unit against
the installed SDK headers with the existing ARM build flags and `-fsyntax-only`.
It does not build the SDK, link an image, or touch serial. The complete ARM
integration build remains separate.

The test SDK does not decode MP3, perform cloud authorization, establish BLE/AP,
or exercise live cloud traffic. EOF and STOPPED, sink errors, queue/cancellation
ordering, and borrowed-buffer lifetime are independently driven by the fakes.

The SDK's normal media callbacks omit EID. This backend uses raw per-session
receive-channel callbacks, deletes the old session before releasing a turn,
creates fresh receive-channel IDs for the next session, checks EID/SID on raw
events, and checks the optional audio `session_id_list`. Raw callbacks capture
the turn before copying data into the bounded FIFO. Old queued turns, old
explicit SID/EID, and post-cancel media are rejected by the tests.

There remains an SDK boundary: `svc_ai_basic/src/tuya_ai_biz.c` caches its raw
callback for fragment continuations (around lines 1307-1314), while continuation
metadata has no EID or receive-channel ID. A hypothetical stale continuation
without the optional SID cannot be positively attributed through the public
raw callback. These tests do not prove that scenario safe on live cloud traffic;
real authorization, repeated turns, forced reconnects, and acoustic tail drain
remain hardware/cloud acceptance work. No live success is asserted here.

Upload lifecycle uses the SDK's synchronous agent functions declared in
`svc_ai_agent/src/tuya_ai_internal.h:50-68`. The HomeHub worker calls
`tuya_ai_agent_start`, then `tuya_ai_agent_upload_stream` with an audio header
whose timestamp is milliseconds and PTS is microseconds. With
`codec_enable=false`, `tuya_ai_agent.c:1525-1566` directly calls the stream
sender. The SDK's hidden `tuya_ai_input_start/stop` state machine is never
started or fed by this backend.

The SDK's `tuya_ai_agent_end` discards nested send errors in
`tuya_ai_agent.c:1373-1396`, so this backend explicitly checks the PCM EOF stream
send and `tuya_ai_agent_event(AI_EVENT_END, 0)` result instead. Network errors
produce FAILED, seal the closing gate, discard unsent FIFO commands, attempt a
break, and delete the local session route. The SDK removes its local session
and channel mapping before its cloud close request
(`tuya_ai_agent.c:1104-1127`, `tuya_ai_biz.c:1145-1181`). Once all already-entered
callbacks have left, the player is confirmed STOPPED, and the audio owner
confirms quiescence, QUIESCED(true) returns the local hardware to family
messages. This does not claim that the remote END was acknowledged. A new chat
requires successful session creation with all readiness layers still valid.
