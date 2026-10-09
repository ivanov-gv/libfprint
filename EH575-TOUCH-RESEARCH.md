# Linux-only EH575 touch/wake research

Target: `1c7a:0575`, revision `1072`. Both touch-to-wake from real suspend and
touch-to-activate a blanked GNOME lock screen are requirements. The owner confirmed
real suspend wake by touch in the isolated, held-claim detector experiment on
2026-10-09. Automatic suspend arming and awake blank-screen integration are not
implemented. The current package changes USB wake permission only; it does not
arm the sensor's low-power touch detector.

An uninstalled, explicitly selected `detector` probe now measures and arms a
cross-checked volatile detector path for **awake-only** observation. It is not a
wake service or proof of suspend wake, and is not called by libfprint/fprintd.
Following a successful owner-run detector/restoration test, a separate explicit
`detector-suspend --allow-suspend-test` mode is available for manual real-suspend
observation. It is also uninstalled, experimental, and not automatic wake support.

## Missing pieces and intended handoff

The stationary driver detects contact from explicitly requested images during an
operation. It does not currently listen to the two interrupt endpoints or implement
a characterized low-power event mode. While awake, polling can potentially detect
a finger without matching it. While suspended, host polling cannot execute: the
hardware must signal remote wake. Interrupt traffic during an awake test is a
lead, not proof of that capability or its arming sequence.

Installed GNOME Shell 50.1 cancels the unlock dialog's authentication conversation
when presence becomes idle. It also cancels before suspend. Its unlock dialog
destroys authentication on returning to the clock. Merely changing libfprint's
USB wake flag does not keep a blank screen listening. We must first establish a
reliable contact signal, then wake/show the normal unlock dialog and let stock
GDM/PAM/fprintd authenticate. A touch signal must never be treated as a match.
Reference: [GNOME screen shield](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/screenShield.js)
and [unlock dialog](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/unlockDialog.js).
Ubuntu carries changes; the installed embedded resources were checked as well.

An unattended watcher must not claim the reader while fprintd is enrolling or
verifying, must release it BEFORE starting the normal authentication conversation,
and must stop before suspend unless a hardware wake mode is actually established.
It must preserve password fallback, attempt limits and system PolicyKit. No
infinite authentication retry loop, unlock D-Bus shortcut, synthetic password,
disabled sleep or arbitrary keyboard injection is proposed. This integration is
not implemented yet: contact and isolated suspend wake are now confirmed, but
safe USB ownership/event handoff still needs testing. A companion GNOME component
may be necessary; that would
remain separate from the upstream libfprint driver contribution.

## Isolated physical tests

Build the current source normally with the existing isolated EH575 configuration.
The `tests/eh575-touch-probe` executable is built in both image and ridge modes,
but is never installed or bundled in the GNOME package. Use an unlocked local
terminal, WITHOUT sudo. Close private fprintd sessions and other fingerprint
clients. Let system fprintd exit normally when idle; the runner refuses an active
daemon or a failed status check. Do not stop system authentication services.
An exclusive interface claim provides a further guard against another USB owner;
the probe never steals it. Do not lock or suspend while an **awake** probe owns
the reader. The explicitly opted-in suspend experiment below is the only exception.
Ctrl+C cancels pending USB reads and attempts normal release/close.

### New detector probe (awake only)

```sh
python3 scripts/eh575-touch.py detector \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root
```

Keep the reader empty from the start. This mode first discards one settling image,
then requires three empty-image quality checks at usable exposure. If necessary,
it searches the installed capture driver's characterized DC range (0–63), with
at most seven exposure attempts and a new settling frame after each setting.
Texture/contact or acquisition failures abort; image/empty-quality thresholds
are unchanged. Failed checks report mean, texture, clipping and DC rather than
assuming poor exposure means a finger is present. This addresses a possible cause
of the owner's repeated pre-arm rejections; repeat hardware testing is still
needed to confirm improvement. It then uses volatile detector calibration (`0x34`/`0x35`), reads
the measured analogue values and adjusts the detection DC using sensor statistics.
The threshold is the measured mean plus the OEM's margin of 80, not the values
from another person's USB trace. Zero DC, invalid statistics, busy timeout and
any malformed/failed command reply stop the operation. The calibration loop and
each status wait are bounded; no firmware or nonvolatile storage is written.

The entry bank and its exit have been cross-checked against the hash-pinned Acer
driver and all four published Windows capture traces. That supports an isolated
experiment on the gated revision-1072 unit, not deployment or a claim that the
unobserved Windows suspend path is reproduced. A different sensor variant or a
software-statistics fallback is not automatically selected.

