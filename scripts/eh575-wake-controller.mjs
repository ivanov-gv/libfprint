// SPDX-License-Identifier: LGPL-2.1-or-later
// Pure handoff policy. No Shell, D-Bus, USB, process or authentication APIs here.
// A future adapter must report child exit only after its actual wait completes.
export const CONTACT_READY = 'EH575_CONTACT_READY: contact only, NOT authenticated; capture restored and USB released/closed.';

export function canObserve(state) {
    return state?.supported === true && state.locked === true &&
        state.blanked === true && state.authIdle === true &&
        state.awake === true && state.busAlive === true;
}

export function cleanContactExit(result) {
    if (result?.exited !== true || result.code !== 0 || result.signal !== null ||
        typeof result.stdout !== 'string' || result.stdout.length > 4096)
        return false;
    const lines = result.stdout.split('\n');
    return lines.filter(line => line === CONTACT_READY).length === 1;
}

// Keep the SAME controller across extension disable/re-enable cycles. Disabling
// cancels the child but cannot dispose its lease before authoritative child exit.
export class WakeHandoff {
    constructor({readState, startProbe, wakePrompt, reportError}) {
        this._readState = readState;
        this._startProbe = startProbe;
        this._wakePrompt = wakePrompt;
        this._reportError = reportError;
        this._enabled = false;
        this._epoch = 0;
        this._probe = null;
        this._latched = false;
    }

    get busy() {
        return this._probe !== null;
    }

    _report(error) {
        try {
            this._reportError(error);
        } catch {
            // A logging failure must not skip USB handoff/cancellation gates.
        }
    }

    _eligible() {
        try {
            return canObserve(this._readState());
        } catch (error) {
            this._report(error);
            return false;
        }
    }

    enable() {
        if (!this._enabled) {
            this._enabled = true;
            this._epoch++;
        }
        this.synchronize();
    }

    disable() {
        this._enabled = false;
        this._epoch++;
        this._stop('disabled');
    }

    synchronize() {
        const eligible = this._eligible();
        if (!eligible) {
            this._latched = false;
            this._stop('state changed');
            return;
        }
        if (!this._enabled || this.busy || this._latched)
            return;

        const probe = {
            epoch: this._epoch,
            handle: null,
            stopping: false,
            stopSent: false,
            pending: new Map(),
        };
        this._probe = probe;
        try {
            // Contract: throwing means spawn did not create a live child.
            // onExit may be synchronous in tests; do not revive a finished lease.
            const handle = this._startProbe(result => this._exited(probe, result));
            if (this._probe === probe) {
                probe.handle = handle;
                if (probe.stopping)
                    this._stop('stopped during spawn');
            }
        } catch (error) {
            if (this._probe === probe) {
                this._probe = null;
                this._latched = true;
                this._epoch++;
                this._flush(probe);
            }
            this._report(error);
        }
    }

    _stop(reason) {
        const probe = this._probe;
        if (!probe)
            return;
        if (!probe.stopping) {
            probe.stopping = true;
            this._epoch++;
        }
        if (!probe.handle || probe.stopSent)
            return;
        probe.stopSent = true;
        try {
            // SIGTERM, not cancellation of a wait promise or immediate SIGKILL.
            // The native helper must get its bounded restoration/close attempt.
            probe.handle.stop(reason);
        } catch (error) {
            this._report(error);
            // Failure to send cancellation is NOT evidence that USB is free.
        }
    }

    // A future Shell adapter can defer normal verifier.begin (not key events or
    // prompt creation). run invokes the ORIGINAL verifier method; it never
    // supplies an authentication result. valid must recheck that exact verifier,
    // live prompt, username/session and cancellation generation. drop releases
    // any original pending hold when the request is superseded/cancelled.
    requestAuth(key, {run, valid, drop}) {
        const request = {run, valid, drop};
        const probe = this._probe;
        if (!probe) {
            this._run(request);
            return;
        }
        const previous = probe.pending.get(key);
        if (previous)
            this._drop(previous);
        probe.pending.set(key, request);
        this._stop('normal authentication requested');
    }

    cancelAuth(key) {
        const request = this._probe?.pending.get(key);
        if (request) {
            this._probe.pending.delete(key);
            this._drop(request);
        }
    }

    _drop(request) {
        try {
            request.drop();
        } catch (error) {
            this._report(error);
        }
    }

    _run(request) {
        let valid = false;
        try {
            valid = request.valid() === true;
        } catch (error) {
            this._report(error);
        }
        if (!valid) {
            this._drop(request);
            return;
        }
        try {
            request.run();
        } catch (error) {
            this._report(error);
        }
    }

    _flush(probe) {
        const requests = [...probe.pending.values()];
        probe.pending.clear();
        for (const request of requests)
            this._run(request);
    }

    _exited(probe, result) {
        if (probe !== this._probe)
            return; // Stale/duplicate completion cannot clear a newer child.
        if (result?.exited !== true) {
            this._report(new Error('Child exit not proven; retaining USB handoff gate'));
            this._stop('exit observation failed');
            return;
        }

        const wake = this._enabled && !probe.stopping &&
            probe.epoch === this._epoch && this._eligible() &&
            cleanContactExit(result) && probe.pending.size === 0;
        this._probe = null; // Only actual child exit releases the process lease.
        this._latched = true; // No immediate calibration/restart loop in one blank cycle.
        this._epoch++;
        this._flush(probe);
        if (wake && this._enabled && this._eligible()) {
            try {
                this._wakePrompt(); // Wake display + ordinary prompt, NEVER unlock.
            } catch (error) {
                this._report(error);
            }
        }
    }
}
