# CLAUDE.md

Project notes for Claude Code sessions working in this repository: **ZComms**,
a standalone intercom built on the Zoom Meeting SDK. Update this file in the
same change as any substantive work — docs-updated is part of done.

## Where things stand

Two things exist: the **Spike A harness** (`spikes/a-tx-latency/`) and the
**audio engine core** (`src/audio/`, plan §6.1–6.3). There is no app shell, no
UI, no channels, no admin, no packaging, and Phase 1 has not started. Two
spikes still carry kill criteria that can invalidate the architecture.

Before writing code, read `docs/PLAN.md` end to end. It exists specifically to
stop work starting in the wrong place.

### The audio engine (`src/audio/`)

Built and verified on real hardware. The chain, and the order is load-bearing:

```
capture -> input gain -> limiter -> PTT envelope -> sidetone tap
        -> 20 ms frames -> ring -> TX pacer -> FrameSink
```

- The limiter sits **before** the PTT envelope so gain reduction does not pump
  against the ramp depending on whether PTT is held.
- The sidetone tap is **after** the envelope, so it monitors what is actually
  being sent. Silence while not transmitting is correct, not a bug.
- `FrameSink` is the seam. `ZoomMicSource` is one implementation; `WavSink` is
  the other, and it is what lets the whole engine be exercised with no meeting
  and no SDK. Use it — the properties that matter here are properties of a
  waveform.

Verified: 22/22 unit tests; on a real GoXLR, 363 ticks / 363 sends /
0 underruns / 0 ring drops, 20 ms grid held to 0.38 ms worst lateness, and a
scripted PTT cycle produces clean gating in the recorded WAV with no clipping.

`zcomms-engine --help` drives all of it. `--ptt-cycle <on,off>` scripts the
press/release pattern so ramp behaviour is inspectable without a person
holding a key.

### zcomms — the talkback panel (Phase 1 slice), END-TO-END VERIFIED

`zcomms.exe` (src/app) is the product's core loop, live-verified 2026-08-27
in a **fully automated** run: joined the meeting, was made host, created the
channel, **auto-admitted** the listener from the waiting room
(`AdmitAllToMeeting`), auto-invited it, and the 700/1000 Hz test signal was
detected at the listening client's render endpoint at ~10^7:1 dominance
(`zcomms-tap`, Goertzel across every playback endpoint).

Operational truths that run established, beyond Spike A's list:

- **The Zoom web client cannot receive talkback** (`IUserInfo::
  IsSupportTalkback()` = false; inviting it fails INVALID_PARAMETER). The
  roster carries the flag and the app skips and says so. Panelists must be on
  a native client.
- **The web client's host menu has no "Make Co-Host"** — only "Make Host".
  Host works fine for channel creation, and zcomms-as-host is the smoothest
  operational shape: it admits its own listeners.
- Verification without ears: `zcomms --latch --test-signal` transmits a
  700/1000 Hz beep pattern through the same ring/pacer as live audio;
  `zcomms-tap` finds it. This pair is the repeatable e2e test.

### Multi-channel: ROUTING PRIVACY VERIFIED LIVE (2026-08-29)

The intercom's core claim proven against a real meeting with the
matched-filter e2e: with a native listener confirmed on CH 1 —

- **CH 1 keyed** → probe detected at the listener: 7/8 bursts, correlation
  peak **0.978**, PSR 5.8.
- **CH 2 keyed only** (8 s of audio transmitted into it throughout) → the
  CH 1 listener heard **nothing**: 0 bursts, peak 0.270 / PSR 1.2 = floor.

Same transmitter, listener, bus; only the key differed. Channel isolation is
real. Re-run any time: bring up `zcomms --channels 2 --test-signal`, then
`POST /act "talk <slot> on|off"` + `zcomms-tap` per phase — no silence
needed since the chirp/matched-filter rework.

Open defects (2026-08-28), still standing:

- **zcomms main-thread hang (AppHangB1), one occurrence.** ~40 s after a
  phase that sent 8 s of audio into an **empty** channel, the main loop
  stopped pumping and Windows killed the process. Not reproduced (2026-08-29's
  phase B also sent into an empty-of-listeners keyed channel without
  incident). Instrument before trusting long unattended runs.
