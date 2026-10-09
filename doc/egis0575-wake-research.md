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

## Acer OEM package cross-check

On 2026-10-09, the Acer-hosted SF314-43 package
[Fingerprint EGISTEC 3.7.1.1](https://global-download.acer.com/GDFiles/Driver/Fingerprint/Fingerprint_EGISTEC_3.7.1.1_W11x64_A.zip?acerid=637715897608988026)
was downloaded for static inspection only. No installer, DLL or command file was
executed, and no firmware resource was sent to the reader. The downloaded files
stay outside this repository. This is an OEM package dated 2021, not a claim that
it is the latest driver or the version formerly installed on the owner's laptop.

Reproducibility hashes (SHA-256):

- ZIP: `9de04ccb27244583a2788987860baa6dc331cc329ec73c33fb71a0b143d1459e`
- `EgisTouchFP0575.inf`: `059dce1967205ab928add642077d3d08aa90418c5da78bb473740d9f4d03c3cd`
- `EgisTouchFP0575.dll`: `25704878eb4b15b41bf9389d2af0c6e44e830ad9c890f4fef2e710110c7b2631`
- `EgisTouchFPSensor0575.dll`: `cd2579797ba4f4694d8813a3d99a86e7bf1ddd1d32b755976bb41e536c2120e7`

The INF targets `USB\\VID_1C7A&PID_0575`; its DriverVer is
`06/15/2020,3.7.1.1`. It enables device idle, sets a 10000 ms default idle timeout,
assigns power-policy ownership away from WinUSB, and sets the vendor preference
`RemoteWakeupEnable` to 1. Separately, it sets `WdfDefaultWakeFromSleepState` to 0.
Those two values must not be read as a contradiction or proof of a hardware limit:
[Microsoft documents](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/user-control-of-device-idle-and-wake-behavior)
that the framework default is consulted only with particular driver-selected
user-control/enabled settings. Runtime policy, modern-standby behavior and the
owner's previous Windows configuration remain unobserved.

Read-only PE/string inspection confirms separate detection parameters, including
gain, voltage reference, DC components and high/low detection thresholds, alongside
the older named detection-calibration settings. In this binary, references to
the detection-mode label occur near VA `0x180012625`, and the resume label near
`0x1800123eb`. These differ from the published archive's addresses: do not apply
its function/vtable addresses directly to this OEM version. The DLL also includes
multiple sensor families and firmware-related strings. A string's presence does
not establish a USB command, active code path, or compatibility with revision 1072.

This cross-check strengthens the separate-detector hypothesis but does not yet
provide a complete, reversible wake-arming sequence. In particular, the correct
EH575 device-method dispatch, calibration values and low-power event transport
still need to be resolved before any new hardware command is tested.

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
