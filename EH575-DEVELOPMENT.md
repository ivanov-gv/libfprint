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