- ~~zcomms-tap Goertzel fragility~~ — fixed 2026-08-28: chirp probe +
  matched-filter/PSR detection; field-proven both directions on a noisy bus.

Also observed: a dead web session can linger as a **ghost participant
holding host** for ~1–2 min; it can even kick joiners. Wait it out — host
auto-reassigns (twice it landed on zcomms itself, which the retry loop turns
into a win).

### Spike A — RESULT: the thesis survives

Measured live 2026-08-26 against a real meeting, over the **talkback
transport** (`IMeetingTalkbackController::SendAudioDataToChannel`, which the
product now prefers — see below):

- **median 165.0 ms, p95 193.6 ms** one-way, harness → channel → a plain
  Zoom client's output. 55 samples, MAD 4.2 ms, jitter (p95−p50) 28.6 ms.
- Both inside plan §9's ~250 ms kill criterion. Bracketed against the 33.2 ms
  local bias: true Zoom-path median in (132, 165] ms.

Established alongside the number, each live-verified:

- **The product's TX path is talkback channels, not the virtual mic.** The
  owner's goal is "talk to the panelists inside the client's meeting";
  `IMeetingTalkbackController` (SDK ≥7.0) does channel routing *inside* one
  meeting — which supersedes plan §1's premise that routing cannot exist
  there. Max 16 channels, 10 listeners each, per-channel meeting ducking.
- **Talkback works under PKCE public-app-key auth.** No JWT, no client
  secret, no raw-data license. The virtual mic's send window never opened
  under this auth (`HasRawdataLicense()` false); if the party-line/virtual-mic
  case ever matters, expect it to need JWT auth.
- **Channel creation needs co-host role** (`SDKERR_NO_PERMISSION`, 12, as a
  guest; works seconds after promotion). No account entitlement beyond that —
  `IsMeetingSupportTalkBack()` was true on an ordinary account.
- **A plain Zoom client hears channel audio with no app installed.**
- **The receiving client renders talkback to the default-communications
  endpoint** (the GoXLR "Music" bus on this machine), *not* its configured
  speaker device. Cost several runs; the harness's `--tap-all` mode (taps
  every playback endpoint at once, each with its own correlator) exists
  because of it and ends that class of failure permanently.

Verified before the live run:

- `spike_a_tests` — 22/22.
- `--self-test` recovers known delays of 45/150/275/600 ms to within 0.1 ms
  through the full signal → correlation → timebase chain.
- `--calibrate` — 13/13 bursts resolved, 0 underruns, 0 gated ticks, 20 ms grid
  held to 0.01 ms mean lateness. Local render+loopback bias **33.2 ms**, which
  is the lower bound of the bracket any Zoom figure gets reported against.
- `--check-auth` — `AUTHRET_SUCCESS`.

**Do not quote a latency number from anywhere until a live run produces one.**
Run `--self-test` before believing any live figure; the live run has no ground
truth by construction, so that is the only place the instrument gets checked.

### The panel is a native window (2026-08-29)

`src/app/shell_window.cpp` hosts the panel in **WebView2** on a dedicated STA
thread (the main thread pumps the Zoom SDK and must never own a UI message
loop). Client area is exactly the panel's designed 1000x640; closing the
window quits the app (`on_closed` -> `quit_req`). The SDK is fetched at
configure time from NuGet into `build/webview2/` (loader DLL staged beside
the exe); if the download or the runtime is absent the app falls back to the
old Edge/Chrome `--app` window. This is also the Mac-port shape: same panel
HTML, WKWebView shell.

### Direct talk + the invite rate limit (2026-08-29, live 12-person meeting)

**Zoom rate-limits back-to-back talkback calls (code 18) on INVITES too**,
not just CreateChannel: the healer's one-batch-exchange-per-person every 2s
drew `SDKERR_TOO_FREQUENT_CALL` on every pass. Rules now enforced in code:
one `InviteMany` batch per channel per pass, per-(channel,person) backoff
(5s doubling to 60s; 10s patience after a successful Execute for the async
confirmation), prune intent/backoff when a person leaves (ids recycle).

