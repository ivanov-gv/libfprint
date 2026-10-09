# Reversible EH575 GNOME trial

This is an experimental local trial, not an upstream-ready or security-certified
authentication implementation. It can enable fingerprint login as well as GNOME
unlock; it is not a lock-screen-only policy. Password authentication must remain
available. Spoof/liveness resistance and population false-accept risk are unknown.

## Evidence and local deployment status

On 2026-10-09 real fprintd completed a fresh 15-touch enrollment, matched three
enrolled-finger trials, rejected three completed non-enrolled-finger trials, and
matched once in a new daemon session after physical sleep/resume. An early touch
during calibration was rejected with an error, not accepted.

The developer then ran the enrolled private session's EMPTY-reader VerifyStop,
client-disconnect/reclaim, and simulated sleep/resume/reclaim tests on the real
reader. All completed successfully; no template was modified. The suspend warning
`Cannot run while suspended` is expected: an in-flight verification is interrupted,
not accepted. Nine selected synthetic/native/guard suites also pass.

The first physical same-daemon test on 2026-10-09 FAILED when sleep interrupted an
active verification: stock `fprintd-verify` released before resume, libfprint
rejected close while suspended, and fprintd dropped the claim while the device
stayed open. Later claims failed with `The device has already been opened!`.
The earlier simulated test missed this because it released after resume.

The branch now contains a local core cleanup fix (separate upstream review
required): completed operations may close while suspended, and resume waits for
any asynchronous close. It does not permit new scans/opens during suspension or
closing an active/preserved scan. The strengthened real-reader test uses the
stock client, releases BEFORE simulated wake and successfully reclaims in the
SAME daemon. Native worker and generic async-close/resume race tests also pass.
Do NOT install the old `de8e9eab` package; it predates this fix.

The user then completed repeated PHYSICAL same-daemon sleep/resume tests with the
fix on 2026-10-09, including interruption while awaiting a finger and after image
capture. Interrupted scans returned an error, without release/open errors. Three
subsequent enrolled-finger scans matched; a separate post-resume non-enrolled
finger scan rejected. Both sessions exited cleanly. These are small functional
checks, not security certification or population false-accept measurements.

With explicit user approval, the user installed
`libfprint-eh575-experimental` version `0.1+git.4ab3b17e0b90` on the development
laptop. Apt completed installation of the trial plus 16 runtime/dependency
packages, with no upgrades or removals. Dependency setup also selected Ubuntu's
OpenBLAS providers for the system BLAS/LAPACK alternatives; the trial did not
independently edit those alternatives or run autoremove.

Post-install read-only checks confirmed:

- Stock system fprintd is active and discovers ONE native EH575 press device with
  15 enrollment stages on the REAL system bus, not the private test bus.
- The service selects `/opt/eh575-libfprint/lib` through the package-owned drop-in.
  ProtectSystem=strict, ProtectHome, PrivateTmp, MemoryDenyWriteExecute and
  NoNewPrivileges remain enabled; StateDirectory=fprint is mode 0700.
- Deployment directories/library/drop-in are root-owned and not user-writable.
  The installed library SHA-256 matches the package manifest:
  `0bc5d3f24ffd1185b9cb325e3473c50816b504c0ef9035311dcae33f388308fb`.
  Its ELF has no RPATH/RUNPATH or sanitizer dependency.
- Checksums of common-auth, gdm-password, gdm-fingerprint and the stock fprintd
  executable match the pre-install baseline. Distribution libfprint is retained.
- At the initial post-install check there was NO system enrollment; no private
  template was copied. A fresh system enrollment was subsequently completed.

