# macOS port P1-A — carried-forward findings

Everything the P1-A execution and its final whole-branch review found and
deliberately did **not** fix on this branch, with the reasoning. Modeled on
`docs/plans/2026-09-05-macos-port-p0-followups.md`'s format — that doc
carried P0's 15 items into the repo so they would not die with the plan's
scratch ledger; `.superpowers/sdd/2026-09-05-macos-port-p1a-client-seams/
progress.md` is P1-A's equivalent ledger, is likewise untracked, and dies on
merge the same way. This doc is what survives it. None of the items below
blocks merge. They are recorded because the next plan (P1-B, or whatever
scopes P2) will be sized against them.

---

## Behaviour notes

### N1 — the panel's status chip lost five statuses' granularity to `IDLE`

`src/app/main.cpp`'s panel-state line switched from `MeetingStatusName`
(Zoom's raw `MeetingStatus`, 15 cases) to `MeetingStateName` (the seam's
`MeetingState`, 10 cases, `zc::ToState`'s `default:` in
`src/zoom/zoom_client_win.cpp`). Six of the old cases now render as `IDLE`
on the status chip:

- `MEETING_STATUS_DISCONNECTING`
- `MEETING_STATUS_LOCKED`
- `MEETING_STATUS_UNLOCKED`
- `MEETING_STATUS_WEBINAR_PROMOTE`
- `MEETING_STATUS_WEBINAR_DEPROMOTE`
- the `UNKNOWN` fallback (any status `MeetingStatusName` itself doesn't name)

This is a real behavioural change under Task 4's move-only contract, but it
is an unavoidable consequence of `MeetingState`'s shape — a Task 3 decision
the brief endorsed, not something Task 4 introduced or could have avoided
while implementing the mapper `ToState()` specifies.

**Blast radius is corrected here from Task 4's original review.** That
review called it "at most one published frame," reasoning from
`session_alive()` alone. The final whole-branch review found the actual
gate is wider: `main.cpp:1468`'s loop condition is

```cpp
while (!quit && (zoom.session_alive() || NowNs() < room_move_grace_ns))
```

`room_move_grace_ns` is the 20 s breakout-move grace window (2026-08-30,
CLAUDE.md), armed on a deliberate room hop and cleared only once the status
has visibly left and returned to `InMeeting`. While that window is open the
loop keeps running, and keeps publishing panel state, **even though
`session_alive()` has already gone false** — that is the whole point of the
grace window, tolerating the `JOIN_BREAKOUT_ROOM` → `RECONNECTING` →
`CONNECTING` rejoin sequence a room move produces. Any of the six collapsed
statuses landing during that window renders as `IDLE` on the status chip for
as long as the window stays armed — up to **20 seconds**, not one frame.

**Diagnostic fidelity is still unaffected.** `zoom_client_win.cpp`'s
`onMeetingStatusChanged` still logs via `MeetingStatusName` (the raw,
15-case function), not `MeetingStateName` — so `[sdk] meeting status: ...`
in the log/console always carries the exact Zoom status, including all six
of the above, for the window's whole duration. Nothing here loses
diagnostic information; only the panel's one-line operator-facing chip does.

No fix planned: `MeetingState` is the seam's design (see `src/zoom/
zoom_client.h`'s header comment — "the platforms enumerate these
differently and ZComms only ever asks two questions of them"), and widening
it to carry six more values nothing acts on would defeat the point of
collapsing the enum at the seam. Recorded here so a future reader sizing
"does this matter" starts from the real bound, not the one-frame estimate.

---

## Bugs in shipping behaviour

### B2-refile — `TalkbackChannels`' unguarded ladder entry points changed risk class

`docs/plans/2026-09-05-macos-port-p0-followups.md`'s B2 found that
`Invite`, `InviteMany`, `Remove` and `SetChannelVolume` in
`src/zoom/talkback_channels.cpp` deref `sdk_` with no null guard (unlike
`CreateChannels`, `SendToSlot` and `SendToKeyed`, which all check
`sdk_ == nullptr`), and ruled it **"unreachable in the app"** because
nothing in P0's `main.cpp` could construct a `TalkbackChannels` with a null
`TalkbackSdk*`.

P1-A changed the precondition that ruling rested on. `ZoomClientWin::
MakeTalkbackSdk()` (`src/zoom/zoom_client_win.cpp:701`) returns `nullptr`
when the meeting has no talkback controller — a real return path a
`std::make_unique<TalkbackSdkWin>(controller_)` construction (P0's old
shape) never had. `main.cpp:1304-1305` then passes that possibly-null
pointer straight through:

```cpp
auto talkback_sdk = zoom.MakeTalkbackSdk();
TalkbackChannels bank(talkback_sdk.get());
```

**Still non-fatal today, and here is why:** `main.cpp:1306`'s very next line
checks `!bank.meeting_supports_talkback()`, which is itself guarded
(`sdk_ != nullptr && sdk_->MeetingSupportsTalkback()` — the same guarded
style `CreateChannels`/`SendToSlot`/`SendToKeyed` use) and returns `-1`
before any of the four unguarded methods can be reached with a null `sdk_`.
So the app cannot crash through this path *today*, and P0-followups B2's
bug is not newly triggered.

**What did change is the risk class.** B2 was filed as "unreachable" on the
strength of an argument about how `main.cpp` happened to construct the
object — an argument about caller discipline, not about the class's own
invariants. P1-A is proof that argument has a shelf life: one seam
extraction later, the exact "impossible" precondition (`sdk_ == nullptr`)
is now a real, live return value one caller-check away from every one of
those four unguarded methods. The class itself is no safer than it was; the
only thing standing between it and B1-style unreachability-turned-real is
`main.cpp`'s early return staying in place and every future caller
noticing it needs to be there. That is a fragile invariant to lean on
across a seam boundary, which is exactly what this phase added.

No fix here — out of scope for this wave (adding guards to
`talkback_channels.cpp` is explicitly excluded). A pointer line has been
added to the P0-followups doc's B2 entry so a reader who reaches B2 first
learns the "unreachable" framing has a known crack in it.

---

## Process and repo

### The CI-trigger story, and why P0-followups P3 is now known wrong

P0-followups' P3 ("post-merge cleanup") called the `push: branches: [main,
macos-port]` trigger on both workflows dead weight once P0 landed, trivial
to drop later. It was not trivial, and it was not dead weight — it was live
and broken. This branch (`macos-port-p1`) ran on `macos-port-p1`, a branch
name the filter never listed, so its first three commits
(`bed48b3`..`2a91204`, per `.superpowers/sdd/2026-09-05-macos-port-p1a-
client-seams/progress.md`) got **zero CI runs on either platform** —
`gh run list --branch macos-port-p1` came back empty, and the commits carry
no check-runs. The entire Windows-correctness argument for this whole port
is "the change is mechanical **plus CI**"; for those three commits it was
just "mechanical," silently, with nobody the wiser until a Task 1 brief
required confirming green and it could not be done.

Fixed forward in this branch's own history (`39c3b60` dropped the branch
filter to bare `on: push:` + `pull_request:` on both `windows.yml` and
`macos.yml` — see their header comments, which now tell this exact story).
The first run under the fixed trigger came back **Windows RED**
(`zoom_client.h:29` still including the just-deleted `mic_source.h`,
run 33994039774) — an immediate, concrete demonstration that the gate P3
called trivial had been silently absent, and that its absence had already
let a real breakage through uncaught.

**P3 as written should be read as superseded, not merely completed.** Its
premise — that the branch filter was inert scaffolding — was wrong on this
very branch, within P1-A's own first three commits. Nothing further to do
here (the fix already landed); this entry exists so nobody re-derives "the
filter never mattered" from reading P3 in isolation.

---

## Deferred minors

- **No `concurrency:` group on either workflow.** Dropping the branch
  filter (see above) means a same-repo PR now triggers both `push` and
  `pull_request` runs of each workflow — two runs per commit instead of
  one. A `concurrency:` group keyed on `github.ref` would cancel the
  superseded run. Cheap, deferred, explicitly out of scope for this wave.
- **`on: push:` with no `branches:` filter also fires on tag pushes.**
  This was a side effect of the trigger fix above, not something anyone
  chose deliberately. It is harmless today — both workflows just build and
  test — but it matters before §7's P4 lands a sign-on-tag job: that job
  will start firing on every tag push through the same trigger, and needs
  its own scoping (e.g. `if: startsWith(github.ref, 'refs/tags/')`) decided
  then, not discovered then.
- **`ZoomClient::last_fail_code()`/`FailReason()` and `zc::
  MeetingStatusName`/`AuthResultName` have no callers outside their own
  translation unit.** All four are used only inside `zoom_client_win.cpp`
  (verified by grep across `src/`, `spikes/` and `tests/` — the lesson
  CLAUDE.md now records after this branch cost two red Windows runs to
  two different single-directory greps). Not a bug: `last_fail_code()`/
  `FailReason()` are seam API by design (a P1-B macOS caller is the
  expected future consumer), and `MeetingStatusName`/`AuthResultName` are
  Windows-internal diagnostic helpers. Noted only so nobody reads "zero
  external callers" as "dead code" and removes something the seam's other
  side is meant to use.

---

## Gate for the next phase

### P1-B must verify the mono law explicitly, against a live meeting

CLAUDE.md's delivery law 5 — `SendAudioDataToChannel` is **mono only**;
`ZoomSDKAudioChannel_Stereo` returns `SDKERR_SUCCESS` and delivers nothing
audible — now lives **below the seam**, duplicated in each backend rather
than enforced once at the seam itself: `src/audio/mic_source_win.cpp:58`
hardcodes `ZoomSDKAudioChannel_Mono` in its `send()` call, and
`talkback_sdk_win.cpp` hardcodes the same enumerator at its own call site.
That duplication is inherent to the seam design (§3.3: the SDK's channel
enum is exactly the kind of platform detail that stays in the adapter), but
it means the law's enforcement now depends on both adapters getting it
right independently, with nothing above the seam checking either one.

**Nothing in this branch's seven new tests pins it.** `FakeVirtualMic` (the
seam-level test double `tests/audio/fake_mic_sender.h` and its Task 2
tests exercise) has no channel-count concept at all — `Send()` takes PCM
and a sample count, full stop. A macOS adapter (`mic_source_mac.mm`, not
yet written) could declare its send as stereo, and every test in this
suite, including the fake-driven ones, would still pass: the fake does not
know the difference, and per law 5 the real SDK would return success while
delivering nothing audible. The failure mode this would produce — "tests
green, nobody can be heard" — is the exact shape of the 2026-08-29 no-audio
hunt CLAUDE.md documents, on a new platform, with no test standing between
it and shipping.

This is not a P1-A defect — the mono law was never pinned at the seam
level even on Windows before this branch, and adding a channel-count
concept to the fakes is a real design change, not move-only work. It is
filed as a **gate**: P1-B (or whichever plan first exercises
`mic_source_mac.mm`/`talkback_sdk_mac.mm` against a real meeting) must
verify law 5 holds on macOS by listening to a live channel, the same way
P0/P1's Windows work verified it live rather than trusting the SDK header's
"mono or stereo" claim (which CLAUDE.md already documents as false). A unit
test cannot substitute for this — the whole reason law 5 exists is that
`SDKERR_SUCCESS` is not evidence of anything reaching a listener.
