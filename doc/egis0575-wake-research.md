# EH575 hardware wake investigation

## Status: not implemented

The current USB-permission helper failed to produce touch wake in owner-run
real s2idle trials on 2026-10-09, including a logged cycle at 14:18 local time.
Awake image polling detects both contact and
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

Initial leads in
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

The first three named paths above are in an older/generic device branch, not
the identified 5-series detector path. In particular, the archive's exported
vtable lists sometimes concatenate adjacent tables. An index in that JSON is
not a reliable byte offset in a device's actual vtable. The 5-series paths were
subsequently cross-checked against the OEM binary and register transactions below.

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
gain, voltage reference, DC components and high/low detection thresholds. The
previously noted references near `0x180012625` / `0x1800123eb` belong to the older
named parameter/resume branch, not the following 5-series detector functions.
The OEM binary uses image base `0x180000000`; `.pdata` function ranges and direct
calls establish these version-specific landmarks:

| Operation / diagnostic label | OEM function VA |
| --- | --- |
| `calibrate_detect_mode_5_series` | `0x180008814` |
| `fp_tz_secure_set_detect_mode` | `0x180009d0c` |
| `fp_tz_secure_set_detect_mode_exit` | `0x180009f4c` |
| `fp_tz_secure_set_sensor_mode` | `0x18000a1a0` |
| `finger_detect` | `0x18000ac9c` |
| `get_image send EGIS_WAIT_INTERRUPT` | `0x18000b178` |

The detector wrapper at `0x18000bd74` is called by `0x1800186bc` (call instruction
`0x18001872c`); a nonzero argument selects detector entry, while zero selects a
separate idle path. Calibration wrapper `0x18000be70` calls the calibration routine
and persists the resulting detection fields. This is static data flow, not proof
of which Windows power transition invoked these methods on the owner's laptop.
The DLL includes several sensor families; do not reuse addresses from the older
published binary or infer revision-1072 applicability from names alone.

### Register-level observations, not a hardware recipe

The 5-series detector path has different gain/reference/DC values from capture.
Its calibration uses register bank `0x34`/`0x35`, then adjusts a DC component using
a measured mean. One measurement branch uses `0x2c`/`0x2d` and reads statistics
at `0x67`; another requires software image/bad-pixel handling. The high threshold
is derived from the measured mean plus a separate margin. Capture exposure DC
cannot simply substitute for these fields.

Further OEM inspection located that margin at VA `0x18004a7b0`: its initialized
byte is `0x50` (80). The register-statistics path returns min/max/mean in that order.
The two entry-variant globals at `0x18004c100`/`0x18004c104` are initialized to zero;
their direct static references are reads, and the zero-variant command layout
matches all four published USB traces. These facts support an isolated gated
probe, not proof of all indirect writes or every firmware variant. The recorded
traces use previously stored detector settings; none runs auto-calibration at
`0x34`. On this laptop those settings must be measured, not replayed from a trace.

Detector entry programs its ROI and analogue settings, enters low-power mode,
then writes six registers in descending order starting at `0x45`. Its register
order is low threshold, high threshold, `87`, `13`, `00`, `03`. A variant branch
instead uses eight descending registers starting at `0x47`. The observed transport
uses `EGIS` opcode `0x71` for descending writes, not opcode `0x63` (ascending).
Variant selection and calibrated thresholds must not be guessed.

The corresponding exit changes `0x0a`/`0x0c`, clears `0x40`, waits for its busy bit,
and uses a descending write starting at `0x02`. Capture mode restores its own
analogue settings and ROI afterward. Some OEM paths do not propagate every write
failure; a Linux implementation must check all transfers, bound waits, and restore
capture on failure rather than reproduce that behavior.

### Cross-check against published Windows USB traces

Only short outbound register commands were examined; image transfers were not
decoded, printed or copied. All four `575-0` through `575-3` captures at the pinned
archive commit contain one six-register descending detector write and one
two-register exit write. For reproducibility, `logs/575-0.pcap` has SHA-256
`b18fbab2f5e92222e3c3ba70d1856dce831f498b615c8d0b5fdc7702e82277cd`.

That trace records ROI `06 60 06 05 2f 06`, detector gain `0a`, reference `03`,
DC components `0c`/`17`, and detector bank `00 ac 87 13 00 03`. These are observations
from another capture, **not defaults for this laptop**. The register ordering and
mode-transition pattern agree with the 5-series binary. Following entry, the
trace repeatedly polls register `0x01`; that is not proof that interrupt endpoint
83/84 or USB remote wake works during host suspend. There is no verified physical
suspend-and-touch-wake event in these traces.

### Reproducible offline inspection

`scripts/eh575-driver-report.py /absolute/path/to/EgisTouchFP0575.dll` prints a
JSON report of the SHA-256, landmark addresses, containing function ranges,
direct calls/callers and indirect-call counts. It accepts only the exact OEM DLL
hash above. Obtain/extract the OEM files separately and keep them outside the
repository. Use a trusted local copy: the before/after check detects ordinary
changes, not hostile replace-and-restore races.

