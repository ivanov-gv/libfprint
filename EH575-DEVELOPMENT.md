# Experimental EgisTec EH575 development

This branch adds a native libfprint press image driver for USB `1c7a:0575`,
revision `1072`, tested on one Acer Swift SF314-43. It uses libfprint's existing
NBIS/Bozorth3 path with the default matching threshold in the default build.
An opt-in native stationary ridge matcher is described below. There is no Python
runtime matcher, proprietary implementation, firmware blob or public ABI change.

Development base: `6f9479c3d55f847c1b3769f28ceb99227f9858cf`. Keep `master`
as the upstream baseline and do driver work on `codex/egis0575`. The GitHub
`origin` is the development fork; `upstream` is the original GitLab repository.

## Current status

Native discovery, claim/release, initialization, idle exposure calibration,
cancellation and real image capture have worked. The reported capture was
206×104 pixels with **only two minutiae**. That is not evidence of usable
native recognition. Real-device enrollment, matching, placement tolerance, fprintd
lifecycle and GNOME unlocking remain unvalidated. Do not enable login yet.
Five subsequent independent native captures produced 2, 2, 4, 2, 2 minutiae.

The portable protocol and ten mocked driver lifecycle cases, plus libfprint's
device and SSM suites, pass in a driver-only warnings-as-errors build. The
native tests have also passed address/undefined/leak sanitizer testing during
prototype development. Mocked callbacks do not prove real core transitions or
biometric matching. Full upstream CI, introspection and USB emulation are pending.
The working Python prototype's results are not native matching evidence.

The swipe experiment was retired after repeated failed real-reader trials and
user feedback on 2026-10-08. The active driver is press-only: place the pad flat,
hold still until capture, then lift. Swipe sources/build options were removed;
the experiment remains recoverable in Git history through `5a54ceee`.
Old swipe binaries are rejected by the hardware runner. Build a fresh directory,
rather than using `/tmp/eh575-libfprint-swipe-build`.

After contact settling, capture requires three frames with full-frame,
background-corrected correlation at least 0.97 to the burst's first frame.
If contact changes, the burst restarts silently within the original 30-second
deadline; no swipe alignment, travel requirement or "too fast" retry is used.
This acquisition gate avoids combining different placements in a median image;
it is not a fingerprint matching threshold or proof of usable recognition.
The measured area remains 103×52 pixels; native feature scarcity remains an
open problem. Login integration is not enabled.
The new experimental ridge build bypasses NBIS rather than lowering its threshold;
verification uses five steady frames with the same acquisition stability gate.
Offline replay of 48 within-touch frame pairs from the private prototype
recording passed this stability gate (minimum correlation 0.985762). These are
selected burst frames, not fresh hardware validation or matching evidence.

The prepared stationary build on the development laptop is:

```sh
python3 scripts/eh575-run.py capture \
  --build /tmp/eh575-libfprint-press-build --deps /tmp/eh575-native-deps/root
```

## Isolated build, no installation

### Opt-in native stationary ridge matching

`-Degis0575_ridge=true` selects a native `FpDevice` implementation with its own
enroll/verify callbacks, not an `FpImageDevice` that delegates to NBIS. It requires
`-Ddrivers=egis0575` alone and OpenCV development libraries >=4.5 (core, imgproc,
features2d, calib3d, video). The default build is unchanged and does not need
OpenCV. This dependency and host-side matcher are experimental, not an
upstream-ready or security-reviewed contribution.

The native C++ registration follows the separate MIT-licensed prototype:
background subtraction, Gaussian ridge enhancement, contact mask, multi-scale
rotation/translation candidates, SIFT-seeded affine refinement, ambiguity checks,
edge detail and three-region corroboration. Five steady probe frames are compared
to one enrolled area at a time under the same fitted transform; at least three
must pass. Matching all enrolled areas is NOT required. Burst frames are correlated
samples, not independent security trials. Both acceptance and geometric bounds
retain the prototype's fixed policy; this is not an exact bit-for-bit Python port.

