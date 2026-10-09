# libfprint with experimental EgisTec EH575 support

This is a development fork of [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint),
the fingerprint-device library used by the Linux fprint stack. It adds native
support for the **EgisTec EH575 USB fingerprint reader (`1c7a:0575`)**, including
an opt-in stationary ridge matcher and a reversible Ubuntu/GNOME trial package.

The project started with a practical problem: a laptop's fingerprint reader worked
well in Windows but was unusable in Ubuntu. The goal is to make that same reader
usable through Linux's normal fingerprint services, particularly **GNOME login
and lock-screen unlock**, without a proprietary Windows driver, a swipe gesture,
or a separate Python authentication service.

**Status: working experimental implementation on one laptop, not an upstream
release or a security-certified biometric authenticator.** As of 9 October 2026,
the owner reports working GNOME unlock and satisfactory responsiveness after the
performance updates. Other hardware, population false-accept rates and resistance
to fingerprint spoofs have not been established. Keep password authentication
available and do not deploy this as the only way to access a machine.

## Hardware and scope

The tested device is an EH575 built into an **Acer Swift SF314-43**, with USB ID
`1c7a:0575` and device revision `0x1072`. The local system trial uses Ubuntu 26.04
amd64. The driver rejects untested device revisions and unexpected descriptors;
support for other EgisTec models or laptops is not implied.

The sensor produces an eight-bit **103 × 52 pixel** image: a narrow patch of the
finger pad, not a full fingerprint. The image API can expose an enlarged
206 × 104 image, but enlargement adds neither physical coverage nor new detail.

There are two build modes:

| Mode | Behavior | Intended use |
| --- | --- | --- |
| Default EH575 image driver | Stationary capture through `FpImageDevice`; existing NBIS/Bozorth3 matching remains unchanged | Protocol, acquisition and image-quality development |
| Opt-in `egis0575_ridge=true` | Native `FpDevice` enrollment and verification using an OpenCV ridge-image matcher | Experimental stationary recognition and the GNOME trial |

The ridge option defaults to **off** and requires an isolated build containing
only `egis0575`. It does not replace the matcher used by other libfprint drivers.
Do not assume that compiling the image-only driver is sufficient for dependable
fingerprint authentication.

## Why a different matching approach was necessary

Initial stationary captures yielded only two to four extracted minutiae. That
was insufficient for dependable recognition through the usual minutiae-based
path. We did not fabricate minutiae or lower the existing Bozorth3 threshold.

The small sensor also means that moving a finger exposes a different patch. An
enrollment concentrated on one spot could match that spot well but reject nearby
placements. Conversely, moving too far between enrollment touches creates patches
with little shared detail.

A swipe-reconstruction experiment was tried and retired after unreliable
real-reader results and impractical handling. The active implementation is a
**stationary press reader**: place the pad flat, hold still, then lift. The swipe
experiment remains in Git history; old swipe builds should not be used.

The experimental solution enrolls a bank of overlapping pad areas and directly
compares their ridge detail. It does not stitch them into a complete fingerprint.

## How acquisition, enrollment and verification work

Each scan session initializes the device. Fresh calibration sets its volatile
exposure with the reader empty and measures an empty-reader background. Calibration
controls clipping and brightness changes; background correction prevents fixed
sensor texture from becoming identity evidence.

The experimental persistent-profile update reuses a previously measured
empty-reader calibration, restoring its DC setting before capture. The protected
`EH575C2` profile is bound to the tested hardware revision and a hash of GUsb's
physical USB port identifier, not its temporary device address. It has no boot,
suspend or age expiry. A valid saved profile can therefore be tried immediately
after reboot/resume: touch and hold still. Changing the port identifier requires
a fresh profile. This is a port binding, not a hardware serial-number guarantee.
It contains an empty-sensor reference and calibration metadata, not a finger image
or template. The trial package stores it separately from fprintd's enrolled prints
in `/var/lib/eh575-libfprint/calibration` (directory 0700, files 0600).
Private fprintd tests use their own `calibration` directory. Without the explicitly
configured `FP_EH575_CALIBRATION_DIR`, reuse is in memory only.

Initialization, reply validation and a settling-frame discard still happen on
every operation. The next reading must be usable contact or a compatible idle
frame. Idle disagreement triggers fresh calibration; persistent raw clipping/low
contrast or failed ridge image-quality checks invalidate a reused profile and
request a lift. A successful idle-start reuse does not repeat the calibration
search or update the background. Cancellation/suspend/USB errors do not erase a
good measured profile, but dirty USB state still requires close/reopen. Background
correction, capture quality and matching thresholds are unchanged. A normal
`verify-no-match` never updates or deletes the profile, and no finger-present
capture is used as a new empty reference. File checksums detect accidental damage;
owner/permission checks protect local storage, not a certified biometric policy.

