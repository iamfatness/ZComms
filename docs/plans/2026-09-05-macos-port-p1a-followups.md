# macOS port P1-A — carried-forward findings

Findings from Task 4 (the `ZoomClient` seam's Windows backend) that were
deliberately **not** treated as bugs, with the reasoning. None blocks merge.

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

**Blast radius is small.** `ZoomClient::session_alive()` is already `false`
for every one of these six statuses (none of them is `InMeeting`,
`JoiningBreakout`, `LeavingBreakout`, `Reconnecting`, or `Connecting`), and
`session_alive()` gates the main loop main.cpp runs the panel-state
publish from. So the affected states can only be visible for at most one
published frame during teardown, immediately before the loop exits on
`!session_alive()` — never as a sustained or misleading "current state."

**Diagnostic fidelity is unaffected.** `zoom_client_win.cpp`'s
`onMeetingStatusChanged` still logs via `MeetingStatusName` (the raw,
15-case function), not `MeetingStateName` — so `[sdk] meeting status: ...`
in the log/console always carries the exact Zoom status, including all six
of the above. Nothing here loses diagnostic information; only the panel's
one-line operator-facing chip does, and only for a frame that's already on
its way out.

No fix planned: `MeetingState` is the seam's design (see `src/zoom/
zoom_client.h`'s header comment — "the platforms enumerate these
differently and ZComms only ever asks two questions of them"), and widening
it to carry six more values nothing acts on would defeat the point of
collapsing the enum at the seam.