Enrollment uses 15 separate stationary touches: three each at center, tip-side,
base-side, left-side, right-side. Move only BETWEEN touches, using small overlapping
areas, not a swipe. Each accepted touch stores its median raw sample and measured
background in versioned `FPI_PRINT_RAW` data (`eh575-ridge-v1`). Native prints are
serialized through libfprint; fprintd can persist them without knowing the format.
Old NBIS/Python prints cannot be imported. Templates are biometric data, not hashes.

The hardware runner defaults to `.state/eh575-ridge`, separate from the image
driver's `.state/eh575`. Files are created exclusively with mode 0600 inside a
0700 directory; existing templates are not overwritten. A failed write preserves
its incomplete file and requires a new test directory. The enrollment step never
installs anything or changes GNOME, PAM, fprintd or USB access rules.

Normal dependency build:

```sh
meson setup /tmp/eh575-libfprint-ridge-build . \
  -Ddrivers=egis0575 -Degis0575_ridge=true -Dintrospection=false -Ddoc=false \
  -Dinstalled-tests=false -Dudev_rules=disabled -Dudev_hwdb=disabled -Dwerror=true
meson compile -C /tmp/eh575-libfprint-ridge-build
meson test -C /tmp/eh575-libfprint-ridge-build \
  egis0575-protocol egis0575-driver egis0575-quality egis0575-ridge \
  egis0575-ridge-device fpi-device fpi-ssm --print-errorlogs
```

On the development laptop this build is already prepared with temporary extracted
dependencies, NOT system-installed packages. Run from this clone, without sudo:

```sh
python3 scripts/eh575-run.py enroll \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root
python3 scripts/eh575-run.py verify \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root
```

Keep empty until calibration/prompt, hold steady until captured, lift fully.
Then test BOTH the enrolled finger and non-enrolled fingers, with variation across
independent touches, reboot and suspend/resume. Any wrong-finger match blocks
deployment. Password fallback remains essential; spoof/liveness resistance is
not established, and a small wrong-finger audit is not a population false-accept
rate estimate. GNOME unlocking is not enabled or yet validated.

Matching runs in a bounded cancellable `GTask` worker with a copied snapshot.
The operation is not completed until that worker stops; invalid templates,
library exceptions and cancellations cannot become matches. Template type,
schema, normal form, total size, count and per-image lengths are checked before
acquisition. Device disconnect/USB errors use the existing transport guards.

On 2026-10-09 an in-memory native replay of the previously recorded development
audit accepted 7/8 genuine touches and 0/24 wrong-finger touches, with no invalid
data/matcher failures. This reproduces the old aggregate result but is NOT fresh
independent native hardware validation or authorization for system login.
No recordings or biometric templates are published. Optional diagnostic:

```sh
../fingerprint/.venv/bin/python scripts/eh575-ridge-replay.py \
  ../fingerprint/.state/coverage-v3/enrollment.npz \
  ../fingerprint/.state/coverage-v3/audits/20261008T184020093520Z \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root
```

Selected tests include synthetic texture identity, wrong texture, blank images,
periodic ambiguity, malformed input and cancellation; real public libfprint core
enrollment/serialization/verification, wrong-texture rejection, worker cancellation,
reactivation and malformed template rejection use synthetic USB input. They do
not prove real-device recognition, GNOME behavior or security. Broad debug logging
is suppressed by the runner; driver-only scalar diagnostics may be requested with
`G_MESSAGES_DEBUG=libfprint-egis0575`. Full upstream CI remains pending.


### Real fprintd on an isolated test bus

The native hardware trial reported on 2026-10-09 completed enrollment and accepted
12/15 completed enrolled-finger scans; three were rejected and one additional
attempt failed calibration. All 10 scans labeled non-enrolled fingers were rejected.
The recordings did not identify the per-finger breakdown. These small, user-reported
trials are development evidence, not a security certification or false-accept estimate.