**The direct-talk model:** the full 16-channel bank is provisioned up front
in ONE CreateChannel(16) request (keying must only SELECT); each capable
participant auto-lands on their OWN channel so their key wears their name;
spillover past 16 goes to the least-loaded channel; a partial grant proceeds
after three rounds. Panel: ALL CALL + LATCH ALL (verbs `talkall`/`latchall`),
empty spares hidden, chips = in-use channels + one spare.

### Signed-in joins: the CoreVideo auth pattern (2026-08-29)

Owner rule: **no anonymous joins** -- ZComms follows CoreVideo's auth shape.
`src/app/zoom_oauth.{h,cpp}` drives the CoreVideo OAuth broker
(`corevideo.iamfatness.us`, the CF Worker in the CoreVideo repo's
`site-worker.js`): browser -> `/oauth/start?state&return_uri` -> Zoom consent
-> broker -> **loopback callback** `http://127.0.0.1:7350/oauth/callback`
(served by ControlServer; RFC 8252 -- no protocol registration, unlike the
OBS plugin's `corevideo://`+helper-exe shape) -> `/oauth/redeem` ->
access/refresh tokens (DPAPI at rest, `%APPDATA%\ZComms\zoom-tokens.bin`).
Joins then use `/oauth/sdk-jwt` for `AuthenticateWithJwt` + the Zoom API ZAK
(`/v2/users/me/token?type=zak`) on `JoinParam.userZAK`. The broker's
return-uri allowlist gained the loopback form in CoreVideo PR #233 (deployed;
additive). Refresh tokens rotate and revoke -- `invalid_grant` clears the
store and re-asks, never retries. Panel: `signin` phase = the join card as a
SIGN IN card; verbs `signin`/`signout`. `--anon` keeps the old public-app-key
guest join for scripted same-account runs (headless without a session errors
loudly -- sign-in needs the panel's callback server).

### The app identity's join boundary (2026-08-29, live-diagnosed)

**Under the PKCE public-app-key identity, ZComms can only guest-join meetings
hosted by the Zoom account that authorized the app.** A cross-account meeting
fails with `MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING` (504) -- decoded
into operator language now. This is an auth-tier property, not a bug; it
gates who can use ZComms against whose meetings until the app has its own
Marketplace identity (§3.7) with guest join approved. Test meetings must be
started from the authorizing account.

Related traps fixed the same session (`zoom_client.cpp`):

- **FAILED's code gets clobbered by the ENDED that follows it** (ENDED
  carries result 0). Latch the FAILED code; report it from the ENDED branch.
- **`IMeetingConfigurationEvent` must be implemented.** A passcode-protected
  meeting joined by bare ID asks for the passcode via
  onInputMeetingPasswordAndScreenNameNotification; with no listener the join
  dies opaquely. The panel's join card doubles as the passcode prompt
  (`/act "passcode <v>"`, wrong-passcode retry included); the name+email
  prompt (onJoinMeetingNeedUserInfo) is answered inline.
- `Join()` takes an `on_tick`: the panel mirrors WAITING_FOR_HOST /
  IN_WAITING_ROOM / passcode states while Join blocks.

### The app outlives its meetings (2026-08-29)

`Run()` is a session cycle: join card → meeting session (a lambda) → back to
the join card on any failure or when the meeting ends; only QUIT / a scripted
`--run-seconds` exits. Failures `log_op` into the panel's ops ticker instead
of exiting — the live bug that forced this: InitSDK error 14 (OBS's
ZoomObsEngine held the SDK) exited the process, which from the operator's
chair looked like "pasted a link and it froze". `SdkConflictHint()` names the
conflicting process by scanning for known SDK hosts. Per-meeting state
(`intent`, `auto_assigned`, `warned_no_talkback`, housekeeping timer) is
session-scoped, never static — user ids are meeting-scoped and recycled.

### Things that cost real time here, worth not rediscovering

- **The SDK refuses to initialise if another process holds one.** `InitSDK`
  returned `SDKERR_OTHER_SDK_INSTANCE_RUNNING` (14) purely because an
  unrelated app's Zoom engine was running. Kill other SDK hosts first. This
  also points at plan §3.2: the singleton may be wider than per-process, which
  would change Phase 2's worker model. **Spike C should test it directly.**
