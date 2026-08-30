# OBF / app_privilege_token Compliance: Audit + Cross-Account Join Plumbing

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Goal

Bring ZComms's join path into line with Zoom's March 2, 2026 requirement (already in force today, 2026-08-29): SDK apps joining meetings hosted outside their own account must present an app privilege (OBF) token, a ZAK, or an on-behalf token in `JoinParam` — audit what ZComms sends today, plumb the missing token fields end to end, and make every cross-account refusal legible to the operator.

## Architecture

The audit's findings (below, already done against the live tree) drive three changes: `ZoomClient::Join()` grows a `JoinTokens` struct so all four credential fields of `JoinParam4WithoutLogin` (`userZAK`, `app_privilege_token`, `onBehalfToken`, `join_token`) can be populated instead of only `userZAK`; a pure decision module (`join_gate`) picks which tokens a given join should carry and produces the exact operator-facing text when a join is doomed before it is attempted; and `ZoomOAuth` gains an OBF-token leg against the CoreVideo broker (the Cloudflare Worker that already mints SDK JWTs and holds the app credentials — token minting belongs server-side with the client secret, never in the exe). Failure decoding extends the existing latched-FAILED-code path in `zoom_client.cpp`.

## Tech Stack

- C++17, MSVC, CMake (existing build).
- Zoom Meeting SDK 7.1.5 vendored headers: `meeting_service_interface.h` (`JoinParam`, `JoinParam4WithoutLogin` with `app_privilege_token` at line 262, `userZAK`, `onBehalfToken`, `join_token`; `MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING = 504`).
- Existing auth stack: `src/app/zoom_oauth.{h,cpp}` (PKCE via the CoreVideo broker, DPAPI token store), `src/zoom/zoom_client.{h,cpp}` (`AuthenticateWithJwt`, `Join`), `src/zoom/jwt.{h,cpp}`.
- WinHTTP for the broker call (already linked: `winhttp` in the `zcomms` target).
- zctest harness, `zcomms_audio_tests` exe.

## Spec

This document doubles as the spec.

### Audit findings (verified against the tree, 2026-08-29)