The next stage uses Ubuntu's REAL `/usr/libexec/fprintd`, not the Python compatibility
bridge. The daemon loads the selected native libfprint build through a child-only
library path. The supervisor verifies its actual mapped library and checks the
exposed driver name, press type and 15 enrollment stages. On this laptop,
stock fprintd 1.94.5 passed private-bus discovery, authorization and open/release.
Real fprintd enrollment/verification still requires fresh hardware touches.

No daemon patch, installation, systemd override, PAM change or GNOME change is made.
A fresh Unix D-Bus daemon has no activation directories and only admits the current
UID. Both bus-address variables are set ONLY in child processes. The system service
is never stopped or contacted by the test clients. Templates are stored separately
under `.state/eh575-fprintd/prints`, with a 0077 umask and a private 0700 state tree.
Existing smoke-test/Python/system templates are not imported, modified or deleted.
The real daemon's file-storage backend honors `STATE_DIRECTORY`; the supervisor
requires the installed configuration to select this backend.

A small TEST-ONLY PolicyKit fixture on this private bus allows only verify/enroll
actions for bus subjects belonging to the current UID. Managing another user's
prints is not authorized. This deliberately does NOT test real system PolicyKit
authorization; it is not suitable for system login. A separate private logind
fixture returns dummy inhibitor FDs, which cannot delay real sleep. The supervisor
uses Python/Gio for orchestration and prompts only, never for matching or templates.
Dependencies: Ubuntu's fprintd clients/daemon, dbus-daemon, Python 3 and python3-gi.

Run from this clone without sudo:

```sh
python3 scripts/eh575-fprintd-session.py \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root \
  -- check

python3 scripts/eh575-fprintd-session.py \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root \
  -- fprintd-enroll -f right-index-finger

python3 scripts/eh575-fprintd-session.py \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root \
  -- fprintd-verify -f right-index-finger
```

This is a NEW enrollment through fprintd: 15 stationary touches, three each at
center/tip-side/base-side/left-side/right-side. Wait for `Reader ready` or
`Reader calibrated`, not the stock client's initial `Enroll/Verify started` message.
Lift fully when captured; use small overlapping placement changes BETWEEN touches.
The test supervisor observes real D-Bus finger-status properties to print prompts.
The enrolled print persists between private sessions, so repeat verification with
both enrolled and non-enrolled fingers. Re-enrollment can replace ONLY that private
fprintd print. `fprintd-delete`, when deliberately invoked through this supervisor,
deletes ONLY private test prints; it never erases the other enrollment directories.

After enrolling, test cancellation with an EMPTY reader:

```sh
python3 scripts/eh575-fprintd-session.py \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root \
  -- cancel-test
python3 scripts/eh575-fprintd-session.py \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root \
  -- sleep-test
```

`cancel-test` checks VerifyStop, release/reclaim, and client-disconnect cleanup.
`sleep-test` emits prepare-for-sleep/resume ONLY on the private bus. It now uses
the STOCK `fprintd-verify` client and requires its release on the terminal suspend
error BEFORE emitting resume, then checks reclaim. The previous direct-call test
released only after resume and missed a lost-claim/open-device failure observed
during physical sleep. It does not suspend the laptop or prove physical USB
resume. Verify with a
real finger again after each test. Native real-core tests also cover cancellation
of an active matching worker by suspend, close-before-resume, and successful
reopen/reactivation. Core tests cover resume racing asynchronous close, including
a close error. A running/preserved scan still blocks close; new scans and opens
remain blocked while suspended.

This branch includes a local libfprint CORE compatibility change: cleanup close
is allowed while suspended once the active operation finishes, and resume waits
for an in-flight close. Stock fprintd otherwise drops the failed-release claim
while libfprint retains the open device. This is not an EH575 matcher adjustment
or a daemon restart workaround. The API cleanup exception needs separate upstream
review from the driver/matcher submission; it is not an upstream-accepted change.

For an actual laptop sleep/resume trial, omit the command to enter a private shell
and add `--forward-sleep`. This option opens a READ-ONLY subscription to the real
logind sleep signal and forwards just those notifications to the private logind
fixture. It never calls a real power-management method or holds a real inhibitor.
Use the normal desktop sleep control, then run `fprintd-verify` again in that same
private shell after waking. Do not interrupt enrollment with sleep on purpose until
simple verification/recovery works. Without this option, the private daemon does
not receive real logind sleep notifications. This harness does not test system
inhibition, GNOME unlock prompts, password fallback or system authorization.

