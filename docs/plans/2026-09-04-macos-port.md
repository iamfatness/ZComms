# ZComms on macOS — design

**Status:** design approved 2026-09-04. Implementation plan not yet written.
**Goal:** ZComms runs on Apple Silicon Macs as a signed, notarized, downloadable
app with feature parity to the Windows product.

This is a design document. Sections are numbered because CLAUDE.md and the code
cross-reference them (`plan §2`, `§6.1`).

---

## §1 Scope, and what "done" means

Done is: **a Mac user downloads ZComms, opens it without fighting Gatekeeper,
signs in with Zoom, joins a meeting, keys a channel, and a panelist hears them.**
Not "it builds here." Signing and notarization are in scope.

Target is **Apple Silicon only** (arm64). The Zoom macOS SDK is universal
(`x86_64 arm64`), so Intel is technically available and was considered and
declined: there is no Intel Mac here to verify on, and shipping an untested
slice is worse than not shipping it. Universal2 later is a build-config change,
not a redesign — the door stays open.

**This port is independent of the CoreVideo macOS talkback engine port**
(`CoreVideo/docs/superpowers/plans/2026-09-04-macos-talkback-engine-port.md`).
The two products solve the same SDK problem twice, deliberately. Sharing a
library was considered and declined: it would couple two release cycles for a
seam that is about 200 lines. The *shape* of that seam is worth copying, and §3
does copy it — the code is not.

**Not in scope:** the shared-intercom roadmap with CoreVideo (CLAUDE.md
2026-08-30), a far-end AEC reference, and any change to Windows behaviour.

---

## §2 What ports unchanged, and why that is most of it

ZComms is ~11,750 lines under `src/`. The overwhelming majority is already
portable, because the platform-specific parts were kept at the edges.

**Ports as-is, no changes expected:**

- All of `src/audio/` except `loopback` and the device-enumeration edges. The
  engine runs on **miniaudio**, which has a CoreAudio backend, and on **speexdsp**,
  which is portable C whose only MSVC flags are already `if(MSVC)`-guarded.
- Most of `src/zoom/`: `duck_plan`, `roster`, `reach`, `room_plan`,
  `signal_protocol`, `signal_outbox`, `breakout`, `chat_signals`, `jwt`. These
  are pure logic and already have unit tests that run without an SDK.
- `src/app/ui_html.h` — all 833 lines. **The panel is one HTML page.** This is
  the single biggest reason this port is cheap: there is no UI framework to
  rewrite, only a host window to replace (§3).

**`loopback.cpp` is deliberately excluded from the macOS build.** WASAPI
loopback has no macOS equivalent without a virtual driver, but it costs nothing
to drop: `engine.cpp` records that the echo canceller currently runs
*reference-starved, i.e. passthrough*, because no true far-end reference is
wired up. Loopback today serves the latency-measurement tools (`zcomms-tap`,
`--calibrate`), not the shipping audio path. **macOS without it behaves exactly
as Windows does today.** If a far-end reference is ever built, it needs a new
design on both platforms.

**`main.cpp` is 2,367 lines but is not the problem it looks like.** Its entire
Win32 surface is `ShellExecuteA` (open a URL or the app window) and `Sleep`.
Both are one-line seams. The real issue in that file is that `Run()` is a single
~1,670-line function — a genuine maintainability problem, but **out of scope
here**. Splitting it during a port would make every Windows regression
ambiguous. It should be its own change, against the CI gate this project builds.

---

## §3 The seams

Six abstract interfaces, two implementations each, selected at build time.
`main.cpp` constructs whichever the build gave it and contains **zero `#ifdef`**.

### §3.1 SDK-facing

| Seam | Today | Windows impl | macOS impl |
|---|---|---|---|
| `ZoomClient` | class inheriting 4 SDK event interfaces (`zoom_client.{h,cpp}`, ~910 ln) | `zoom_client_win.cpp` | `zoom_client_mac.mm` |
| `TalkbackSdk` | `TalkbackChannels : IMeetingTalkbackCtrlEvent`, holding `IMeetingTalkbackController*` | `talkback_sdk_win.cpp` | `talkback_sdk_mac.mm` |
| `VirtualMic` | `ZoomMicSource : IZoomSDKVirtualAudioMicEvent` (`mic_source.h`) | `mic_source_win.cpp` | `mic_source_mac.mm` |