1. **Where `JoinParam` is populated:** `src/zoom/zoom_client.cpp`, `ZoomClient::Join()` (~line 258). It sets `meetingNumber`, `userName`, `psw`, and — only when the caller passes one — `userZAK`. It **never** sets `app_privilege_token`, `onBehalfToken`, or `join_token`. The fields exist in the vendored header; ZComms simply doesn't plumb them.
2. **Auth flows in play:** (a) signed-in: PKCE → broker → `AuthenticateWithJwt` + ZAK from `ZoomOAuth::EnsureJoinCredentials` on `JoinParam.userZAK` — this satisfies the mandate for meetings the *signed-in user* can join, because a ZAK is one of the accepted credentials; (b) `--anon`: bare public-app-key guest join with no token at all — this is the non-compliant path for cross-account meetings and already fails them with `MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING` (504), decoded at `zoom_client.cpp` (`MeetingFailReason`).
3. **The gap:** a signed-in operator joining a meeting hosted by a *different* account (the client's production meeting — ZComms's core use case) presents a ZAK identifying the operator, which authenticates the user but does not by itself carry the app's cross-account privilege; the OBF `app_privilege_token` is the credential designed for that case. Today the outcome depends on Zoom-side enforcement state; after full enforcement the join can fail even signed-in.
4. **Not found in the vendored SDK:** any API to *mint* an OBF/app-privilege token (correct — it comes from Zoom's REST side, tied to the Marketplace app identity, not from the Meeting SDK), and any distinct MeetingFailCode for "token required/expired" beyond 504 (so error surfacing keys on 504 plus join context). Minting therefore lands on the CoreVideo broker; the exact Zoom REST endpoint and required Marketplace scopes are a broker-repo task and are treated here as an interface contract (`/oauth/obf-token`), not as settled fact.

### Requirements

1. `ZoomClient::Join()` accepts all four token credentials; empty strings mean "absent" and leave the `JoinParam` pointer fields null (the struct's `memset` default), never pointing at empty wide strings.
2. A pure `join_gate` decides, from what the session has (signed-in? ZAK? OBF token? anon flag?), which tokens the join carries and whether the join should even be attempted — with exact operator text for each refusal.
3. `ZoomOAuth` exposes `EnsureObfToken(uint64_t meeting_number, std::string* obf, std::string* error)` hitting the broker's `/oauth/obf-token?meeting=<n>` with the operator's access token; a broker 404 (endpoint not yet deployed) degrades to "absent" with one ops line, not a join abort.
4. A 504 join failure reports differently depending on what was sent: anon → the existing "start the meeting from the account that installed ZComms" text; signed-in without OBF → "cross-account meeting needs the app privilege token -- broker OBF support required (see docs/plans/2026-08-29-obf-token-audit.md)"; signed-in with OBF → "Zoom refused the app privilege token -- token may be stale, retry sign-in".
5. Nothing here weakens the owner rule "no anonymous joins": `--anon` remains a scripted-test escape hatch only.

## Global Constraints

- Build: `cmake -S . -B build`; `cmake --build build --config Release`. Tests: `build\Release\zcomms_audio_tests.exe` / `ctest --test-dir build -C Release`.
- Pure modules include no SDK headers; SDK-including TUs need `windows.h` first.
- `JoinParam4WithoutLogin` string fields are `const zchar_t*` borrowing from caller-owned storage: every wide string passed must outlive the `meeting_->Join(jp)` call — keep the existing local-`std::wstring` pattern of `Join()`.
- Secrets discipline: OBF tokens are short-lived join credentials — hold them in memory only, never in the DPAPI store, never in logs (log presence, length class, and expiry only).
- The FAILED code is latched and reported from the ENDED branch (existing trap — do not re-break it).
- Update `CLAUDE.md` in the same change as any substantive work.

## Tasks

### Task 1: `join_gate` — token decision + refusal text (pure)

**Files**
- Create: `src/zoom/join_gate.h`
- Create: `src/zoom/join_gate.cpp`
- Test: `tests/zoom/test_join_gate.cpp`
- Modify: `CMakeLists.txt`, `tests/audio/test_util.h`, `tests/audio/test_main.cpp`

**Interfaces**
- Produces:
  ```cpp
  // src/zoom/join_gate.h  (no SDK includes)
  namespace zc {
  struct JoinCredentials {
    bool signed_in = false;
    std::string zak;         // "" = absent
    std::string obf_token;   // "" = absent
    bool anon_flag = false;  // --anon scripted escape hatch
  };
  struct JoinDecision {
    bool attempt = false;
    std::string zak;         // what to put on JoinParam.userZAK
    std::string obf_token;   // what to put on JoinParam.app_privilege_token
    std::string ops_line;    // non-empty when something is worth saying
    std::string refuse_reason;  // non-empty iff !attempt
  };
  JoinDecision DecideJoin(const JoinCredentials& c);
  // Post-mortem: the operator text for a MEETING_FAIL code given what was sent.
  std::string JoinFailText(int fail_code, const JoinDecision& sent);
  }
  ```

**Steps**

- [ ] Write the failing test `tests/zoom/test_join_gate.cpp`:
  ```cpp
  #include "join_gate.h"
  #include "test_util.h"

  void TestJoinGate() {
    ZC_TEST("join_gate: signed-in with zak+obf sends both");
    zc::JoinCredentials c;
    c.signed_in = true;
    c.zak = "Z";
    c.obf_token = "O";
    auto d = zc::DecideJoin(c);
    ZC_CHECK(d.attempt);
    ZC_CHECK(d.zak == "Z");
    ZC_CHECK(d.obf_token == "O");

    ZC_TEST("join_gate: signed-in, no obf, still attempts with a warning");
    zc::JoinCredentials c2;
    c2.signed_in = true;
    c2.zak = "Z";
    auto d2 = zc::DecideJoin(c2);
    ZC_CHECK(d2.attempt);
    ZC_CHECK(d2.obf_token.empty());
    ZC_CHECK(d2.ops_line.find("cross-account") != std::string::npos);

    ZC_TEST("join_gate: not signed in and not --anon refuses");
    zc::JoinCredentials c3;
    auto d3 = zc::DecideJoin(c3);
    ZC_CHECK(!d3.attempt);
    ZC_CHECK(d3.refuse_reason.find("sign in") != std::string::npos);

    ZC_TEST("join_gate: --anon attempts bare (scripted escape hatch)");
    zc::JoinCredentials c4;
    c4.anon_flag = true;
    auto d4 = zc::DecideJoin(c4);
    ZC_CHECK(d4.attempt);
    ZC_CHECK(d4.zak.empty() && d4.obf_token.empty());

    ZC_TEST("join_gate: 504 text depends on what was sent");
    zc::JoinDecision anon_sent;    // nothing sent
    anon_sent.attempt = true;
    ZC_CHECK(zc::JoinFailText(504, anon_sent).find("account that installed") !=
             std::string::npos);
    zc::JoinDecision zak_only = anon_sent;
    zak_only.zak = "Z";
    ZC_CHECK(zc::JoinFailText(504, zak_only).find("app privilege token") !=
             std::string::npos);
    zc::JoinDecision with_obf = zak_only;
    with_obf.obf_token = "O";
    ZC_CHECK(zc::JoinFailText(504, with_obf).find("stale") != std::string::npos);

    ZC_TEST("join_gate: non-504 codes defer to empty (caller keeps old decode)");
    ZC_CHECK(zc::JoinFailText(9, zak_only).empty());
  }
  ```
- [ ] Register `TestJoinGate` in `tests/audio/test_util.h` + `tests/audio/test_main.cpp`; add `tests/zoom/test_join_gate.cpp` and `src/zoom/join_gate.cpp` to `zcomms_audio_tests` in `CMakeLists.txt` (plus `target_include_directories(zcomms_audio_tests PRIVATE tests/audio src/zoom)` if a sibling plan has not already).
- [ ] `cmake --build build --config Release --target zcomms_audio_tests` — failing.
- [ ] Implement `DecideJoin` / `JoinFailText` in `src/zoom/join_gate.cpp` to the exact truth table the test pins: signed-in → attempt with whatever tokens exist, warn `"no OBF token -- cross-account meetings may refuse this join"` when `obf_token` is empty; not signed-in and not anon → refuse `"sign in first (owner rule: no anonymous joins)"`; anon → attempt bare. `JoinFailText` returns `""` for every code except 504 (the caller falls through to the existing `MeetingFailReason`), and the three 504 variants from the test.
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green.
- [ ] Commit: `git add src/zoom/join_gate.h src/zoom/join_gate.cpp tests/zoom/test_join_gate.cpp tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt && git commit -m "feat(auth): join_gate token decision + 504 context-aware refusal text (pure)"`

### Task 2: `Join()` carries all four token fields

**Files**
- Modify: `src/zoom/zoom_client.h`
- Modify: `src/zoom/zoom_client.cpp`

**Interfaces**
- Produces (replacing the trailing `const std::string& zak` parameter — update the two existing call sites in `src/app/main.cpp` in the same change):
  ```cpp
  struct JoinTokens {
    std::string zak;                  // JoinParam4WithoutLogin::userZAK
    std::string app_privilege_token;  // ::app_privilege_token (OBF)
    std::string on_behalf_token;      // ::onBehalfToken
    std::string join_token;           // ::join_token
  };
  bool Join(uint64_t meeting_number, const std::string& password,
            const std::string& display_name, int timeout_ms, std::string* error,
            const std::function<void()>& on_tick = nullptr,
            const JoinTokens& tokens = JoinTokens());
  ```
- Consumes: `JoinParam4WithoutLogin` fields verified present in `meeting_service_interface.h` (lines 254–288 of the vendored header).

**Steps**

- [ ] Change the signature in `zoom_client.h`; in `zoom_client.cpp` extend the population block (keeping every wide string a named local so it outlives `meeting_->Join(jp)`):
  ```cpp
  const std::wstring zak_w = Widen(tokens.zak);
  const std::wstring obf_w = Widen(tokens.app_privilege_token);
  const std::wstring obo_w = Widen(tokens.on_behalf_token);
  const std::wstring jt_w  = Widen(tokens.join_token);
  // ...
  if (!zak_w.empty()) p.userZAK = zak_w.c_str();
  if (!obf_w.empty()) p.app_privilege_token = obf_w.c_str();
  if (!obo_w.empty()) p.onBehalfToken = obo_w.c_str();
  if (!jt_w.empty())  p.join_token = jt_w.c_str();
  std::printf("[sdk] join credentials: zak=%s obf=%s obo=%s jt=%s\n",
              zak_w.empty() ? "no" : "yes", obf_w.empty() ? "no" : "yes",
              obo_w.empty() ? "no" : "yes", jt_w.empty() ? "no" : "yes");
  ```
  (presence-only logging — Global Constraints forbid token values in logs).
- [ ] Update the call sites in `src/app/main.cpp` (the signed-in join passes `{zak, "", "", ""}` for now; `--anon` passes the default).
- [ ] Build the app target: `cmake --build build --config Release --target zcomms` — compile-clean gate; run `build\Release\zcomms_audio_tests.exe` to confirm nothing else moved.
- [ ] Commit: `git add src/zoom/zoom_client.h src/zoom/zoom_client.cpp src/app/main.cpp && git commit -m "feat(auth): Join() plumbs app_privilege_token, onBehalfToken, join_token alongside ZAK"`

### Task 3: broker OBF leg in `ZoomOAuth` + 504 context surfacing

**Files**
- Modify: `src/app/zoom_oauth.h`
- Modify: `src/app/zoom_oauth.cpp`
- Modify: `src/app/main.cpp`
- Modify: `src/zoom/zoom_client.cpp` (`MeetingFailReason` stays; the context-aware layer sits above it)
- Modify: `CLAUDE.md`

**Interfaces**
- Produces on `ZoomOAuth`:
  ```cpp
  // Fetches an app-privilege (OBF) token for joining the given meeting
  // cross-account. Refreshes the session first (same rule as
  // EnsureJoinCredentials). Returns false with *error on transport or auth
  // failure; a broker 404 (endpoint not deployed yet) returns true with an
  // empty *obf so callers degrade gracefully.
  bool EnsureObfToken(uint64_t meeting_number, std::string* obf,
                      std::string* error);
  ```
- Consumes: the broker contract `GET https://corevideo.iamfatness.us/oauth/obf-token?meeting=<n>` with `Authorization: Bearer <access token>`, response `{"obf_token":"...","expires_in":<sec>}` — **deploying this endpoint is CoreVideo-repo work** (the Worker holds the Marketplace credentials and calls Zoom's REST side; the exact Zoom endpoint/scopes get confirmed there against current Zoom docs). Until it deploys, the 404 degrade path keeps ZComms shippable.

**Steps**

- [ ] Implement `EnsureObfToken` in `zoom_oauth.cpp` using the same WinHTTP request helper `EnsureJoinCredentials` uses (refresh-if-needed under `m_`, one GET, parse the two fields with the file's existing JSON field scanner); map HTTP 404 → `return true` with `obf->clear()` plus one `[oauth] broker has no OBF endpoint yet -- joining without app privilege token` printf; map 401 → clear tokens and ask for re-sign-in (the established `invalid_grant` rule).
- [ ] In `src/app/main.cpp`'s signed-in join path: build `JoinCredentials` (`signed_in=true`, `zak` from `EnsureJoinCredentials`, `obf_token` from `EnsureObfToken(meeting_number, ...)`), run `DecideJoin`, `log_op` any `ops_line`, refuse with `refuse_reason` when `!attempt`, else call `client.Join(..., JoinTokens{d.zak, d.obf_token, "", ""})`.
- [ ] On join failure: call `JoinFailText(fail_code, decision_sent)`; when non-empty it replaces the generic decode in the operator-facing line (the raw `MeetingFailReason` still goes to the console log for diagnosis).
- [ ] Live checklist: (a) same-account signed-in join still works (regression); (b) cross-account meeting, signed-in, broker without OBF endpoint → join attempt + the "cross-account meeting needs the app privilege token" line on 504 (this reproduces today's live-diagnosed boundary with better words); (c) once the broker endpoint deploys: cross-account join succeeds with `obf=yes` in the credentials log — that success is the compliance gate closing, record it in CLAUDE.md.
- [ ] Update `CLAUDE.md`: rewrite "The app identity's join boundary" section to describe the token ladder (ZAK → +OBF), the broker dependency, and the degrade behavior.
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green.
- [ ] Commit: `git add src/app/zoom_oauth.h src/app/zoom_oauth.cpp src/app/main.cpp src/zoom/zoom_client.cpp CLAUDE.md && git commit -m "feat(auth): broker OBF token leg, DecideJoin wiring, context-aware 504 surfacing"`

## Self-review checklist (fix inline before PR)

- [ ] No `TBD`; the broker endpoint is an explicit interface contract with a degrade path, not a hand-wave.
- [ ] Wide-string lifetime rule respected in every `JoinParam` sketch (named locals, no temporaries).
- [ ] No token value ever printed or persisted; presence-only logging.
- [ ] `JoinFailText` returns `""` for non-504 so existing decodes keep working — pinned by test.