After `Volatile detector armed`, follow the same empty/touch/lift six-second
phases. The probe counts interrupt packets and reports register-`0x01` status
changes; it does not interpret a bit as authentication. Status changes without
interrupt packets would also be useful evidence. Do **not** suspend, lock the
session, run fingerprint clients or touch until asked during this probe.

After success, failure or Ctrl+C following calibration, a separate five-second
recovery budget attempts detector exit and the characterized 47-command capture
initialization, even when the operation's cancellation token is set. Confirm
`Detector exited; characterized capture initialization restored`, then test normal
`fprintd-verify`. A failed restore is explicitly reported; never proceed to suspend
on that result. Uncatchable termination/disconnection cannot guarantee cleanup;
normal driver initialization is still needed on the next claim. No enrollment or
persistent capture calibration is accessed. Share the printed output, not images.

### Awake one-shot contact handoff (development only)

The `detector-contact --allow-contact-test` mode prepares the same measured
detector, rejects a pre-latched/unknown status, and waits for a fresh contact by
polling register `0x01` at most four times per second. It does not capture images
while waiting, consume templates, authenticate, wake a screen or install a daemon.
The current 45-second overall awake budget includes setup. Run unlocked, without
sudo; do **not** lock or suspend during this development-only test.

```sh
python3 scripts/eh575-touch.py detector-contact --allow-contact-test \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root
```

Keep empty until `CONTACT TEST READY`, then touch once and hold. The sole handoff
marker, `EH575_CONTACT_READY`, is printed only after successful detector exit,
capture initialization, interface release and USB close, and never on cancellation
or cleanup failure. It means contact only, **not a fingerprint match**. A future
GNOME companion must also require process exit success before waking/showing the
ordinary authentication dialog. The native helper must not retain the reader
while GNOME/fprintd starts that authentication.

This mode checks that logind is not preparing sleep and that system fprintd has
no bus owner. It subscribes before checking, without auto-starting either service.
Sleep preparation, fprintd bus-name acquisition, a lost system bus or Ctrl+C
cancels observation and attempts bounded capture restoration. If a suspend is
observed via clocks, contact notification is suppressed. Unknown detector statuses
other than the characterized `0x00`/`0x04` abort. No interface is stolen and no
authentication service is stopped. Cancellation is cooperative: these checks do
not guarantee restoration before kernel freeze or beat every concurrent claim.
There is no sleep inhibitor here. Real ownership races, host bus notifications,
cleanup failures and GNOME handoff still need integration/physical testing.

The helper is uninstalled and not an unattended watcher. No GNOME extension is
installed or enabled. Use it first as an isolated unlocked test; releasing a USB
handle safely is necessary but is not sufficient to provide awake lock-screen
wake. After it finishes, normal `fprintd-verify` must still work.

### Measured-detector real-suspend experiment

Prerequisites: the awake detector probe must succeed and report successful capture
restoration; normal `fprintd-verify` must still match. Both were owner-confirmed
on 2026-10-09. Keep keyboard/power-button wake and password login available.
The existing optional wake-permission package/service must already be installed.
This test does not install or enable a boot service and never suspends automatically.

In the first terminal, with private fingerprint sessions closed and system fprintd
already idle, run:

```sh
sudo systemctl start eh575-wakeup.service
sudo /usr/libexec/eh575-wakeup reapply
python3 scripts/eh575-touch.py detector-suspend --allow-suspend-test \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root
```

The Python/C probe still runs **without sudo**; the first two commands use only
the existing, opt-in permission helper. The runner requires an inactive fprintd,
active permission service, one tested reader, and enabled reader/hub/platform wake
permission. It refuses rather than changing any of those policies itself.

Keep the sensor empty until `SUSPEND TEST READY`. An already-latched touch aborts
before this point. Within 120 awake seconds, use the desktop menu or run
`systemctl suspend` in a **second terminal**. Wait until the laptop is genuinely
asleep, then touch the sensor once. If it does not wake after about 15 seconds,
use keyboard/power-button wake. Do not touch during the arming/pre-suspend period:
that could latch a premature event and invalidate the test.

While waiting, the probe retains its exclusive USB claim so no competing client
can overwrite the armed state, but sends **no USB transfers**. It does not hold a
sleep inhibitor, poll fingerprint images, block host suspend or set a wake timer.
It only samples clocks while the host is awake; userspace cannot do this in sleep.
On resume, a BOOTTIME/MONOTONIC elapsed-time difference of at least two seconds
triggers the same bounded detector-exit/capture restoration, then releases/closes
the reader. Wait for that restoration before using fingerprint login; password
fallback remains available. If USB resets/disconnects, restoration may fail and
must not be reported as success. A normal driver claim will initialize again.