- **`sdk.dll` crashes the process on exit.** Returning from `main` reaches its
  `DLL_PROCESS_DETACH`, which throws under the loader lock and fastfails
  (`0xC0000409`). It fires even if `InitSDK` was never called. See `HardExit()`
  in `spikes/a-tx-latency/src/main.cpp`.
- **Zoom SDK headers require `windows.h` first** — they use `HWND`/`RECT`/
  `UINT64` bare.
- **`setvbuf(_IOLBF)` is a no-op on Win32.** Use `_IONBF`, or a long run's
  progress output stays invisible until it exits.
- **A waiting room blocks a guest SDK client indefinitely.** The harness joins
  as `SDK_UT_WITHOUT_LOGIN` and lands in the waiting room; someone has to
  click admit. Turn the waiting room off before an unattended run, and watch
  the 40-minute limit on basic accounts — it ends the meeting underneath you.

## What this project is not

ZComms is a **standalone product with no dependencies on any other codebase**.
It links the Zoom Meeting SDK directly and owns its whole stack: SDK
integration, audio engine, UI, sign-in, packaging.

In particular there is **no helper process and no IPC layer**. The
two-process designs common in this space exist because the media consumer is a
plugin inside somebody else's application; ZComms has no host, so it does not
pay that cost. Do not introduce pipes, shared memory or a wire protocol on the
assumption that this is how Zoom media apps are built — see plan §3.1.

Multi-channel needs no workers either: talkback channels live inside one
meeting, so **one SDK client carries all 16** (plan §3.2, post-Spike-A).
Worker-per-meeting exists only as a deferred idea for an operator running two
clients' meetings at once — and the SDK's machine-wide exclusivity may forbid
even that (Spike C).

## Design constraints that are load-bearing

- **Feeding raw PCM into Zoom bypasses its echo cancellation** (plan §2), so
  ZComms carries its own: speexdsp MDF in `src/audio/aec`, first stage on
  every capture frame, referenced against the engine's own monitor output
  (post-fader). Unit-proven: **40.6 dB ERLE** on a synthetic 40 ms / −6 dB
  acoustic path, 0.4 dB near-end impact, exact bypass. On by default;
  `--no-aec` / panel "ECHO CANCEL" toggle. Live-room verification with real
  speakers still worth one session; headsets remain best practice.
- **`send()` is legal only between `onMicStartSend` and `onMicStopSend`.** Hold
  the sender pointer from `onMicInitialize` until `onMicUninitialized` and gate
  every call on the window. Plan §6.1.
- **The TX thread runs on a fixed 20 ms clock, not on events.** Zoom wants a
  steady stream, and a paced puller needs no wakeup protocol at all — which
  removes a whole class of consumed-wakeup bugs before it can exist. Underrun
  is a countable condition: send silence, increment a counter. Plan §6.1.
- **PTT edges ramp, they do not gate.** Zoom's one-way callback can carry true
  zero-valued PCM for hundreds of ms and jumping back to full amplitude is
  audible as a click; a PTT release is deliberately that same transition. Plan
  §5.
- **Timestamps derive from sample counts, never from callback arrival.** Reset
  only on re-subscribe, new session, or rate change. Plan §5.
- **Never run media callbacks inline on the thread handling control messages.**
  Under load that starves audio behind control traffic. Plan §5.
- **Zoom user ids are meeting-scoped and recycled.** Store participants by a
  stable identity, never by user id. Plan §5.
- **Every OS object carries the `ZComms` prefix**, and no binary here is ever
  named so another product's process cleanup could mistake it for its own.
  Plan §3.3.

## Known gates

- The Meeting SDK is not redistributable in a public repo:
  `third_party/zoom-sdk/` is gitignored and fetched at build time from a
  private release asset on this repository. The first CI run fails without it.
- ZComms needs its own Zoom Marketplace app identity, baked in at build time,
  with a broker endpoint so end users never enter app credentials. That is a
  review cycle with lead time — it gates shipping, not building.

## Style

Comments state the constraint the code cannot show. When a change is motivated
by a real failure, say what happened, with numbers. Tests pin invariants, not
implementations.
