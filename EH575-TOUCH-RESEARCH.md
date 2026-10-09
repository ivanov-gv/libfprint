# Linux-only EH575 touch/wake research

Target: `1c7a:0575`, revision `1072`. Both touch-to-wake from real suspend and
touch-to-activate a blanked GNOME lock screen are requirements. Neither is claimed
working by these diagnostics. The current package changes USB wake permission
only; it does not know how to arm the sensor's low-power touch detector.

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
not implemented yet: an isolated contact result is needed before choosing a safe
ownership/event design. A companion GNOME component may be necessary; that would
remain separate from the upstream libfprint driver contribution.

## Isolated physical tests

Build the current source normally with the existing isolated EH575 configuration.
The `tests/eh575-touch-probe` executable is built in both image and ridge modes,
but is never installed or bundled in the GNOME package. Use an unlocked local
terminal, WITHOUT sudo. Close private fprintd sessions and other fingerprint
clients. Let system fprintd exit normally when idle; the runner refuses an active
daemon or a failed status check. Do not stop system authentication services.
An exclusive interface claim provides a further guard against another USB owner;
the probe never steals it. Do not lock or suspend while a probe owns the reader.
Ctrl+C cancels pending USB reads and attempts normal release/close.

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

All modes have a 45-second overall transfer/observation deadline, bounded transfer
timeouts, strict tested-revision/endpoint/reply/framing checks, and cleanup on
errors. Any initialization/acquisition change is volatile; the installed driver
will initialize normally on its next claim. No system power policies are changed
by these probes. Afterward confirm normal `fprintd-verify` still works.

Share only the printed aggregate output. No raw USB trace or fingerprint dump is
needed for this first test. If the interrupt modes stay silent but the image mode
detects contact/release, awake screen-wake polling is a possible prototype route;
hardware low-power wake remains a separate unresolved protocol investigation.
If touch-specific interrupts appear, a follow-up must establish their framing,
idle/release semantics and behavior during real suspend before enabling a watcher.

## Evidence boundaries

The owner's full physical run on 2026-10-09 reported zero packets on both interrupt
endpoints in all phases, both before and after known initialization. Image polling
successfully detected CONTACT and RELEASE. This establishes an awake polling lead,
not a hardware interrupt or low-power wake mode. Two subsequent explicit s2idle
trials did not wake on touch; see EH575-GNOME-TRIAL.md. Static wake-mode research
and the new pre/post sleep metadata are described in doc/egis0575-wake-research.md.

The real revision-1072 reader passed the `open` probe: discovery, descriptor
checks, exclusive claim, release and close. Ridge and default-image builds passed
their selected regression suites; selected ridge tests also passed ASan/UBSan and
leak checks. These automated checks do not reproduce the physical touch trial.

Synthetic tests cover bounded event statistics, brightness-only/background-texture
rejection, contact/clipping gates and runner safeguards. They do not exercise a
physical reader, prove exclusive-claim races are harmless, demonstrate wake during
suspend, establish battery impact, or test a GNOME extension. Do not publish these
tools as completed wake-on-touch support.
