# EH575 hardware wake investigation

## Status: not implemented

The current USB-permission helper failed to produce touch wake in two owner-run
real s2idle trials on 2026-10-09. Awake image polling detects both contact and
release, but interrupt endpoints 83/84 were silent before/after our known capture
initialization. None of this proves hardware wake is impossible. No undocumented
command or firmware has been sent to the development laptop in this investigation.
The installed working fingerprint matcher and templates are unchanged.

## Published static-analysis leads

The public [EH575 research archive](https://github.com/Animeshz/EgisTec-EH575)
was inspected at commit `57fa58a2b39a67869645dbaad4f3d12a6a67ec99`. Its published
analysis of EgisTouchFP0575 was read as data, not executed. No vendor implementation,
binary, decompiled body, database, raw capture or biometric sample is copied into
this repository. The archive's decompilation contains explicit warnings and
multiple device classes, so it is a lead rather than a verified specification.

Relevant facts in
[the archived analysis](https://github.com/Animeshz/EgisTec-EH575/blob/57fa58a2b39a67869645dbaad4f3d12a6a67ec99/findings/EgisTec-EH575/decompiled_source/ghidra/EgisTouchFP0575.c):

- `FUN_180013dc0` (lines 13729–13824) loads a remote-wake preference and separate
  detection-calibration settings. Their values are not established for our unit.
- `FUN_180014ed0` (lines 14335–14444) has detection-mode entry/exit parameter
  paths. They depend on those stored settings and indirect device methods.
- `FUN_180014cb0` (lines 14240–14314) restores parameters on resume and can adjust
  a detection threshold. This differs from merely restoring the capture DC value.
- `FUN_18001a2a0` (lines 17372–17446) loads remote-wake preference and invokes
  initialization/calibration indirectly; resolving the correct derived-device
  methods matters. The published class/vtable reconstructions disagree in their
  granularity and cannot be treated as direct USB mappings.

Inference: a separate device-side detection calibration/arming path is a plausible
missing piece. This does NOT establish its exact registers, payload values,
transport, applicability to revision 1072, or whether wake reaches USB or a
platform sideband signal. No arbitrary values should be substituted for the
unresolved settings. The archive's capture descriptions identify browser/lock-screen
sessions, not a verified suspend-and-finger-wake trace.

Image exposure calibration and wake-detection calibration must not be conflated.
Our protected EH575C2 empty-image/DC cache is for capture quality, not proof that
the Windows detection parameters have been measured or restored.

## Linux-side evidence to collect

The opt-in helper now emits one `EH575 sleep snapshot:` JSON record per pre/post
hook while its protected state is active. Pre reapplies and verifies reader/hub
permission. Post observes only. Records include the selected USB nodes, platform
wake permission, available runtime/wake attributes, sleep mode, last wake IRQ,
monotonic clock and boot-time clock. They contain no serial, USB payload, image or
template. Missing optional attributes are null, not zero. A last IRQ is not an
unambiguous device identity, and counters need before/after comparison.

Read the fresh cycle with `journalctl -b -u systemd-suspend.service`. Confirm real
kernel suspend entry/exit separately. Our hook runs after logind's delayed suspend
cleanup, but system-sleep hooks can run in parallel; a snapshot is not atomic with
the kernel suspend boundary. Post may observe policy already changed by resume.
Logging cannot prove that the sensor internally armed a detector or retained power.

Next protocol work needs the correct EH575 derived-device mapping, the transport
of the detect-mode operation, bounds and origin of its calibration parameters,
and a characterized reversal back to ordinary capture. A current OEM driver
package can be analyzed without installing Windows or running its binaries; exact
version and hash must be recorded if acquired. Only once the full reversible
sequence is established should an isolated opt-in hardware arming test be added.
No package should silently arm wake, alter PCI/ACPI policy, keep the CPU awake,
poll during suspend, or treat a contact event as authentication.