### §3.2 OS-facing

| Seam | Today | macOS |
|---|---|---|
| `ShellWindow` | WebView2 host, 326 ln, already has a `.h` contract | `WKWebView` |
| `SecretStore` | `CryptProtectData` inside `zoom_oauth.cpp` | Keychain (`SecItemAdd` / `SecItemCopyMatching`) |
| `Platform` | `ShellExecuteA`, `Sleep`, app-data paths, `crash_trap`, `diag_log` | `NSWorkspace`, POSIX signals, `~/Library/Application Support/ZComms` |

### §3.3 The `TalkbackSdk` split is the important one

`talkback_channels.cpp` (342 ln) holds the healer, the ~600 ms membership pacing
law, the duck-plan integration, and the retry/backoff. **None of that is
platform-specific and none of it moves.** The class stops inheriting the SDK's
event interface and stops holding `IMeetingTalkbackController*`; it holds a
`TalkbackSdk*` and implements a `TalkbackSdkEvents` callback interface.

**The ladder must never see a raw SDK error code.** Windows spells the rate
limit `SDKERR_TOO_FREQUENT_CALL`; macOS spells it `ZoomSDKError_TooFrequentCall`.
Law 2's backoff turns on exactly that distinction, so the mapping to a single
normalised enum lives in the adapters and nowhere else.

Operations are stated **semantically**, not in the shape either SDK happens to
use. `invite_users()` takes a whole list because that is what the operation
means. Windows spells it as a Begin/Add/Execute batch with mutual-exclusion
rules; macOS spells it as one atomic call. That difference is the Windows
adapter's business and is invisible above the seam.

This is the same shape as the `TalkbackSdk` seam in CoreVideo's plan of the same
date. Independent code, borrowed design — the abstraction is already proven to
fit this exact problem on both platforms.

---

## §4 The macOS backends

### §4.1 Operation mapping — every call is 1:1 or simpler

| ZComms needs | Windows | macOS |
|---|---|---|
| capability probe | `IsMeetingSupportTalkback` | `isMeetingSupportTalkBack` |
| provision | `CreateChannel(count)` | `createChannel:` |
| add members | Begin/Add/Execute batch | `inviteUsersToChannel:userIDList:` — atomic |
| remove members | Begin/Add/Execute batch | `removeUsersFromChannel:userIDList:` — atomic |
| tear down | batch | `destroyChannels:` |
| send audio | `SendAudioDataToChannel` | `sendAudioDataToChannel:audioData:dataLength:sampleRate:channel:` |
| duck | `SetChannelBackgroundVolume` | `setChannelBackgroundVolume:backgroundVolume:` |
| virtual mic | `setExternalAudioSource` + 4-callback lifecycle | same, via `ZoomSDKVirtualAudioMicDelegate` |

Verified against `ZoomSDK.framework/Versions/A/Headers/` locally. The header
documents the same limits as Windows: **max 16 channels, max 10 users per
channel**, `dataLength` a multiple of 2, PCM 16-bit, 32 kHz or 48 kHz,
background volume 0.0–2.0.

Every other capability ZComms uses has a macOS controller:
`ZoomSDKNewBreakoutRoomController`, `ZoomSDKMeetingChatController`,
`ZoomSDKWaitingRoomController`, `ZoomSDKMeetingActionController` (host/co-host,
admit). **There is no ZComms feature the macOS SDK cannot express.**

macOS is strictly *easier* than Windows here: the Begin/Add/Execute
mutual-exclusion rules that produced a Major on the Windows side have no
analogue.

### §4.2 Threading — the one genuine unknown