Reading the root daemon's `/proc/PID/maps` required an interactive sudo password,
so direct mapped-path inspection was not completed. Native device discovery,
service configuration and installed artifact identity were checked independently.
The user subsequently completed ordinary system enrollment (15 touches, with a
recoverable retry), an enrolled-finger match and four completed non-enrolled-finger
rejections. The other `verify-unknown-error` results corresponded to journal
errors `Keep the EH575 reader empty during calibration`, not completed comparisons.
The user reported GNOME unlock working, but with roughly three seconds of matching
latency. Explicit separate password-fallback, system-service sleep/cold-boot
results and fresh testing of the faster update remain unconfirmed. Do not treat
these small functional checks as security certification.

The speed update keeps the same stored template format and thresholds; existing
system enrollment can be reused. Package versions starting `0.2+git.COUNT.HASH`
sort after the original `0.1` trial and increase for descendant checkpoints.
Use normal `apt-get --no-remove install /absolute/path/to/reviewed-update.deb` for
an approved update: `--no-upgrade` would prevent updating this installed package.
Review that only this trial package changes. No re-enrollment or PAM/GDM change is
part of the performance update; repeat genuine/wrong-finger and suspend checks.

The user subsequently installed the first speed update (`ae5de72c`) and reported
GNOME unlock "Much better". The next optimization reuses per-comparison FFT
correlation transforms and reduces initial-contact polling waits, not the settling
gate or verification-sample spacing. On two historical replays it reduced median
genuine matcher time by about 22-24% and full wrong-finger search by about 30%, with
all 32 decisions agreeing with the installed version. This is not measured live
unlock latency or fresh validation. The next package is prepared separately; no
system service is changed by building/testing it. Existing enrollment is compatible.

For another installation, repeat the physical same-daemon lifecycle and
wrong-finger gates BEFORE installation. A separate session started after wake
does not test that lifecycle. The private harness does not test real system
PolicyKit, service hardening or GNOME password fallback.

## Physical sleep/resume without changing authentication

Run from a normal terminal in this clone, without sudo:

```sh
python3 scripts/eh575-fprintd-session.py \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root \
  --forward-sleep
```

Inside that private shell:

1. Run `fprintd-verify -f right-index-finger` and verify an enrolled-finger match.
2. Start a SECOND `fprintd-verify -f right-index-finger`, keep the reader empty,
   wait for `Reader calibrated`, and suspend DURING that pending verification
   using the normal desktop control. Leave the shell and daemon RUNNING.
   Resume and unlock with your existing password.
3. The supervisor should print forwarded sleep notifications `True` then `False`.
   The interrupted verification may end with `verify-unknown-error`; that is
   expected and is not a match. There must be NO `ReleaseDevice failed` error.
4. In the SAME private shell, run `fprintd-verify -f right-index-finger` again.
   Also repeat with a non-enrolled finger, which must not match.
5. Type `exit`. Send the complete scalar output; never send fingerprint files.

Forwarding is a read-only subscription to the real logind signal. The test fixtures
remain on the private bus; they never own real PolicyKit/logind/fprintd names or
delay actual sleep. The supervisor does not suspend the machine itself.

## Installation plan requiring explicit approval

The prepared Debian package is named `libfprint-eh575-experimental`. It is deliberately
limited to this Ubuntu 26.04 amd64 environment and the OpenCV 4.10 runtime ABI.
Its package builder refuses a dirty source tree or another driver configuration.

It contains ONLY:

- A Meson-staged native libfprint library and SONAME link under
  `/opt/eh575-libfprint/lib`, root-owned through dpkg.
- The package-owned service drop-in
  `/usr/lib/systemd/system/fprintd.service.d/60-eh575-native.conf`, selecting that
  directory ONLY for the existing stock fprintd daemon, with the EH575 driver allowlist.
- Licenses, experimental-trial instructions, library checksum and source revision.
- Maintainer scripts that reload systemd and try-restart ONLY fprintd on installation
  or removal. They do not enable a service or restart GDM.

The builder uses Meson's DESTDIR staging to remove the test library's `/tmp`
RUNPATH. It rejects any remaining nonempty RPATH/RUNPATH or sanitizer runtime.
Never deploy a root authentication service loading user-writable `/tmp`/home code.
No Python helper/matcher, test authorization fixture, biometric template or capture
is shipped. No distribution libfprint/daemon file, PAM file, D-Bus policy, PolicyKit
policy, udev rule or GNOME setting is overwritten.

