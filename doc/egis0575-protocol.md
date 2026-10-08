# EH575 protocol and acquisition notes

Target: USB 1c7a:0575, bcdDevice 1072; interface 0/alternate 0, class/subclass
ff/ff and protocol 00. Bulk OUT 01 and IN 82 have 512-byte maximum packets.
The reader also exposes interrupt IN 83/84; this driver does not use them.
GUsb's endpoint `kind` is USB descriptor type, not transfer attributes; its
public endpoint API does not expose `bmAttributes`. The driver validates the
known interface, endpoint addresses, descriptor type and packet sizes, then
uses only bulk transfers. No kernel driver detachment or USB reset is requested.

## Commands and replies

Commands begin with ASCII `EGIS`. The characterized volatile initialization
contains 47 commands, followed for each capture by eight rearm commands and
`EGIS 64 14 ec`. The exact byte tables are in `egis0575.h`; regression tests
check byte-for-byte parity with the working Python transport.

Replies begin with `SIGE` or `EGIS`; bytes 4 and 5 carry address/value or count,
and byte 6 must be successful status 01. Ordinary replies are seven bytes;
opcodes 62/63/71 have an additional command-count-sized payload. Read opcode
60 returns the register value in byte 5, rather than echoing the request's
placeholder. Unknown signatures, wrong echoes/counts, unsuccessful status or
short writes abort the operation. Register semantics beyond those documented
here are not inferred from the arbitrary initialization constants.

## Images and stream synchronization

Each image is exactly 103×52 = 5356 unsigned eight-bit pixels in row-major
order. Observed framing includes a whole image and fragments of 5120 + 236
bytes. Use a packet-aligned 8192-byte receive buffer, reject excess/zero-sized
fragments and status packets where image data is expected, and impose a
monotonic 1500 ms deadline for the entire image, not per fragment. Do not pad
truncated samples. After a complete image, drain an optional seven-byte ACK
with a 20 ms timeout. Only timeout is an expected missing-ACK condition.

Command transfers have 500 ms timeouts. Native activation has a 15 s deadline;
each enrollment presentation has a 30 s bounded wait. Pending transfers use a
per-activation cancellable linked to the caller's cancellation. Deactivation
removes timers and cancels USB work; completion waits for its callback. A
cancelled/inconsistent wire exchange requires close/reopen. No stream retries,
firmware writes or speculative commands are used to recover it.

## Exposure and contact

Register 0f is a six-bit volatile DC setting, not firmware. Read the current
setting first; each trial writes a candidate, discards one settling frame and
measures three frames. Keep the reader empty. Search the range 0..63 toward
mean 128 only when the initial mean lies outside 112..144 or clips. Remeasure
the selected setting; require mean 96..160, frame deviation ≤18 and clipping
≤0.5%. Excess contrast during calibration fails rather than learning a finger
as an empty reference. The session background is a per-pixel median of the
three actually measured reference frames. Failed calibration leaves a volatile
setting until fresh initialization; it does not promise restoration.

Contact is spatial RMS deviation ≥8 from the measured background after removing
global brightness shift. Require three empty frames to report finger removal.
Discard 250 ms of finger settling, then collect three frames with raw deviation
≥20 and clipping ≤5%. Persistent poor quality generates a center-finger retry.
These are acquisition gates, not matching thresholds or proof of identity.

The candidate median frame has its own measured background subtracted and
global mean removed, with a fixed gain of two around level 128. Bounded bilinear
2× interpolation yields a 206×104 partial image for native minutiae extraction.
This is not additional physical coverage. The raw 103-byte row stride cannot
be passed to libfprint's pixman resizing helper, which requires aligned rows.
Orientation, physical resolution, gain choice, pressure effects and actual
minutiae reliability need hardware evaluation. No Bozorth3 threshold is changed.

## Provenance and evidence boundaries

The tables derive from SandroRoque/python-egistec-eh575, MIT, commit
38de042e0103607c83f7ec4b0497504449e7bd5e, `device_profile.py`. Retain
`egis0575-reference-MIT.txt`. Original EH575 research: Animeshz/EgisTec-EH575.
The 0f semantics were cross-checked against related EH576 research, but no
EH576 driver, vendor DLL, decompiled matcher, firmware blob or biometric sample
is copied into this contribution. Native transport/image/lifecycle code is new.

Tests use synthetic arrays and fake USB callbacks, not personal fingerprints.
Native discovery/open/calibration/cancellation have been exercised on one
laptop. A real capture produced a 206×104 image with only two extracted
minutiae; improving/evaluating acquisition quality remains necessary. Native
matching and GNOME integration are not established by this capture or the
Python prototype's separate 7/8 genuine and 0/24 wrong-finger audit.