Omitting a command opens a shell whose fprintd commands all use the private bus.
Type `exit` to close it. Only the child client, private real-daemon process and
owned temporary bus are stopped; the private enrollment is preserved. A state lock
prevents concurrent sessions sharing one private store. Failed checks or unexpected
daemon exit stop the test rather than falling back to system fprintd.

Run the bus/authorization/storage-guard tests (no sensor):

```sh
python3 tests/test-egis0575-fprintd-session.py
```

The source behavior was checked against official
[fprintd v1.94.5](https://gitlab.freedesktop.org/libfprint/fprintd/-/tree/v1.94.5),
commit `b54a007ccf58ac0ae074c7151b223f35cbd17306`. No upstream source patch is
needed for this test. Full upstream fprintd tests and real GNOME deployment remain
separate work.

The separately approved system trial is described in
[EH575-GNOME-TRIAL.md](EH575-GNOME-TRIAL.md). Its package builder prepares a
root-owned service-scoped override and rollback, but does NOT install it.
Private cancellation/disconnect passed on 2026-10-09. Physical sleep during
verification exposed the release-before-resume bug; the strengthened simulated
stock-client release/reclaim test passes with the core cleanup fix. Physical
sleep with that fix passed in the SAME daemon, including interrupted capture and
post-resume wrong-finger rejection. The user approved and installed the
`4ab3b17e` trial package on the development laptop. Real system fprintd now
discovers the native press device with 15 enrollment stages; its existing
hardening and PAM/daemon file checksums were checked and remain unchanged.
System library selection HAS changed; private templates were not copied.
There is no system enrollment yet. Actual system enrollment/verification,
GNOME lock/password fallback and system-service sleep recovery still need
testing. Do not install the old `de8e9eab` trial artifact.

### Default stationary image driver


With a compiler, Meson, Ninja, pkg-config, GLib development headers, GUsb
0.3.3 or newer and libusb development headers available, run from this clone:

```sh
meson setup _build . \
  -Ddrivers=egis0575 -Dintrospection=false -Ddoc=false \
  -Dinstalled-tests=false -Dudev_rules=disabled -Dudev_hwdb=disabled -Dwerror=true
meson compile -C _build
meson test -C _build egis0575-protocol egis0575-driver egis0575-quality fpi-device fpi-ssm --print-errorlogs
```

This does not install anything or change PAM, fprintd, GNOME or USB permissions.
The GitHub workflow runs these selected synthetic tests, not hardware or the
full upstream suite.

## Hardware tests

Close other reader clients and run without sudo using existing desktop USB
access. For a normal dependency build:

```sh
python3 scripts/eh575-run.py capture --build _build
python3 scripts/eh575-run.py enroll --build _build
python3 scripts/eh575-run.py verify --build _build
```

On the development laptop, a separate build using already-extracted temporary
dependencies is available. These paths can disappear after reboot:

```sh
python3 scripts/eh575-run.py capture \
  --build /tmp/eh575-libfprint-github-build --deps /tmp/eh575-native-deps/root
```

Keep the reader empty until “Reader calibrated”, then hold your pad flat until
capture and lift fully. `capture` reports dimensions/minutiae and discards the
image. `open` tests discovery/claim/release; `idle` calibrates and cancels after
three seconds without a finger.

To measure independent native touches before changing acquisition, run:

```sh
python3 scripts/eh575-run.py capture-series \
  --build /tmp/eh575-libfprint-github-build --deps /tmp/eh575-native-deps/root
```

This takes five independent captures, reopening the reader for each. Lift fully
and press Enter with the reader empty before each calibration. After “Reader
calibrated”, place the finger flat with small placement variations. The series
stops on a failed capture or Ctrl+C. It prints counts, saves no images/templates
and does not enroll or authenticate. Keep its output for development comparison.

## Offline acquisition-quality investigation

`tests/eh575-quality` consumes exactly 10,712 bytes from stdin: a raw 103×52
uint8 median followed by its measured empty reference. It never opens USB or
writes files. It compares gains 1/2/3, bilinear scales 1/2/3 and both polarities
through native minutiae extraction, printing counts only. The current-driver
variant calls the actual `eh575_normalize` and `eh575_enlarge` helpers. All
variants keep partial-image perimeter filtering and the native extractor's
defaults. These are offline experiments, NOT authentication settings. Scale
changes alter apparent ridge geometry; more extracted points can be artifacts.

For a private recording made by the separate Python prototype, NumPy is required
only by this optional replay adapter (not the driver or CI). For example, from
this clone, using the existing prototype environment on the development laptop:

```sh
../fingerprint/.venv/bin/python scripts/eh575-replay.py \
  ../fingerprint/.state/coverage-v3/enrollment.npz \
  --build /tmp/eh575-libfprint-github-build --deps /tmp/eh575-native-deps/root
```

The adapter forms one median per touch, pairs it with its measured reference
and sends it to the diagnostic in memory. It prints aggregates, not images,
minutia coordinates or individual labels. No data is imported into native
enrollment, published or saved. Treat recordings as sensitive even when a
directory is Git-ignored.

Local investigation on 2026-10-08 used 24 previously recorded enrollment touches,
each with three frames and its own empty reference. The current pipeline
(gain 2, scale 2, original polarity) yielded **0..8 minutiae, median 4**, with
one zero extraction. At scale 2, gains 1 and 3 also yielded median 4. At scale 3,
some variants had higher counts (medians up to 6), but physical resolution and
feature validity are unestablished. Neither this count experiment nor the old
Python match results establish native recognition or justify threshold changes.
The production driver's acquisition, scale, flags and matching are unchanged.

`enroll` requires ten independent right-index-finger touches. It writes a
private biometric template, not images, under `.state/eh575` (directory 0700,
template 0600). It refuses to overwrite an existing template; use a fresh
`--state .state/eh575-trial-2` for a new enrollment. Python enrollments cannot
be imported. The old fingerprint repository and `.state/coverage-v3` are
unchanged. Never publish templates, captures or personal USB traces.

Cancelled/incomplete wire exchanges require close/reopen; the helper closes
on exit. Calibration can reject an early finger. Real GNOME prompt behavior
still needs testing.

## Next development and upstream review

1. Measure native capture/minutiae quality across repeated placements. The
   physical image is 103×52; 2× interpolation does not add coverage or detail.
   Investigate stationary acquisition/preprocessing and whether native minutiae
   matching is suitable for this small measured area. Do not lower the authentication threshold
   to make a demonstration pass.
2. Establish real native enrollment and independent genuine/wrong-finger
   outcomes, including placement variation, reboot and suspend/resume. Any
   wrong-finger match blocks login deployment.
3. Test real fprintd enroll/verify/delete, cancellation/disconnect and GNOME
   Settings/GDM lock-screen behavior with password fallback, only after an
   explicitly approved, reversible installation plan.
4. Add consented/sanitized USB-emulation fixtures, run full upstream CI and
   confirm physical resolution/orientation, preprocessing, GUsb compatibility
   and driver naming with maintainers.

Follow [HACKING.md](HACKING.md) for upstream submission. libfprint uses GitLab
merge requests; publishing this fork on GitHub does not submit it upstream.
An initial draft should be marked **WIP: egis0575: add an image driver for
EgisTec EH575**, with actual native results and failures stated clearly.

The driver, Meson integration, unsupported-device-list adjustment, protocol
notes and synthetic tests form the contribution. The helper, smoke executable,
GitHub workflow and this development guide may belong in separate development
commits rather than the final upstream patch. Review before staging; nothing
here automatically commits, pushes or creates a merge request.

See [protocol notes](doc/egis0575-protocol.md) for exact transport assumptions
and provenance. New native code is LGPL-2.1-or-later; the MIT register-table
notice is retained beside the driver. No personal biometric data is included.
