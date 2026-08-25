# Spike A — TX round-trip latency

> Plan [§9](../../docs/PLAN.md): *"The smallest possible SDK harness: init,
> auth, join, `setExternalAudioSource`, push a tone, and measure the delay to a
> second Zoom client."*
>
> **Kill criterion: if one-way latency exceeds ~250 ms, the Zoom-transport
> intercom thesis is dead for live crew use.** Phase 1 still ships as
> producer-to-guest talkback, but Phase 3 moves to the front of the queue.

This is Phase 0 work. It is not Phase 1, it has no UI, no channels and no IPC
layer, and it links the Meeting SDK directly in-process per plan §3.1.

## What it measures

One-way latency from the instant this app hands a frame to Zoom's virtual mic,
to the instant that audio comes back out of a second Zoom client.

```
  ZComms harness (this process)                    second Zoom client
  ┌───────────────────────────────┐                ┌──────────────────┐
  │ generator ─► ring ─► TX pacer │                │  Zoom desktop    │
  │                      (20 ms)  │                │  joined to the   │
  │                         │     │                │  same meeting    │
  │             send() ◄────┘     │                │  MIC MUTED       │
  │               │  t_emit       │                └────────┬─────────┘
  │               ▼               │                         │ renders to
  │        ┌──────────────┐       │   Zoom cloud            ▼
  │        │  Meeting SDK ├───────┼──────────────►  ┌───────────────┐
  │        └──────────────┘       │                 │ output device │
  │                               │                 └───────┬───────┘
  │  probe ◄── loopback capture ◄─┼─────────────────────────┘
  │    │        t_recv            │      WASAPI loopback tap
  └────┼───────────────────────────┘
       ▼
   latency = t_recv − t_emit,   both from the same steady_clock
```

Both ends live in one process, so there is **one clock**. That is the whole
reason the second client runs on this machine: timestamping at the far end
would mean two clocks, and the difference between two unsynchronised wall
clocks is not a measurement.

### How the number is arrived at

- **Emission time** is read immediately before `send()`, not at the tick we
  aimed at and not when the frame was generated. The quantity is *when audio
  entered Zoom*, so it is anchored to the call that does the entering.
- **Arrival time** cannot be read directly — capture buffers are delivered on
  a jittery schedule. Instead the capture stream's sample index is mapped onto
  the same clock by a least-squares fit over a sliding window of
  `(index, host time)` anchors. Callback jitter is zero-mean noise the fit
  averages out; sample-clock drift is a genuinely non-nominal slope the fit
  measures rather than assumes.
- **Detection** is a matched filter: the known chirp is correlated against the
  slice of captured audio that could plausibly contain it. The peak is taken
  on `|correlation|` (a codec round trip can invert polarity) and normalised
  (AGC changes amplitude, not shape). A peak-to-sidelobe gate rejects matches
  that are not clearly above the noise, because a false detection would become
  a fabricated latency sample and quietly corrupt the distribution.
- **Sub-sample interpolation** on the correlation peak, so resolution is not
  quantised to 20.8 µs for no reason.

### What the number includes

It runs from `send()` to the point the far client's audio reaches the OS
mixer. That covers **all of Zoom** — encode, network, cloud, decode, jitter
buffer, and the far client's own render buffering — plus a small amount of
local capture-side plumbing that is ours, not Zoom's.

`--calibrate` measures an upper bound on that local part by running the exact
same paced TX path into a local output device with no Zoom in it. Because the
calibration also contains our own render buffering (which stands in for Zoom's,
and Zoom's is genuinely Zoom latency), it over-estimates. So it brackets rather
than corrects:

```
measured − bias  <  true Zoom latency  ≤  measured
```

Report the bracket, not a single unqualified figure.

## Confidence: `--self-test`

The live run has no ground truth by construction — that is why the spike
exists. So the instrument is checked rather than trusted: `--self-test` pushes
known delays of 45, 150, 275 and 600 ms through the complete
signal → correlation → timebase chain, with a non-nominal capture clock,
milliseconds of anchor jitter and additive noise, and reports the recovered
figure against the truth.

**Run it before believing any live number.** If it does not recover a known
delay to within 2 ms on this machine, nothing the harness says about Zoom is
worth reading.

`spike_a_tests` covers the same ground in CI form, plus the properties that
would otherwise fail silently: amplitude invariance, polarity tolerance,
refusal to match noise, and refusal to confuse an up-chirp with a down-chirp.

## Running it

### 1. Prerequisites

- The Meeting SDK vendored at `third_party/zoom-sdk/` (gitignored, plan §3.5).
  The x64 tree — `h/`, `lib/`, `bin/` — goes directly in that directory.
- Credentials in `local.env` (gitignored). Copy `local.env.example`.
- A second Zoom client on **this machine**, joined to the same meeting.

### 2. Build

```powershell
cmake -S spikes/a-tx-latency -B spikes/a-tx-latency/build -A x64
cmake --build spikes/a-tx-latency/build --config Release
```

Without the SDK present, CMake still configures and builds the core plus tests,
and says so. A fresh clone has no SDK; that is a normal state, not a failure.

### 3. Check the instrument

```powershell
.\build\Release\spike_a_tests.exe
.\build\Release\zcomms_spike_a.exe --self-test
```

### 4. Set up the far end

```powershell
.\build\Release\zcomms_spike_a.exe --list-devices
```

Then, in the second Zoom client:

- **Mute its microphone.** If it is live it picks up the loopback and feeds it
  back into the meeting, and the harness measures a loop rather than a path.
- Set its speaker to the device you intend to tap, and check that device is not
  muted at the OS level.
- Turn off its video. Nothing here needs it.

### 5. Measure

```powershell
.\build\Release\zcomms_spike_a.exe --meeting "<join url or id>" `
    --duration 300 --loopback-device "Speakers" --csv spike-a.csv
```

`--calibrate` first, on the same device, gives the bracket's lower bound.

## Reading the diagnostics

The summary distinguishes failure modes that would otherwise look identical:

| Symptom | Means |
| --- | --- |
| `capture RMS` ≈ 0 | Tapping the wrong device, or the far client is silent/muted. Not a latency result. |
| `no detection` high, RMS healthy | Audio is arriving but no burst matches. Suspect Zoom's noise suppression eating the signal, or the far client applying processing. |
| `data gap` high | The harness fell behind; capture aged out before it was searched. |
| `TX gated ticks` high | `onMicStartSend` never opened, or Zoom muted us. Check the participant list — a host "mute all" silences the harness with no fix on our side (plan §2). |
| `TX underruns` high | The generator is not keeping the ring fed. |
| `TX tick lateness` large | The 20 ms grid is not being held; the machine is too loaded to trust the run. |

## What this seeds

Per plan §9 this harness is not throwaway. `ZoomMicSource` (§6.1), the frame
ring (§6.2) and the paced TX thread carry the non-negotiable behaviours
forward: fixed cadence, counted underruns, gated sends, ramped edges.

One deliberate deviation is recorded in `frame_ring.h`: the ring is
mutex-guarded rather than lock-free. Drop-oldest makes the producer a writer of
the read index, which needs slot versioning to be safe lock-free, and at 50
pushes/second a correctly-boring mutex is worth more than a subtly wrong
lock-free ring in the code that takes the measurement. §6.1's real component
should revisit that under real capture load.

## Not in scope

RX (`onOneWayAudioRawDataReceived`) is not used. Spike A only needs the send
path, and raw audio RX generally requires recording privilege from the host —
a wall worth not hitting for a number that does not need it. Spike B is where
mute policy and identity get examined.