The package depends on Ubuntu's normal OpenCV runtime packages (not development
packages). The current apt simulation proposes 16 new runtime/dependency packages,
zero upgrades and zero removals. Apt may report unrelated autoremove suggestions;
DO NOT run autoremove as part of this trial. Review the actual transaction again
immediately before installation.

The inspected laptop already has stock `gdm-fingerprint` calling `pam_fprintd.so`,
a separate `gdm-password` service, no fprintd systemd drop-ins, and no fingerprint
module in `common-auth`. Do NOT run pam-auth-update, change common-auth, or enable
fingerprint sudo/TTY authentication for this trial.

Preparing the artifact does not install dependencies or change a service.
Building with normal installed development dependencies:

```sh
python3 scripts/eh575-package.py \
  --build /tmp/eh575-libfprint-ridge-build --output /tmp/eh575-gnome-package
```

On the development laptop the temporary dependency environment used by the native
build must be supplied too, with `--meson /tmp/eh575-native-deps/root/usr/bin/meson`.
The already prepared artifact's exact filename and checksum are printed by the
builder; these temporary paths may disappear across reboots. Do not substitute an
unreviewed package or use an artifact from a dirty/unknown revision.

Before a HUMAN-APPROVED installation, inspect the EXACT artifact:

```sh
dpkg-deb --contents /absolute/path/to/reviewed-package.deb
dpkg-deb --info /absolute/path/to/reviewed-package.deb
apt-get --simulate install /absolute/path/to/reviewed-package.deb
```

Only after the lifecycle gate and explicit consent:

```sh
sudo apt install /absolute/path/to/reviewed-package.deb
```

This installs a root-loaded experimental authentication library and changes the
system fprintd service's library selection. Unlike the private test, real system
PolicyKit and GNOME/PAM now apply. It is a meaningful authentication change even
though PAM files remain untouched.

## System trial sequence (only after approved installation)

Use a NORMAL terminal, not the private shell. No private-bus environment variables
may be set. Start with CLI access before trying the lock screen:

```sh
fprintd-list "$USER"
fprintd-enroll -f right-index-finger
fprintd-verify -f right-index-finger
```

Create a new SYSTEM enrollment; no private test prints are copied. Keep the sensor
empty at startup, then use the same 15 stationary touches with small overlapping
area changes. The stock CLI/GNOME UI may not show the private helper's calibration
or guided-area text. In the early-touch update, a compatible saved empty-reader
calibration allows immediate touch-and-hold. Without one, early contact requests
remove-and-retry rather than aborting immediately: lift briefly and touch again.
The new persistent-profile update tries reuse across reboot/resume without age
expiry. It binds to the physical USB port and hardware revision and checks current
readings; unusable profile/image quality requests lift and fresh calibration.
After upgrading from `EH575C1`, seed `EH575C2` with one empty-start scan; existing
prints do not need re-enrollment. The package stores only empty
sensor calibration in root-owned `/var/lib/eh575-libfprint/calibration`, separate
from prints; no matching thresholds or template formats change.
The system PolicyKit agent may request your password for enrollment.

Check several enrolled-finger matches and non-enrolled-finger rejections first.
Any wrong-finger match blocks deployment. Check ordinary password authentication
still works. Keep a terminal open and know the rollback command. Then:

1. Check GNOME Settings -> System/Users -> Fingerprint Login sees the reader/print.
2. Lock via the normal desktop action. Keep empty initially; test fingerprint
   unlock, then separately test password unlock with the reader untouched.
3. Test real suspend/resume and password fallback.
4. Check cold-boot behavior only once lock/password recovery is proven.

