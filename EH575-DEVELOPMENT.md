# Experimental EgisTec EH575 development

This branch adds a native libfprint press image driver for USB `1c7a:0575`,
revision `1072`, tested on one Acer Swift SF314-43. It uses libfprint's existing
NBIS/Bozorth3 path with the default matching threshold. There is no Python
matcher, proprietary implementation, firmware blob or public ABI change.

Development base: `6f9479c3d55f847c1b3769f28ceb99227f9858cf`. Keep `master`
as the upstream baseline and do driver work on `codex/egis0575`. The GitHub
`origin` is the development fork; `upstream` is the original GitLab repository.

## Current status

Native discovery, claim/release, initialization, idle exposure calibration,
cancellation and real image capture have worked. The reported capture was
206×104 pixels with **only two minutiae**. That is not evidence of usable
native recognition. Enrollment, matching, placement tolerance, real fprintd
lifecycle and GNOME unlocking remain unvalidated. Do not enable login yet.
Five subsequent independent native captures produced **2, 2, 4, 2, 2** minutiae.
An opt-in swipe acquisition build is now available for capture evaluation only;
real swipe quality and matching are not yet established.

The portable protocol and nine mocked driver lifecycle cases, plus libfprint's
device and SSM suites, pass in a driver-only warnings-as-errors build. The
native tests have also passed address/undefined/leak sanitizer testing during
prototype development. Mocked callbacks do not prove real core transitions or
biometric matching. Full upstream CI, introspection and USB emulation are pending.
The working Python prototype's results are not native matching evidence.

## Isolated build, no installation

With a compiler, Meson, Ninja, pkg-config, GLib development headers, GUsb
0.3.3 or newer and libusb development headers available, run from this clone:

```sh
meson setup _build . \
  -Ddrivers=egis0575 -Dintrospection=false -Ddoc=false \
  -Dinstalled-tests=false -Dudev_rules=disabled -Dudev_hwdb=disabled -Dwerror=true
meson compile -C _build
meson test -C _build egis0575-protocol egis0575-driver egis0575-quality egis0575-swipe fpi-device fpi-ssm --print-errorlogs
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
The default press pipeline and all matching settings remain unchanged.

## Opt-in experimental swipe capture

The small press image does not consistently yield enough native features. A
separate `-Degis0575_swipe=true` build now collects overlapping frames during
one slow straight slide and assembles a larger measured area before native
extraction. The default is `false`; enabling it requires `-Ddrivers=egis0575`
alone. This development switch is not a proposed permanent upstream API.
Both the Python and C hardware helpers refuse enrollment/verification in
swipe mode. Do not install this build or load it into fprintd yet.

For a normal dependency environment, build a new directory (do not change the
press build):

```sh
meson setup /tmp/eh575-libfprint-swipe-build . \
  -Ddrivers=egis0575 -Degis0575_swipe=true -Dintrospection=false -Ddoc=false \
  -Dinstalled-tests=false -Dudev_rules=disabled -Dudev_hwdb=disabled -Dwerror=true
meson compile -C /tmp/eh575-libfprint-swipe-build
meson test -C /tmp/eh575-libfprint-swipe-build \
  egis0575-protocol egis0575-driver egis0575-quality egis0575-swipe fpi-device fpi-ssm --print-errorlogs
```

The extracted-dependency build on this laptop is already prepared at that
temporary path. Run just one swipe first:

```sh
python3 scripts/eh575-run.py capture \
  --build /tmp/eh575-libfprint-swipe-build --deps /tmp/eh575-native-deps/root
```

Keep the reader empty until calibrated. Place the pad flat, briefly let it
settle, then slide **slowly in one straight direction across the sensor's
narrow dimension** for about 3–5 seconds. Move along the length of your finger
so nearby overlapping pad areas pass over the reader; keep contact throughout.
Then lift fully to finish. Unlike press capture, do not wait for “Captured”
before lifting: the image is assembled after removal. If too fast/uncertain,
try a slower, straighter slide; if too short, increase the measured travel.
Holding still must fail as a short swipe. No image/template is written.

Optionally prefix that command with `G_MESSAGES_DEBUG=libfprint-egis0575` for
driver-only frame-count/crop/rejection diagnostics. Do not enable `all` debug
logging, which can include biometric coordinates from the native extractor.

Tests cover both motion directions, exact measured crops, brightness shifts,
stationary frames, periodic ambiguity, excessive speed/drift, coverage gaps,
memory bounds, cancellation and clean reactivation. The swipe driver suite has
thirteen mocked lifecycle cases; these still do not establish real core/matcher
behavior. Each build passes the six selected suites. The synthetic swipe and
driver suites also pass address/undefined/leak sanitizer testing outside the
tracing sandbox. The real reader opens/closes with the swipe build. Real swipe capture,
geometric accuracy, feature reliability and genuine/wrong-finger matching
remain pending. More features alone are not proof of a better fingerprint.

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
   Investigate acquisition/preprocessing or overlapping swipe acquisition if
   the press patch is insufficient. Do not lower the authentication threshold
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
