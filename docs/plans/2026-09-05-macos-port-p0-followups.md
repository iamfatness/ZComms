# macOS port P0 — carried-forward findings

Everything the P0 execution found and deliberately did **not** fix, with the
reasoning. Written 2026-09-05, when the plan's scratch ledger was deleted.

P0 delivered: CI for both platforms (there was none), the `TalkbackSdk` seam,
`TalkbackChannels` moved off Zoom's Windows SDK interface, that file's first
ten tests, and the macOS adapter. Nothing here talks to a live meeting — that
is P1.

Each item below was raised by a review, triaged, and ruled on. None blocks
merge. They are recorded because the next plan will be scoped against them.

---

## Bugs in shipping behaviour

### B1 — `AlreadyExists` never records presence, so the healer re-invites forever

`src/zoom/talkback_channels.cpp` records a member only on `TalkbackEvent::Ok`.
Every other response, including `AlreadyExists`, only sets `last_error_`. When
Zoom answers an invite with `ALREADY_EXIST` — meaning the person **is** in the
channel — the ladder does not record them, `want && !have` stays true, and the
healer re-invites the same person every 5–60 s for the life of the session,
spending the per-call rate-limit budget that Law 2 exists to protect.

It contradicts `src/zoom/talkback_sdk.h`'s own contract ("Confirmed presence.
NEVER retried -- retrying loops").

**Pre-existing** — not introduced by the port; the refactor preserved it
exactly. Fixing it is a behaviour change, and P0 was a move-only port, so
owner ruling (2026-09-05) was: pin reality, file the bug, fix it separately
with its own live verification.

`tests/zoom/test_talkback_channels.cpp` pins the current behaviour
(`members.count == 0`) and says why. **When this is fixed, that test inverts to
`== 1u` and its comment goes.**

Filed as a GitHub issue.

### B2 — four of eight ladder entry points would null-deref on a null `TalkbackSdk*`

`CreateChannels`, `SendToSlot` and `SendToKeyed` guard `sdk_ == nullptr`;
`Invite`, `InviteMany`, `Remove` and `SetChannelVolume` do not. Pre-existing —
the original guarded `controller_` in exactly the same places — so it is not a
regression.

Unreachable in the app (`main.cpp` bails on `!meeting_supports_talkback()`),
but **reachable from tests now**: a test constructing `TalkbackChannels(nullptr)`
and calling `Invite` segfaults the suite today. Adding the guards is a
behaviour change, hence next-plan work.

Note the related change P0 did make: `controller_ == nullptr` became
`sdk_ == nullptr`, which is never true. On the paths that do guard,
`CreateChannels` now mutates `want_` and resizes `channels_` before failing,
and the send paths take `send_m_` and increment `send_failures_` where the
original left them at zero. Only observable with a fake.

---

## Robustness, unreachable today

### R1 — `widen_cache_` is safe by unreachability, not by construction

`src/zoom/talkback_sdk_win.cpp`'s `WidenCached` returns a **reference** into a
`std::vector`, relying on a `reserve(64)` that is never enforced. Ids enter
only from `channels_[].id`, `channels_` is capped at 16, and the adapter is
session-scoped — so ≤16 distinct ids, genuinely unreachable.

But the safety **is** an unreachability assertion, and both `CLAUDE.md` and the
design spec name that as the exact claim behind two prior Majors in this
feature. The macOS side solved the same problem structurally: `CachedId`
returns a **copied** `void*`, so a reallocation costs a copy and nothing more.

If Windows is touched again, the right shape is stable-address storage
(`std::deque`, or `vector<unique_ptr<wstring>>`) — **not** returning by value,
which would restore the per-send allocation the fix removed.

### R2 — `TalkbackSdkWin` never unregisters its SDK event sink