First use, a missing/invalid profile, or quality-triggered recovery still needs a
brief empty-reader measurement. The old epoch-bound `EH575C1` files are not
imported: upgrading requires one empty-start scan, but no re-enrollment.
Touching early without a usable profile requests
`verify-remove-and-retry` rather than immediately failing calibration. Stock
fprintd automatically restarts the retry while keeping its D-Bus verification
request active: lift briefly, then touch again. The restarted scan waits for lift
within a bounded 30-second calibration deadline. This acquisition change has
synthetic coverage; fresh GNOME/real-reader testing is still required.

After detecting a finger, capture allows 250 ms for contact to settle. Frames must
pass contrast/clipping and stationary-contact checks; movement restarts the burst
within a bounded deadline instead of demanding a swipe.

Enrollment uses **15 independent stationary touches**, three at each of:

1. Center of the finger pad.
2. A nearby area toward the fingertip.
3. A nearby area toward the finger base.
4. A nearby area toward the pad's left side.
5. A nearby area toward the pad's right side.

Use small overlapping placement shifts, not the nail or the extreme edge of the
finger. Keep the pad flat and covering most of the sensor. Move only between
touches, after lifting fully. Each accepted touch stores a median of three stable
frames together with its measured background.

Verification captures **five stable frames**. The matcher subtracts the measured
background, enhances ridge detail, builds a contact mask and searches bounded
rotation, translation, scale and affine alignments. It checks actual overlap,
ridge correlation, edge detail, agreement across three regions and separation
from competing alignments. At least **three of the five frames must pass against
one enrolled area under the same fitted transform**.

A probe does not have to match every enrolled spot. Evidence cannot be pooled
from unrelated enrolled areas to manufacture the required frame agreement.
Five frames from one touch are correlated observations, not five independent
security trials.

Templates use versioned raw libfprint data (`eh575-ridge-v1`). Type, schema, encoded
size, touch count and image lengths are checked. Matching runs in a cancellable
native worker; operations wait for that worker to stop before completing. Invalid
data, exceptions, timeouts and cancellation must not become successful matches.

Exact protocol details and experimental policy values are documented in
[the protocol notes](doc/egis0575-protocol.md).

## What changed in libfprint

The main additions are:

- **USB acquisition:** `libfprint/drivers/egis0575.c` and `egis0575.h` implement
  initialization, validated command/reply handling, image assembly, calibration,
  presence/lift detection, finite deadlines and transport cleanup. They use the
  existing userspace USB stack; no kernel module, firmware upload, kernel-driver
  detachment or USB reset is required.
- **Native ridge matching:** `egis0575-ridge.cpp`, `egis0575-ridge.h` and
  `egis0575-ridge-adapter.h` implement the optional matcher, enrollment storage and
  libfprint device callbacks. No new public API signatures or template imports
  from the earlier Python prototype are introduced.
- **Build and discovery:** Meson recognizes `egis0575`, gates OpenCV behind the
  experimental option, and the unsupported-device exclusion for this USB ID is
  removed.
- **Shared suspend cleanup:** `fp-device.c` permits an inactive device to close
  while suspended; `fpi-device.c` waits for an asynchronous close if resume races
  with cleanup. Active operations still prevent closing and new scans/opens stay
  blocked during suspension. This is a shared-core behavior change, not just a
  driver addition, and needs separate upstream review.
- **Tests and tools:** protocol, quality, matcher, correlation-equivalence,
  lifecycle and packaging tests; private real-fprintd supervision; replay tools;
  CI; protocol documentation; and reversible trial packaging.

The suspend fix addressed a real failure: a verification interrupted by sleep
could release its fprintd claim without closing the device, leaving later attempts
stuck with "The device has already been opened!". Tests now exercise release
**before** resume, including the asynchronous close/resume race.

### Performance work

The first speedup eliminated unnecessary detailed scoring during coarse search,
prioritized likely enrolled areas without filtering others out, and stopped after
one area fully passed verification. The second reuses per-comparison FFT
correlation transforms and rotated probe data, polls initial contact sooner, and
avoids rounding the end of the settling wait up to another full polling interval.

The search bounds, matching thresholds, five-frame capture, frame-agreement rule,
250 ms settling gate and 80 ms waits between captured samples remain unchanged.
Caches live only for one comparison and are not written to disk. Rejections still
search the entire enrollment bank.