For the early-touch update, seed the cache with a successful empty-start scan.
Then try immediate touch-and-hold with enrolled and non-enrolled fingers, both
before and after fprintd's normal idle exit. Expect matches and rejections,
respectively, without a calibration error. After real suspend/resume AND reboot,
try holding the enrolled finger on the sensor as soon as the verification/login
screen is ready: a usable saved profile should avoid an empty-start requirement.
If quality recovery requests `verify-remove-and-retry`, lift briefly and touch
again within the same client request. Repeat immediate wrong-finger rejection,
password fallback and GNOME
lock. Preserve journal errors if any terminal unknown error remains. Until these
fresh trials pass, cache/retry behavior is development-tested, not hardware-proven.
5. Record failures and roll back rather than weakening matcher thresholds or
   disabling service protections.

GNOME officially supports fingerprint login through the user account settings;
fingerprint authentication can also be disabled by system policy. Consult
[GNOME fingerprint administration](https://help.gnome.org/system-admin-guide/login-fingerprint.html).
The service-scoped override follows
[systemd drop-in semantics](https://github.com/systemd/systemd/blob/main/man/systemd.unit.xml).
No GNOME setting is changed automatically by this package. If an administrator
disabled fingerprint authentication, resolve that deliberately rather than
overriding their policy.

## Optional touch-to-wake permission trial

The owner's initial trial did not wake the display. The available journal did not
show a new system suspend after the service started, so it does not establish a
hardware suspend-wake failure. Both a blanked screen and real suspend are now
requirements. See [EH575-TOUCH-RESEARCH.md](EH575-TOUCH-RESEARCH.md) for isolated
Linux-only contact/interrupt tests; the automatic integration is not implemented.

The package's `eh575-wakeup.service` is opt-in; installation never enables it.
It enables USB remote-wake permission for the tested reader and its hub ancestors
and reapplies it just before system sleep, since libfprint can reset the policy
at discovery/resume. PCI/platform wake must already be enabled. No undocumented
sensor wake command or authentication bypass is implemented. Enabling a shared
hub can allow other attached devices to wake too.

```sh
/usr/libexec/eh575-wakeup status
sudo systemctl start eh575-wakeup.service
```

Have password fallback and another wake method available. Lock/suspend normally,
wait until asleep, then touch and hold. Confirm both actual wake and GNOME unlock;
an enabled wake attribute alone proves neither. A wrong finger may wake but must
not unlock. Repeat suspend cycles. If reliable, opt in at boot with
`sudo systemctl enable eh575-wakeup.service`. Disable/restore with
`sudo systemctl disable --now eh575-wakeup.service`.

Original settings are stored only in a protected `/run` journal and restored on
stop for freshly validated current reader/hub paths. Restoration failures retain
an inactive journal and are reported; a reboot clears transient settings. Inspect
`journalctl -b -u eh575-wakeup.service -u systemd-suspend.service` on failure.
If touch does not wake, disable the trial: sensor-side wake arming remains an
open protocol investigation. No hardware wake-on-touch result has been established
for this new trial; it does not promise hibernation or powered-off wake.

## Rollback

From your still-open terminal or a password-authenticated TTY:

```sh
sudo apt remove libfprint-eh575-experimental
```

Dpkg stops/disables the optional wake service and restores its recorded policy,
then removes this package's library, drop-in, docs and wake helper/service/hook.
The removal hook reloads
and try-restarts fprintd, restoring Ubuntu's original library selection. It does
not remove fprintd/libpam-fprintd, overwrite PAM, or restart GDM. Existing system
biometric prints are preserved, as are all private test enrollments. Keep runtime
dependencies initially; remove only explicitly reviewed unneeded packages later.

Verify `systemctl show fprintd.service -p DropInPaths -p Environment` no longer
selects `/opt/eh575-libfprint`. Distribution libfprint may not support this sensor
again; password unlock should be unaffected. If package hooks fail, report that
failure instead of assuming rollback completed. Other future custom drop-ins or
service changes need independent review.