The script uses the standard library and `/usr/bin/objdump`; it never loads the
DLL, downloads files, extracts firmware, opens USB or saves a report automatically.
PE metadata layout follows [Microsoft's PE format documentation](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
Its tests use only original synthetic PE metadata, not proprietary fixtures.
This report is a reproducibility aid, not an executed call graph or wake protocol.

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

The 2026-10-09 14:18:06–14:18:44 owner-run cycle entered actual kernel s2idle.
Boot-time minus monotonic elapsed time indicates approximately 37.2 seconds
asleep. The pre and post snapshots both recorded enabled reader/root-hub wake
permission; reader and hub wake/active/abort counters remained zero. No reader
disconnect appeared in that cycle. This weakens permission-only explanations
but does not eliminate races between parallel hooks or prove an armed detector.
The observed last wake IRQ (7, `pinctrl_amd`) does not uniquely identify the wake
source. Later live reader permission was disabled, which must not be mistaken
for its recorded pre/post state.

The register transport, default-variant layout, calibration margin and capture
restoration sequence are cross-checked. Physical awake detector response and one
held-claim suspend wake have now been owner-confirmed below. Event transport and
unattended idle/suspend ownership sequencing remain incomplete; the installed
reader is not automatically armed during system suspend.

An uninstalled `eh575-touch.py detector` experiment now implements the
cross-checked entry/exit with freshly measured statistics, a strict tested-device
gate, bounded transfers/calibration and independent cancellation recovery. See
EH575-TOUCH-RESEARCH.md. Synthetic failure-injection tests cover the protocol and
recovery paths. It is not integrated into libfprint, system sleep hooks or the
package. The owner's physical trial measured reference 3, DC 11/20, mean 100 and
threshold 180: empty status was zero, touch set `0x04`, and lift did not clear it.
No interrupt packets arrived. Detector exit/capture initialization succeeded and
normal `fprintd-verify` matched afterward. These results validate this unit's
awake detector and capture recovery, not suspend wake.

The explicitly opted-in `detector-suspend --allow-suspend-test` experiment now
uses that measured path, retains exclusive ownership without USB traffic while
waiting for a manual suspend, and restores after clock evidence of resume. It
never suspends automatically, writes wake policy or installs a watcher. The
existing opt-in permission service must already be active. See
EH575-TOUCH-RESEARCH.md for prerequisites, fallback and evidence boundaries.
In the 2026-10-09 15:19:20–15:19:24 CEST cycle, the owner explicitly confirmed that
touch woke the laptop. The isolated detector measured reference 3, DC 11/20,
mean 107 and threshold 187; clock evidence indicated 3.29 seconds asleep. Capture
restoration succeeded and normal fprintd matched afterward. Reader/root-hub wake
permissions remained enabled, active counts increased and last wake IRQ changed
to 9. The metadata are supporting evidence, not unique wake-source identification.
Thus detector state survived one real suspend while the probe held the USB claim.

The new `detector-suspend-released` variant tests whether that state and wake also
survive releasing/closing USB before suspend, then reacquires without stealing for
bounded capture restoration. It is not physically validated or installed. The
probe now settles/checks exposure with the already-characterized 0–63 DC range
before detector calibration; contact/quality gates and restoration remain intact.
Behavior after USB reset, repeated wake reliability, release/reclaim handoff and
awake blank-screen integration remain unverified. The full wake goal remains
active and unachieved.
No package should silently arm wake, alter PCI/ACPI policy, keep the CPU awake,
poll during suspend, or treat a contact event as authentication.

## Awake GNOME handoff preparation

The installed Ubuntu Shell 50.1 embedded resources were rechecked from
`/usr/lib/gnome-shell/libshell-18.so`. Its screen shield cancels the authentication
dialog on idle and before sleep. `_wakeUpScreen()` clears the blanking lightboxes
and emits a wake signal; `_activateDialog()` starts the ordinary locked-session
dialog. The unlock dialog's `activate()` calls its normal prompt path, which uses
GDM. No deactivation/unlock function or authentication result should be synthesized.
These private APIs are version-sensitive: a companion must fail safely when its
supported Shell API/state is absent. See upstream
[screen shield](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/screenShield.js)
and [session-mode extension documentation](https://gjs.guide/extensions/topics/session-modes.html).
An extension can opt into `unlock-dialog` mode, but its lifecycle may be disabled
and re-enabled on mode changes; every disable path must cancel and release its
native helper. A logged-in session extension does not automatically run in the
separate GDM greeter process.

The uninstalled `detector-contact` experiment now provides one-shot, awake contact
observation followed by capture recovery and USB release/close before it emits a
contact-only marker. It refuses pending sleep/active fprintd and cancels on their
system-bus transitions. It does not wake or unlock the display. That marker and
successful process exit are prerequisites for a future companion's UI action,
not authentication evidence. Synthetic status/notification/cancellation tests
exercise its fail-closed gates; real bus delivery and USB ownership races remain
unverified. The current unlocked-only helper is not sufficient for lock-screen
use: a companion must start it only when normal authentication is idle and stop
it before keyboard/mouse-driven authentication or sleep. That integration is not
yet installed or complete.
