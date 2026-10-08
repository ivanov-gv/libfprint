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
meson test -C _build egis0575-protocol egis0575-driver fpi-device fpi-ssm --print-errorlogs
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