**There is no threading guidance anywhere in the macOS SDK headers.** All of
them were grepped for `main thread`, `main queue`, `dispatch_`, `thread-safe`,
`must be called on`. Nothing.

What is known:

- **Windows:** one thread calls `InitSDK` and pumps the message queue; SDK
  callbacks arrive on it. A separate TX thread on a fixed 20 ms clock is the
  only caller of `send()` (`zoom_client.h`, and CLAUDE.md §5's rule about never
  running media on the control thread).
- **macOS:** the equivalent of the message pump is the main run loop.
  CoreVideo's port concluded that membership calls there are main-queue-only.

What is **not** known is whether `sendAudioDataToChannel:` is also
main-queue-only. That single fact forks the design:

- *Free-threaded* — the existing TX pacer calls it directly. No change.
- *Main-queue-only* — the 20 ms pacer must hand frames across, 50 dispatches per
  second, contending with `WKWebView` on that same queue. That is a real jitter
  risk, and the mitigation is a dedicated `CFRunLoop` thread owning the SDK
  rather than the app's main queue.

**This is not guessed. P1 answers it empirically before any of P2 is written**
(§7), and it is cheap to answer: send from the pacer thread and see whether
audio arrives or the SDK refuses.

Three smaller unknowns P1 also settles:

- Whether `Authenticate`'s blocking-while-pumping pattern survives, or needs a
  nested `CFRunLoop` or an async rework.
- Whether the header's "mono or stereo" claim is the same lie it is on Windows
  (Law 5: `ZoomSDKAudioChannel_Stereo` returns success and delivers nothing
  audible). We downmix to mono at the boundary regardless, so this is
  confirmation, not a dependency.
- Whether the ~600 ms membership rate limit holds at the same interval. §4.3
  expects it to, because it is server behaviour — but "expected" is not
  "measured", and the ladder's backoff is built on the number. P1 measures it.

### §4.3 What does not change

The delivery laws (CLAUDE.md, 2026-08-29) are **server behaviour, not API
shape**, and are expected to port unchanged: talkback delivers only while this
client's meeting audio is open; talkback does not cross breakout rooms; Zoom
ducks channel members by default; the membership rate limit. The adapters
normalise the error codes; the ladder keys its backoff on the normalised enum
exactly as it does today.

---

## §5 Build, test, CI

### §5.1 CMake

The existing layering already supports this: `speexdsp` → `zcomms_audio` →
`zcomms_zoom` (already conditionally built on SDK presence, with a graceful
"engine only" fallback) → `zcomms`. Five changes, no restructuring:

1. `speexdsp` — expected to build as-is.
2. `zcomms_audio` — link `CoreAudio`, `AudioToolbox`, `CoreFoundation` for
   miniaudio's CoreAudio backend; drop `loopback.cpp` on Apple (§2).
3. `zcomms_zoom` — the gate is currently `EXISTS ${ZOOM_SDK_DIR}/lib/sdk.lib`;
   add the macOS arm against `ZoomSDK.framework`, and compile the `.mm` adapters.
4. `zcomms` — `MACOSX_BUNDLE`; link `Cocoa`, `WebKit`, `Security`; embed and
   rpath `ZoomSDK.framework`.
5. New `zcomms_zoom_tests` target linking fakes against the six seams, buildable
   on **both** platforms.

### §5.2 The testing payoff

`tests/` today covers `envelope`, `limiter`, `frame_ring`, `aec`, `signal_gate`,
`channel_mix`, `extern_feed`, `duck_plan`, `reach`, `room_plan`,
`signal_protocol`, `signal_outbox`, `crash_trap`, `diag`.

**There is no test for `talkback_channels.cpp`** — the healer, the pacing law,
the retry/backoff, the channel ladder. The most safety-critical code in the
product is untested, and it is untested *because* it inherits
`IMeetingTalkbackCtrlEvent` and cannot be constructed without the Windows SDK.

The seam removes exactly that obstacle, and a `FakeTalkbackSdk` can then pin the
laws that were found live and currently have nothing holding them:

- Law 2 pacing spaces membership calls ~600 ms, round-robin.
- A `TooFrequent` refusal retries **the same** item and does not advance.
- `AlreadyExists` counts as confirmed presence and is never retried.
- The healer skips cross-room invites (Law 2, breakout).
- Duck is unity-at-create, 0.2 only while that channel is keyed.

Per house rule, **every new pin is mutation-proved**: break the thing, watch the
test fail, revert. Tests pin invariants, not implementations.

Second-order benefit: once the fakes implement these interfaces rather than
Zoom's Windows headers, **the suite builds and runs on the Mac** — the machine
the work actually happens on.

### §5.3 CI — there is none today

`.github/workflows/` does not exist. That matters because seam extraction edits
Windows code on a machine with no Windows toolchain. CoreVideo's port could lean
on "Windows is still green because the PR check passed"; ZComms has no such
check, so **building it is P0** (§7).

- **`windows.yml`** — `windows-latest`; `gh release download` the Windows SDK
  from the private release asset (CLAUDE.md Known Gates: the SDK is gitignored
  and fetched at build time); configure, build, `ctest`.
- **`macos.yml`** — `macos-14` arm64, same shape against a macOS SDK asset.
  Extended in P4 with signing and notarization.

**Prerequisite:** the macOS Zoom SDK must be uploaded as a release asset
alongside the Windows one. It exists locally at `~/Developer/zoom-sdk-macos`, so
this is a task, not a blocker — but macOS CI cannot build until it is done.

---

## §6 Packaging and shipping

The template already exists: CoreVideo's `scripts/make-macos-bundle.sh`
(commit `b7f9afc`). Same bones — embed `ZoomSDK.framework` into
`Contents/Frameworks`, strip build-tree rpaths and add exactly one
`@executable_path/../Frameworks` via `install_name_tool`, sign inside-out,
`--options runtime --timestamp`, `notarytool submit` a zip, `stapler staple`
then `validate`.

**Entitlements** — CoreVideo's set minus the camera:

```
com.apple.security.cs.disable-library-validation   REQUIRED
com.apple.security.device.audio-input              REQUIRED
com.apple.security.device.camera                   dropped — ZComms has no video
```

`disable-library-validation` is not optional: the Zoom SDK loads its own bundles
(`airhost.app`, `annoter.bundle`, `libzContext.dylib`, …) that are not signed by
this team, and the hardened runtime rejects them without it.

**`Info.plist` must carry `NSMicrophoneUsageDescription`.** Without it the app is
not prompted for permission — it is killed the moment it touches the mic.

**OAuth is simpler on macOS than it was on CoreVideo.** ZComms uses an RFC 8252
loopback redirect — `control_server` serves `GET /oauth/callback` on a local
port. There is no custom URL scheme to register and no equivalent of
`CoreVideoOAuthCallback.app` to build, embed and sign. Only token *storage*
changes: DPAPI → Keychain, behind `SecretStore`.

**Signing is mandatory here in a way it is not on Windows.** The Windows build
ships unsigned and users click through SmartScreen. Gatekeeper is a wall, not a
warning: an unsigned, unnotarized download is unusable for an ordinary operator.
macOS will therefore be the **first signed ZComms**.

**The Zoom Marketplace app is shared with CoreVideo and is already approved by
Zoom** (owner, 2026-09-04). Splitting ZComms onto its own identity is wanted
eventually but is not a gate for this work. CLAUDE.md's Known Gates entry should
be updated to say so.

**Assumption to confirm at P4:** the Developer ID used for CoreVideo's signing
covers ZComms (same team). If it does not, an Apple Developer account is a
lead-time dependency and P4 must start earlier.

---

## §7 Phasing and gates

Sequencing is **vertical slice first**: the smallest real thing that proves
talkback works on macOS, built on the actual seams so none of it is disposable.

