# Reversible EH575 GNOME trial (not yet installed)

This is an experimental local trial, not an upstream-ready or security-certified
authentication implementation. It can enable fingerprint login as well as GNOME
unlock; it is not a lock-screen-only policy. Password authentication must remain
available. Spoof/liveness resistance and population false-accept risk are unknown.

## Evidence and remaining gate

On 2026-10-09 real fprintd completed a fresh 15-touch enrollment, matched three
enrolled-finger trials, rejected three completed non-enrolled-finger trials, and
matched once in a new daemon session after physical sleep/resume. An early touch
during calibration was rejected with an error, not accepted.

The developer then ran the enrolled private session's EMPTY-reader VerifyStop,
client-disconnect/reclaim, and simulated sleep/resume/reclaim tests on the real
reader. All completed successfully; no template was modified. The suspend warning
`Cannot run while suspended` is expected: an in-flight verification is interrupted,
not accepted. Nine selected synthetic/native/guard suites also pass.

Still required before installation: verify with a real finger after those checks,
and a physical sleep/resume trial while ONE private daemon stays running and receives
real logind notifications. A separate session started after wake does not test that
lifecycle. This harness does not test real system PolicyKit, service hardening or
GNOME password fallback. The system trial checks those separately.

## Physical sleep/resume without changing authentication

Run from a normal terminal in this clone, without sudo:

```sh
python3 scripts/eh575-fprintd-session.py \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root \
  --forward-sleep
```

Inside that private shell:

1. Run `fprintd-verify -f right-index-finger` and verify an enrolled-finger match.
2. Leave that shell and daemon RUNNING. Suspend using the normal desktop control.
   Resume and unlock with your existing password.
3. The supervisor should print forwarded sleep notifications `True` then `False`.
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
or guided-area text. An early finger can abort calibration; remove it and retry.
The system PolicyKit agent may request your password for enrollment.

Check several enrolled-finger matches and non-enrolled-finger rejections first.
Any wrong-finger match blocks deployment. Check ordinary password authentication
still works. Keep a terminal open and know the rollback command. Then:

1. Check GNOME Settings -> System/Users -> Fingerprint Login sees the reader/print.
2. Lock via the normal desktop action. Keep empty initially; test fingerprint
   unlock, then separately test password unlock with the reader untouched.
3. Test real suspend/resume and password fallback.
4. Check cold-boot behavior only once lock/password recovery is proven.
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

## Rollback

From your still-open terminal or a password-authenticated TTY:

```sh
sudo apt remove libfprint-eh575-experimental
```

Dpkg removes ONLY this package's library, drop-in and docs. The removal hook reloads
and try-restarts fprintd, restoring Ubuntu's original library selection. It does
not remove fprintd/libpam-fprintd, overwrite PAM, or restart GDM. Existing system
biometric prints are preserved, as are all private test enrollments. Keep runtime
dependencies initially; remove only explicitly reviewed unneeded packages later.

Verify `systemctl show fprintd.service -p DropInPaths -p Environment` no longer
selects `/opt/eh575-libfprint`. Distribution libfprint may not support this sensor
again; password unlock should be unaffected. If package hooks fail, report that
failure instead of assuming rollback completed. Other future custom drop-ins or
service changes need independent review.