The constructor calls `controller_->SetEvent(this)`; nothing undoes it, and
there is no destructor. `TalkbackSdkMac` does clear its delegate. Same
pre-existing hazard class as before the port, and unreachable because SDK
callbacks arrive on the pump thread, which is the thread running the
destructors, with `zoom.Cleanup()` ahead of both.

Recorded as a **coherence divergence**: P0 built the natural place to close
this and closed it on only one side.

### R3 — a nil channel id would poison the macOS id cache, then crash at teardown

`Ns()` returns nil for invalid UTF-8. `CachedId` would store `NULL`, return
`NULL` for that id forever (every later send/invite on that channel silently
no-ops), and the destructor's unguarded `CFRelease(NULL)` would crash.
Unreachable while ids are SDK-issued ASCII GUIDs. A nil check in `CachedId`
beats a guard in the destructor.

### R4 — the macOS adapter's destructor releases the cache without its mutex

Fine under the current quiesce-before-destroy invariant, which `main.cpp`
satisfies. Wants an assertion if teardown ever becomes threaded.

---

## Test and tooling gaps

### T1 — nothing pins the branch's headline invariant

`TalkbackResult::raw` exists so the ladder can print a platform SDK code
without ever branching on it. But `FakeTalkbackSdk::next_result` is a
`TalkbackCall` implicitly converted, so **every scripted response carries
`raw == 0`** — no test can distinguish a ladder that branches on `raw` from one
that doesn't. A future `if (r.raw == 18)`, precisely the regression the seam
exists to prevent, would land green.

Cheap fix: add `next_raw` to the fake, script `TooFrequent` with
`raw = 4242`, assert the ladder's decision is unchanged.

### T2 — two ladder tests share one mutation

The readiness test and the invite-refuses-unready test were both mutation-proved
by the same edit in `CreateChannels`. `Invite`'s ready-guard is a **separate**
`if` from `InviteMany`'s; deleting one leaves the other green, so that pin was
never independently proven. The house rule is "every pin mutation-proved" and
this one is not.

### T3 — the macOS framework link line is never exercised

`zcomms_zoom` is a **static** library, so it has no link step, and on Apple no
executable links it (`zcomms` and `zcomms_spike_a` are both `AND NOT APPLE`).
The macOS gate therefore proves the adapter **compiles** against the real
headers — most of the value, since every selector and enumerator is checked at
compile time — but **not** that the framework links or that `-F` propagates to
a consumer. The first executable in P1 finds out.

Do not read "macOS CI green" as "the framework links."

### T4 — the exhaustive-switch design half-works

`talkback_sdk.cpp`'s two switches deliberately omit `default:` so that adding an
enumerator warns. Verified by mutation: Clang's `-Wswitch` is **on by default**
(not gated behind `-Wall`) and does fire on macOS. MSVC's C4061/C4062 are
off-by-default at `/W4`, so Windows stays silent. No `-Werror` anywhere, so
neither platform fails.

Complete fix if ever wanted: `-Werror=switch` on the non-MSVC branch plus
`/we4062` on MSVC — one line each. A clean macOS build produces zero warnings
today, so `-Werror=switch` is safe to add whenever.

---

## Process and repo

### P1 — no branch protection

Neither `main` nor `macos-port` is protected. The whole Windows-correctness
argument for this branch is "the changes are mechanical **plus CI**" — and with
no protection, CI reports but does not block a merge on red.

Fine for P0, where every check was green and verified by hand. **Not fine from
P1 onward**, where Windows edits stop being mechanical. Recommend requiring
`Windows build & test` on `main` before P1 starts. Owner call.

### P2 — fork PRs will false-RED

The repo is public, and GitHub forces fork-PR tokens read-only regardless of a
workflow's `permissions:` block, so the draft-release SDK fetch 404s. A
false-RED for outside contributors, never a false-GREEN. No action unless
external PRs start arriving.

### P3 — post-merge cleanup

Both workflows still trigger on `push: branches: [main, macos-port]`. The
`macos-port` trigger is dead once this lands.