Timeout/Ctrl+C also attempts restoration. Uncatchable termination cannot guarantee
cleanup. Clock evidence distinguishes host sleep from an awake wait but **does not
identify what woke the laptop**: report whether touch worked or fallback wake was
needed, the printed output, normal verification afterward, and optionally the
fresh `journalctl -b -u systemd-suspend.service` cycle. This is not an unattended
GNOME integration. The owner physically confirmed touch wake in one held-claim
trial; this does not establish reliable automatic integration across boot/resume.

### Release-before-suspend handoff experiment

The successful trial above retained its USB claim. A future sleep hook must not
leave the reader unavailable to fprintd after resume. Test whether detector state
and wake survive releasing the interface and closing the USB handle:

```sh
sudo systemctl start eh575-wakeup.service
sudo /usr/libexec/eh575-wakeup reapply
python3 scripts/eh575-touch.py detector-suspend-released --allow-suspend-test \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root
```

Use the same empty-reader, manual suspend, touch and fallback procedure above.
This variant prints `USB interface released and handle closed with detector armed`
before `SUSPEND TEST READY`. Do not run fingerprint clients until the test ends:
they could overwrite detector state and invalidate the experiment. The process
holds no USB handle during the wait. On resume, timeout or cancellation, it tries
to reopen and exclusively claim the same device, then restore capture. It never
steals an interface if another client claimed it, stops fprintd or resets USB. A
disconnect/reset or competing claim can prevent recovery, which is reported as a
failed restore; close the probe and check normal fprintd before another suspend.
Uncatchable termination cannot guarantee restoration. The owner tested this
variant: touch did NOT wake the laptop; keyboard wake was needed after about
20.61 seconds asleep. Capture restoration reported a status/busy timeout, but
normal `fprintd-verify` afterward matched. This is not a working automatic wake
service and has not been installed.

### Release the claim but keep the handle open

The next isolation experiment is `detector-suspend-unclaimed`. It releases the
exclusive interface claim but keeps the USB handle open throughout the manual
sleep test. Linux's [USB power-management documentation](https://docs.kernel.org/driver-api/usb/power-management.html)
states that an open usbfs file makes a device non-idle even without I/O. Keeping
it open tests whether runtime autosuspend before system suspend explains the
difference between the held-claim success and closed-handle failure. This is a
hypothesis, not an established hardware cause or a permanent power-policy change.

```sh
sudo systemctl start eh575-wakeup.service
sudo /usr/libexec/eh575-wakeup reapply
python3 scripts/eh575-touch.py detector-suspend-unclaimed --allow-suspend-test \
  --build /tmp/eh575-libfprint-touch-power-build --deps /tmp/eh575-native-deps/root
```

Keep the reader empty until `SUSPEND TEST READY`. In another terminal, within
120 awake seconds, run `systemctl suspend`. Wait until truly asleep (at least
three seconds), then touch once. If no wake after 15 seconds, use the keyboard
or power button. Do not run fingerprint clients during the experiment; they
could overwrite detector state even though its exclusive claim was released.
There is no USB I/O during the wait, sleep inhibitor, automatic suspend or wake
timer. The mode does not write USB/hub/PCI/ACPI power policies.

After resume, timeout or cancellation, it attempts to reclaim without stealing,
restore capture, release and close. Wait for cleanup before `fprintd-verify`.
Restoration failures now identify the last attempted opcode/register, exchange
count and known busy-bit state; no raw replies or fingerprint data are printed.
The unclaimed-handle variant still needs physical validation. It is not a daemon
and has not been installed or enabled for GNOME.

Run the modes one at a time:

```sh
python3 scripts/eh575-touch.py interrupt \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root

python3 scripts/eh575-touch.py interrupt-initialized \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root

python3 scripts/eh575-touch.py touch \
  --build /tmp/eh575-libfprint-ridge-build --deps /tmp/eh575-native-deps/root
```

The first mode sends NO vendor commands and listens only to interrupt IN 83/84.
An optional `open` mode checks discovery/descriptors/exclusive claim/release only,
without initialization, interrupt reads, or image acquisition.
The second performs only the already-characterized 47-command volatile
initialization, then listens; it does not request an image. Follow the three
six-second prompts: empty, touch-and-hold, lift. Output is per-phase endpoint
packet counts and payload-change counts, never packet contents or USB serials.
Interrupt meaning is unknown; more packets alone must not trigger an unlock.