Historical 32-probe replays preserved all acceptance/status decisions: 7/8 genuine
touches accepted and 0/24 wrong-finger touches accepted. In one first-speedup replay,
median genuine processing fell from about 4.4 seconds to 0.38 seconds. The second
pass reduced genuine processing by another roughly 22–24% and full wrong-finger
searches by about 30% in two runs. These are same-host, process-inclusive **offline
matcher measurements**, not live touch-to-unlock timings or population validation.

## How this connects to GNOME

The deployed path uses the existing Linux authentication stack:

```text
GNOME / GDM
    -> existing pam_fprintd
    -> Ubuntu's stock fprintd on the system bus
    -> this fork's native libfprint EH575 driver and ridge matcher
    -> USB reader
```

The GNOME trial does **not** replace fprintd, patch GNOME, install a Python matcher
as a system service, weaken PolicyKit, or rewrite PAM configuration. Stock fprintd
owns system enrollment, print storage and verification. Existing password
authentication configuration remains intact.

The trial package installs a root-owned native library under
`/opt/eh575-libfprint/lib` and a service-specific systemd drop-in that selects it
for fprintd, with an EH575-only driver allowlist. The distribution libfprint package
is retained. Existing fprintd service hardening is not removed, and no
user-writable home/build directory is added to the root daemon's library path.

This is nevertheless a meaningful system authentication change: a root daemon
loads an experimental matcher. It is not a lock-screen-only policy; existing
GNOME/GDM configuration may allow fingerprint login as well as unlock. Do not
enable fingerprint authentication for sudo, common-auth or other PAM services
as part of this trial.

## Build and test without changing authentication

The EH575 implementation is available on `master`. Its development history is
also preserved on branch `codex/egis0575`; the original upstream base is recorded
in the attribution section below.

```sh
git clone --branch master https://github.com/ivanov-gv/libfprint.git
cd libfprint
```

You need a C/C++ toolchain, Meson, Ninja, pkg-config, GLib development headers and
GUsb **0.3.3 or newer**. The ridge build additionally requires OpenCV **4.5 or
newer**, including core, imgproc, features2d, calib3d and video. Python 3 with
PyGObject/GIO and `dbus-daemon` is needed for private-bus tests. Python is used for
development tooling, not production fingerprint recognition.

With those dependencies available:

```sh
meson setup _build/eh575-ridge . \
  -Ddrivers=egis0575 -Degis0575_ridge=true \
  -Dintrospection=false -Ddoc=false -Dinstalled-tests=false \
  -Dudev_rules=disabled -Dudev_hwdb=disabled -Dwerror=true
meson compile -C _build/eh575-ridge
meson test -C _build/eh575-ridge \
  egis0575-protocol egis0575-driver egis0575-quality egis0575-surface \
  egis0575-ridge egis0575-ridge-device egis0575-fprintd-session \
  egis0575-package fpi-device fpi-ssm --print-errorlogs
```

This builds locally and runs selected synthetic/isolated tests. It installs
nothing, makes no hardware scans and changes no system authentication. Private-bus
tests need permission to create local Unix sockets. Use a separate build directory
and `-Db_sanitize=address,undefined` for sanitizer testing; do not deploy that build.

For image-only development, use a fresh build directory and omit
`-Degis0575_ridge=true`; omit the ridge-specific test suites too. OpenCV is not
required for that mode. **Do not run `sudo meson install` to replace your system
library.** Use the reviewed trial package for any deliberate system integration.

## Hardware checks before system installation

Unprivileged tests require access to the USB reader. Resolve USB permissions
through normal local device-access policy rather than running these helpers with
sudo or making the device world-writable. Only one process should use the reader;
do not run private and system fingerprint scans simultaneously.

The native hardware helper can capture without saving an image:

```sh
python3 scripts/eh575-run.py capture --build _build/eh575-ridge
```

For a separate native test enrollment and verification:

```sh
python3 scripts/eh575-run.py enroll --build _build/eh575-ridge
python3 scripts/eh575-run.py verify --build _build/eh575-ridge
```

These helpers never enable GNOME authentication. Their default ridge state is
`.state/eh575-ridge`, not system fprintd's storage. An existing native test print
is not overwritten; use `--state .state/another-trial` for a fresh test enrollment.

### Test with the real fprintd on a private bus

On the supported Ubuntu test environment, with the distribution fprintd daemon
and client tools already available:

```sh
python3 scripts/eh575-fprintd-session.py \
  --build _build/eh575-ridge --forward-sleep
```

In the private shell:

```sh
fprintd-enroll -f right-index-finger
fprintd-verify -f right-index-finger
exit
```