**This design is one document; it is deliberately more than one implementation
plan.** P0+P1 is the first plan and the natural unit — it ends at a falsifiable
claim ("someone heard it") and everything in it is invalidated together if the
threading answer in §4.2 goes the wrong way. P2, P3 and P4 each become their own
plan, written *after* the phase before it lands, so each is informed by what the
last one learned. This mirrors how the CoreVideo show-engine port was run as
stacked plans rather than one long document.

### P0 — the gate, before anything moves

`windows.yml` green. Upload the macOS SDK as a release asset.

**Exit:** a green Windows check on a PR, *and* a deliberately broken commit
showing red. Mutation-prove the gate itself or it is decoration.

### P1 — smallest audible path

Extract the three SDK seams (§3.1). The Windows side is **move-only — no logic
changes**, with CI as the safety net. Write the `.mm` backends. Add enough CMake
to build a headless probe in the `tools/engine-cli` style: no UI, no OAuth,
meeting and JWT from env. Add `macos.yml`. Write the first `FakeTalkbackSdk`
tests (§5.2).

**Exit:** a real Zoom client on another device **hears audio keyed from the
Mac**, plus written answers to the §4.2 unknowns.

### P2 — a real app

`ShellWindow` → `WKWebView`. `SecretStore` → Keychain. `Platform`. `main.cpp`
de-Win32'd (`ShellExecuteA`, `Sleep`).

**Exit:** `ZComms.app` launches, shows the panel, signs in, joins, and keys a
channel from the UI.

### P3 — parity

Extern feeds over multichannel CoreAudio, breakout awareness, chat signalling,
EDIT TALENT, settings, device pickers, test tone, the diagnostic stream and both
watchdogs.

**Exit:** a feature-by-feature checklist against the Windows product, and the
delivery laws re-verified on macOS.

### P4 — ship

Bundle script, entitlements, `Info.plist`, sign, notarize, staple; CI extended
to sign on tag.

**Exit:** a Mac user downloads it and it opens.

---

## §8 Risks

| Risk | Severity | Answered |
|---|---|---|
| `sendAudioDataToChannel:` is main-queue-only → 50 dispatches/sec contending with `WKWebView` | High | **P1, before P2 is written.** Mitigation: dedicated `CFRunLoop` thread owning the SDK. |
| Seam extraction breaks Windows and nobody notices | High | P0 exists precisely for this. Windows edits are move-only. |
| `Authenticate`'s blocking-pump pattern does not survive | Medium | P1. Nested `CFRunLoop` or async rework. |
| Multichannel CoreAudio enumeration differs from WASAPI | Medium | P3. Extern feeds are the affected feature. |
| Zoom SDK framework loading under hardened runtime | Medium | P4, but `disable-library-validation` is the known answer from CoreVideo. |
| Breakout behaviour on macOS | Low | **Not live-verified on Windows either** (Law 2). macOS inherits that gap and is not held to a higher bar than the shipping product. |
| macOS SDK raw-data entitlement differs by auth tier | Low | Law 1 records that broker-JWT auth carries it and bare public-app-key did not. Same broker, so expected to hold; P1 confirms. |

**Standing policy (CLAUDE.md): never assert a branch unreachable.** Two Majors
in the Windows talkback feature lived behind exactly that claim.

---

## §9 Decisions

| Decision | Rationale |
|---|---|
| Independent of CoreVideo's talkback port | Sharing couples two release cycles for ~200 lines of seam. Copy the shape, not the code. |
| Full seam extraction, not `#ifdef` | Keeps `main.cpp` free of scar tissue and makes the SDK layer testable for the first time (§5.2). |
| Windows CI before any code moves | No Windows toolchain on this machine; without a gate, a seam refactor is unverifiable. |
| Vertical slice first | Retires the threading unknown on ~day 4 with code that is kept, rather than a throwaway spike or a late discovery. |
| Apple Silicon only | No Intel hardware to verify on; an untested slice is worse than no slice. Universal2 remains a build-config change later. |
| `talkback_channels` tests in scope | The seam is what makes them possible, and the laws they pin were found the expensive way. |
| `Run()` split **out** of scope | Would make every Windows regression ambiguous during a port. Its own change, against the P0 gate. |