The third uses known initialization/rearm/image commands to measure three
quality-gated empty frames at the current DC setting. It does not change DC,
read/write persistent calibration, or load enrollment templates. It then requires
two contact frames and three empty frames for release. Output says `CONTACT`
and `RELEASE`, not `match`. Images are processed in memory and discarded, never
saved. On a poor idle exposure the test may time out; first do a normal successful
empty-start `fprintd-verify`, then let the daemon exit and repeat. No image or
matching thresholds are lowered to force contact.

Awake modes have a 45-second overall transfer/observation deadline. The explicit
suspend experiment uses that budget for setup, then 120 awake seconds for manual
suspend/resume observation; detector recovery has its own five-second budget.
All modes use bounded transfer
timeouts, strict tested-revision/endpoint/reply/framing checks, and cleanup on
errors. Any initialization/acquisition change is volatile; the installed driver
will initialize normally on its next claim. No system power policies are changed
by these probes. Afterward confirm normal `fprintd-verify` still works.

Share only the printed aggregate output. No raw USB trace or fingerprint dump is
needed for this first test. If the interrupt modes stay silent but the image mode
detects contact/release, awake screen-wake polling is a possible prototype route;
automatic low-power arming/handoff remains a separate investigation.
If touch-specific interrupts appear, a follow-up must establish their framing,
idle/release semantics and behavior during real suspend before enabling a watcher.

## Evidence boundaries

The owner's full physical run on 2026-10-09 reported zero packets on both interrupt
endpoints in all phases, both before and after known initialization. Image polling
successfully detected CONTACT and RELEASE. This establishes an awake polling lead,
not a hardware interrupt or low-power wake mode. Two subsequent explicit s2idle
trials did not wake on touch; see EH575-GNOME-TRIAL.md. Static wake-mode research
and the new pre/post sleep metadata are described in doc/egis0575-wake-research.md.

The owner-run calibrated detector trial measured reference 3, DC 11/20, mean 100
and threshold 180. Empty register-`0x01` status was zero; touch changed it to `0x04`,
which stayed latched after lift. Both interrupt endpoints remained silent in all
three phases. Detector exit/capture initialization succeeded, followed by a real
normal `fprintd-verify` match. This proves the measured detector responds on this
unit and ordinary matching recovers; alone it does not establish suspend wake.

In the owner's subsequent held-claim detector-suspend trial, measured reference
was 3, DC 11/20, mean 107 and threshold 187. Clocks recorded approximately 3.29
seconds asleep, detector exit/capture initialization succeeded, and the owner
explicitly confirmed that touching the sensor woke the laptop. Normal fprintd
matched afterward. The 15:19:20–15:19:24 CEST system-suspend snapshots recorded
enabled reader/root-hub wake permission, increased reader/root-hub active counts
and last wake IRQ 9 rather than 7. These metadata support the observation but do
not independently identify the wake source. This is one successful real-suspend
test on this laptop, not proof of release-before-suspend retention, repeated-cycle
reliability, awake blank-screen wake, or automatic GNOME handoff.

The real revision-1072 reader passed the `open` probe: discovery, descriptor
checks, exclusive claim, release and close. Ridge and default-image builds passed
their selected regression suites; selected ridge tests also passed ASan/UBSan and
leak checks. These automated checks do not reproduce the physical touch trial.

Synthetic tests cover bounded event statistics, brightness-only/background-texture
rejection, contact/clipping gates and runner safeguards. They do not exercise a
physical reader, prove exclusive-claim races are harmless, establish wake
reliability or battery impact, or test a GNOME extension. Do not publish these
tools as completed wake-on-touch support.

Detector synthetic tests additionally cover trace-derived command ordering,
measured thresholds, zero/underflow rejection, invalid statistics, bounded busy
polls, failure/malformed-reply injection at every calibration step, and restoration
after partial entry/calibration. They also reject failed restoration at every
transfer. Exposure tests cover all synthetic target DC values, a stale settling
frame, contact-texture rejection, unattainable exposure and read/capture/write
failures. Runner tests cover the contact and both suspend variants' explicit opt-in, inactive
fprintd and wake-permission guards. These validate software control flow, not
physical touch, USB wake or release/reclaim behavior on hardware.

Contact synthetic tests additionally check all 256 status bytes, failed/malformed
status exchanges, and all combinations of contact/restoration/release/close/
cancellation notification gates. Native callback tests exercise sleep, service
owner and bus-close cancellation, plus pre-cancelled and expired observations
without USB or a system-bus connection. These do not prove real D-Bus subscription
delivery, USB cleanup ordering under a race, or GNOME authentication handoff.