This runs **real fprintd**, loading the native build, on a fresh private bus with
separate storage at `.state/eh575-fprintd/prints`. Test-only authorization fixtures
stay on that bus; they must never be used for system authentication. Private
`fprintd-delete` affects private prints only. The `--forward-sleep` option subscribes
to real sleep notifications and forwards them to the private daemon; it does not
suspend the laptop or inhibit real sleep itself.

The supervisor also provides `-- check`, `-- cancel-test` and `-- sleep-test`.
Cancellation/simulated-sleep tests require a private enrolled print and an empty
reader. Before system installation, test enrolled and non-enrolled fingers,
cancellation, client disconnect and **physical sleep/resume in the same daemon
session**. Starting a new daemon after waking does not test recovery of the old one.
See [the trial guide](EH575-GNOME-TRIAL.md) for the complete sequence.

## Reversible Ubuntu/GNOME trial

The package builder is deliberately restricted to **Ubuntu 26.04 amd64 and the
OpenCV 4.10 runtime ABI**. Building source elsewhere does not make this package
portable. It requires a clean, committed source tree and an EH575-only ridge build.

After the private hardware/lifecycle checks, prepare an artifact without sudo:

```sh
python3 scripts/eh575-package.py \
  --build _build/eh575-ridge --output /tmp/eh575-reviewed-package
```

The output directory must be new. The builder prints the exact `.deb` filename
and produces `contents.txt` and `sha256.txt`. It stages the library to remove build
search paths and rejects retained RPATH/RUNPATH, sanitizer runtimes and an
unexpected OpenCV ABI. Preparing the package installs nothing.

Inspect that exact artifact and simulate the transaction first:

```sh
dpkg-deb --contents /absolute/path/to/reviewed-package.deb
dpkg-deb --info /absolute/path/to/reviewed-package.deb
apt-get --simulate --no-remove install /absolute/path/to/reviewed-package.deb
```

The paths above are placeholders: substitute the actual filename printed by the
builder. Review dependencies and reject unexpected removals or upgrades. Do not
act on unrelated autoremove suggestions. Keep a working terminal/TTY and password
access available. Only after deliberately approving the authentication change:

```sh
sudo apt-get --no-remove install /absolute/path/to/reviewed-package.deb
```

Installation reloads systemd and tries to restart **fprintd only**, not GDM. The
package contains the native library, service drop-in and documentation/metadata;
no personal prints, captures, Python matcher or test authorization fixtures ship.

Create a **fresh system enrollment** from a normal terminal, outside the private
shell, or through GNOME's account settings:

```sh
fprintd-enroll -f right-index-finger
fprintd-verify -f right-index-finger
```

Do not copy Python/private-test prints into system storage. Existing native system
enrollment remains compatible with the two performance updates; updating those
versions does not require re-enrollment. The stock client/UI may not show the
private helper's detailed calibration and area prompts, so follow the 15-touch
placement sequence described above.

Before relying on lock-screen use, check multiple genuine matches, wrong-finger
rejections, password fallback, physical suspend/resume and cold boot. Any
wrong-finger match is a reason to stop and investigate, not lower thresholds.
The package does not automatically enable unrelated PAM services or guarantee
GNOME's fingerprint policy on another machine.

### Rollback

```sh
sudo apt remove libfprint-eh575-experimental
```

Removal deletes the package-owned override and library, reloads systemd and
tries to restart fprintd so it uses the distribution library again. It does not
erase private test data or system fingerprint enrollments. Stored prints are
deliberately preserved; remove them separately through the appropriate fingerprint
service only if you intend to delete that biometric data. Password configuration
is not changed by this package.

## Known limitations and troubleshooting

- **Missing or unusable calibration still needs a brief lift.** A valid profile
  is tried across reboot/resume, with no one-hour expiry. Without one, or when its
  readings fail quality checks, early contact produces a
  recoverable remove-and-retry request. Lift briefly and touch again; leaving the
  reader covered until the deadline can still end the attempt. GNOME's own PAM
  timeout may be shorter. This is not yet a promise of touch-and-wait after every
  hardware reset: saved settings can still become unusable. Other USB/exposure
  errors need diagnosis. Actual reboot/resume profile reuse still needs fresh
  hardware testing; passing synthetic tests does not establish reliability.
- **Small placement changes still need overlapping detail.** Keep the finger
  flat and covering the strip. Partial/poor contact can produce retries. A retry
  named `swipe-too-short` by the generic fprintd API does not mean this press
  driver requires swiping.
- **A rejected comparison is not always a driver failure.** `verify-no-match`
  means the capture did not satisfy recognition checks. Do not loosen thresholds
  just to make an isolated failed attempt pass.
