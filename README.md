# ZComms

Talkback / IFB for live production inside a Zoom meeting.

ZComms gives a director or producer a desktop comms panel for cueing talent
through Zoom's talkback channels. Talk to one person, build a group line, or
use ALL CALL. Panelists listen through their ordinary native Zoom client;
they do not need ZComms, virtual audio cables, or an additional account.

[Download for Windows](https://github.com/iamfatness/ZComms/releases/latest)
· [Release history](https://github.com/iamfatness/ZComms/releases)
· [Report an issue](https://github.com/iamfatness/ZComms/issues)

## Availability

| Platform | Status |
| --- | --- |
| Windows x64 | Desktop app available. Latest published release: **v0.1.14**, September 2, 2026. |
| macOS | In development. The audio core, shared talkback logic, and macOS SDK adapter have build/test coverage; the desktop app and live-meeting integration are not yet available. |

The Windows release includes a per-user installer and a portable ZIP. The
installer places the app in `%LOCALAPPDATA%\ZComms\app` without requiring
administrator rights and includes the Zoom Meeting SDK runtime. Builds are
currently unsigned, so Windows SmartScreen may prompt on first launch.

## What it does

- **Person-based talk keys.** Eligible participants are assigned to individual
  talkback channels as capacity allows, with their names on the panel.
- **Direct, group, and all-call comms.** Hold a person's key, latch a channel,
  or key the whole bank. EDIT TALENT lets you change channel membership and
  create shared lines. Zoom supports up to 16 channels with 10 listeners each.
- **Extern audio feeds.** Route a single input channel or a stereo pair from
  a multichannel capture device to a talkback channel. Stereo pairs are
  downmixed to mono. Each feed has its own gain, latch, and input meter.
- **Signal-driven ducking.** Active talkback audio reduces the channel
  listeners' meeting-audio level. Operator speech also ducks an extern feed
  beneath the voice; a silent latched feed does not trigger ducking.
- **Breakout-room awareness.** The panel shows participant room information
  and unavailable destinations. The station-room selector lets the operator
  move between rooms when Zoom permissions allow.
- **Microphone controls.** Input gain, look-ahead limiting, ramped talk
  transitions, sidetone monitoring, device selection, and a test tone through
  the microphone processing chain.
- **In-app meeting controls.** Sign in, paste a meeting link or ID, enter a
  passcode when requested, and leave a meeting without closing the app.
- **Operational feedback.** Connection and transmit lamps, input meters,
  channel membership, send counters, and an expandable event history.

Use a headset for monitoring. Talkback is designed for production cues:
repository measurements of the Zoom talkback path recorded approximately
**165 ms median / 194 ms p95** one-way latency in one test setup; actual
latency depends on the devices, network, and meeting.

## Get started

1. Install and open ZComms on Windows. Connect a microphone and headset.
2. Select **SIGN IN** to authorize Zoom in your browser. Normal app use joins
   meetings as your signed-in Zoom account. Stored tokens are encrypted with
   Windows DPAPI under `%APPDATA%\ZComms`.
3. Paste a Zoom meeting link or meeting ID and select **CONNECT**. Enter a
   passcode or wait for admission if the meeting requires it.
4. Have the host make ZComms **host or co-host** so it can create talkback
   channels. The app retries channel setup while you arrange the role.
   When it is host, it also admits participants from the waiting room.
5. Open **SETTINGS** to choose microphone and sidetone output. Check the
   destination assignments in **EDIT TALENT**, then hold a person's key to talk.

Panelists need a native Zoom desktop or mobile client that supports
receiving talkback. The Zoom web client cannot receive it. Talkback also
stays within the station's current room; it does not cross breakout rooms.

On a listener's machine, talkback may use the operating system's default
communications output rather than the speaker selected in Zoom. Check that
output when preparing a multichannel interface for a show.

## Using the panel

| Control | Action |
| --- | --- |
| Hold a person or group cell | Momentary talk to that channel. |
| Digits **1–9** | Key the first nine displayed destinations while the panel has focus. |
| **SPACE** or **ALL CALL** | Momentary talk across the channel bank. |
| **LATCH** | Change cell/all-call presses to toggle transmission on and off. |
| **EDIT TALENT** | Add or remove participants using the numbered channel chips. |
| **SETTINGS** | Select devices, adjust processing, configure feeds, or move the station between rooms. |
| **LEAVE** | Leave the current meeting and return to the connection panel. |

Key colours reflect channel activity and known membership/reach information;
they are not a measurement of sound at a panelist's headphones. The **LINK**,
**MTG MIC**, **CHANNEL**, and **TX** lamps show the station's meeting and
transmit state. Click the status strip to expand its event history.

## Extern feeds

In SETTINGS, choose a **SOURCE**, its input **CHANNEL**, and the destination
under **HEARD BY**. You can use a console bus, a Dante interface, or another
capture device exposed to Windows. Each destination slot can carry one feed;
its controls let you latch, adjust gain, or remove it.

Input meters remain active while the feed is unlatched. They cover
−60…0 dBFS over 12 segments, with the −50 dBFS activity threshold marked.
Below that threshold the system treats the feed as inactive for ducking and
status; this does not mean the audio has been muted. A latched feed can show
`latched · silent` while low-level input is still present.

Feed device selections, channel picks, gain, and latch state are saved in
`%APPDATA%\ZComms\feeds.env` and restored at launch. HEARD BY describes the
current occupants of the destination channel; check those assignments when
changing meetings or reconfiguring a rig.

## Development status

ZComms is early software. The core talkback path, channel isolation, and
breakout-room awareness have been exercised in live meetings. Extern-feed
end-to-end delivery and extended operation, breakout creation/assignment,
and desk-to-desk chat signaling still have live validation work ahead.
Rehearse the intended setup before using it on a production.

Development on `main` since v0.1.14 adds Windows and macOS CI, a shared
`TalkbackSdk` interface, tests for the channel layer, and the macOS talkback
adapter. These are source-tree developments, not a new packaged release.

The local command API also includes breakout administration and chat cue /
assignment-notice commands. These are developer/integration surfaces; they
are not all exposed as controls in the desktop panel. A ready-made
Stream Deck or Companion module is not included.

See [release history](https://github.com/iamfatness/ZComms/releases) for
version-specific changes and [the macOS port plan](docs/plans/2026-09-04-macos-port.md)
for platform work. Planned work includes the macOS desktop app, named
party-line channels, a dedicated ZComms Zoom Marketplace identity, code
signing, and further live qualification.

## Diagnostics and support

Every run writes a log to `%APPDATA%\ZComms\logs`, including launches from a
console. Logs can be read while the app is running, rotate by size, and keep
the last ten runs. The application also records heartbeat and stall
information for troubleshooting.

When [reporting an issue](https://github.com/iamfatness/ZComms/issues), include
the app version, what you were doing, your audio devices, and the relevant
log. Review logs for meeting or participant information before posting them
publicly.

## Building from source

### Windows

Windows x64, a Visual Studio C++ toolchain (Visual Studio 2022 or newer), and
CMake 3.20 or newer are required.

Supply the Windows Zoom Meeting SDK **7.1.5+** under
`third_party/zoom-sdk`, with `lib/sdk.lib`, `h/`, and `bin/` in that directory.
The SDK is not included in the source repository. The WebView2 SDK is fetched
at configure time.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

With the Zoom SDK present, the build includes the `zcomms` desktop app.
Without it, the audio engine, SDK-independent tests, and diagnostic tools
still build. `tools/release.ps1` stages the Windows ZIP and NSIS installer.

### macOS development

The current macOS target is the engine and talkback adapter, not an
installable desktop app. With Xcode command-line tools and CMake installed:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

For the SDK adapter, place the macOS SDK's `ZoomSDK.framework` under
`third_party/zoom-sdk/` before configuring. Without it, the SDK-independent
core and tests remain available. CI currently uses Zoom SDK 7.1.5 on both
platforms.

### Tools and architecture

- `zcomms --list-devices` lists capture/playback devices and reported native
  channel counts.
- `zcomms-engine` exercises the audio engine without a meeting.
- `zcomms-tap` detects test audio on Windows playback endpoints for
  end-to-end verification.
- `spikes/a-tx-latency/` contains the latency measurement harness.

The Windows panel is embedded HTML hosted in WebView2, with a browser-window
fallback. It uses an HTTP control surface on `127.0.0.1:7350`: `GET /` serves
the panel, `GET /events` streams state with SSE, and `POST /act` accepts
one-line commands. This interface is for local controls and integrations.

The audio engine is independent of Zoom; the shared talkback layer uses
platform-specific SDK adapters. See [CLAUDE.md](CLAUDE.md) for engineering
notes and recorded platform behavior, and [docs/PLAN.md](docs/PLAN.md) for the
original architecture rationale. Dated plans include historical designs;
the source and current platform status above describe what is implemented.

## License

ZComms' own source is **MIT** — see [LICENSE](LICENSE).

Bundled or fetched dependencies retain their own terms:

- **Zoom Meeting SDK:** Zoom's license and distribution terms. Supply the
  SDK separately for source builds; Windows release packages include its runtime.
- **speexdsp:** BSD-style license; see [COPYING](third_party/speexdsp/COPYING).
- **miniaudio:** public domain or MIT-0, at your choice.
- **WebView2:** Microsoft's terms; its SDK is fetched during configuration and
  the Windows loader is packaged with the app.

The MIT license covers ZComms source, not the third-party components. Review
those components' terms before redistributing a build.