- **Errors and device-busy conditions need diagnosis.** Lift, stop competing
  clients and retry with a fresh claim. Interrupted or inconsistent USB transactions
  deliberately require close/reopen instead of reusing possibly stale replies.
- **Permission denied is not a matching problem.** Check local USB access for
  unprivileged tests; do not use sudo with the private supervisor. The system trial
  uses stock fprintd's normal privileges and authorization.
- **Single-finger verification is implemented; identification is not.** The ridge
  path does not implement libfprint's identify operation. Duplicate-detection
  functionality that depends on identification is not established.
- **Authentication assurance is limited.** There is no liveness/spoof detector,
  independent security review or population validation. A handful of rejected
  fingers does not establish a false-accept rate. This is not upstream-approved.

For system failures, inspect `journalctl -u fprintd` and the installed build
identity in `/usr/share/doc/libfprint-eh575-experimental/build.json`. For native
development, the helper accepts `G_MESSAGES_DEBUG=libfprint-egis0575` for scoped
scalar diagnostics. Avoid broad debug logs and never attach templates or raw
fingerprint images to public issues.

## Evidence, privacy and remaining work

Development proceeded from a separate Python acquisition/matching prototype to
a native image driver, the retired swipe experiment, stationary native ridge
recognition, isolated real-fprintd tests, suspend cleanup fixes, reversible system
integration and two performance passes. The installed GNOME trial uses the native
implementation, not the earlier prototype or its private D-Bus bridge.

Ten selected regression suites and five selected sanitizer suites passed during
the second performance pass. Physical same-daemon suspend recovery and wrong-finger
rejection were exercised during development; the owner subsequently reported
working system enrollment, GNOME unlock and improved latency. These are limited
local functional results. The complete upstream test matrix, introspection/USB
emulation coverage, additional hardware and independently repeated system
password-fallback/cold-boot checks are not all established by those results.

Prints contain raw fingerprint-derived data and backgrounds, **not hashes**.
Private helpers use restrictive state-directory/file permissions; system prints
are managed by fprintd in its normal state directory. There is no claim of
additional encryption at rest. `.state/` and `.captures/` are ignored by Git;
do not publish their contents, upload biometric fixtures, or include them in
binary releases. Replay tools process private recordings in memory and report
aggregate diagnostics; NumPy is optional diagnostic tooling, not a runtime matcher.

Before an upstream submission, separate the acquisition driver, experimental
matcher/dependency proposal and shared-core cleanup fix into reviewable changes.
Follow [HACKING.md](HACKING.md), extend upstream-compatible tests, gather more
hardware/security evidence and review license attribution for redistributed
binaries. The local deployment package is trial infrastructure, not a proposed
replacement for distribution packaging. A GitHub pull request can review this
fork; libfprint upstream contributions go through its GitLab merge-request process.

Further documentation:

- [Development history, build details and evidence](EH575-DEVELOPMENT.md).
- [GNOME trial, lifecycle gates and rollback](EH575-GNOME-TRIAL.md).
- [EH575 USB protocol, acquisition and template format](doc/egis0575-protocol.md).
- [Upstream contribution guidance](HACKING.md) and [libfprint API documentation](https://fprint.freedesktop.org/libfprint-dev/).

## Upstream, attribution and license

libfprint is part of the [fprint project](https://fprint.freedesktop.org/). The
upstream library and its other drivers remain the foundation of this fork. The
development base is `6f9479c3d55f847c1b3769f28ceb99227f9858cf`; `origin` is
[this GitHub fork](https://github.com/ivanov-gv/libfprint) and `upstream` is
[the original GitLab repository](https://gitlab.freedesktop.org/libfprint/libfprint).

Library contributions here use **LGPL-2.1-or-later**; see [COPYING](COPYING) and
individual file notices. Upstream libfprint includes NIST NBIS code with its own
notices. The optional ridge matcher bypasses NBIS for EH575 verification; it does
not remove or relicense the bundled upstream code.

EH575 initialization tables derive from the MIT-licensed
[SandroRoque/python-egistec-eh575](https://github.com/SandroRoque/python-egistec-eh575),
commit `38de042e0103607c83f7ec4b0497504449e7bd5e`. Original EH575 research by
[Animeshz/EgisTec-EH575](https://github.com/Animeshz/EgisTec-EH575) informed the work.
The native ridge approach follows the separate MIT-licensed prototype. Retain
[the reference notice](libfprint/drivers/egis0575-reference-MIT.txt) and
[the prototype notice](libfprint/drivers/egis0575-prototype-MIT.txt), together with
applicable upstream notices, when redistributing derived work. No vendor DLL,
proprietary matcher or firmware blob is included.
