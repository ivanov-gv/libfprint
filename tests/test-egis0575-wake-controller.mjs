// SPDX-License-Identifier: LGPL-2.1-or-later
// Run with gjs -m (or node). Original fakes only: no Shell, USB or system D-Bus.
import {WakeHandoff, CONTACT_READY, canObserve, cleanContactExit}
    from '../scripts/eh575-wake-controller.mjs';

let passed = 0;
function assert(condition, message = 'assertion failed') {
    if (!condition)
        throw new Error(message);
}
function test(name, action) {
    action();
    passed++;
    console.log(`ok ${passed} - ${name}`);
}
const eligible = () => ({supported: true, locked: true, blanked: true,
    authIdle: true, awake: true, busAlive: true});
const matched = () => ({exited: true, code: 0, signal: null,
    stdout: `${CONTACT_READY}\n`});
function fixture() {
    const f = {state: eligible(), children: [], wakes: 0, errors: []};
    f.controller = new WakeHandoff({
        readState: () => f.state,
        startProbe: exit => {
            const child = {exit, stops: 0};
            f.children.push(child);
            return {stop: () => child.stops++};
        },
        wakePrompt: () => f.wakes++,
        reportError: error => f.errors.push(error),
    });
    return f;
}
function request(counts, valid = () => true) {
    return {run: () => counts.ran++, valid, drop: () => counts.dropped++};
}

test('strict state gates, including missing and non-boolean values', () => {
    assert(canObserve(eligible()));
    assert(!canObserve(null));
    for (const key of Object.keys(eligible())) {
        for (const value of [false, undefined, null, 1, 'true']) {
            const f = fixture();
            f.state[key] = value;
            f.controller.enable();
            assert(f.children.length === 0, key);
        }
    }
});
test('exact contact marker plus authoritative clean exit required', () => {
    assert(cleanContactExit(matched()));
    for (const result of [null, {}, {...matched(), exited: false},
        {...matched(), code: 1}, {...matched(), signal: 15},
        {...matched(), stdout: 'EH575_CONTACT_READY'},
        {...matched(), stdout: `${CONTACT_READY}\n${CONTACT_READY}\n`},
        {...matched(), stdout: `${CONTACT_READY}\n${'x'.repeat(4096)}`}])
        assert(!cleanContactExit(result));
});
test('one child, no wake until successful actual exit, no immediate restart', () => {
    const f = fixture();
    f.controller.enable();
    f.controller.synchronize();
    assert(f.children.length === 1 && f.controller.busy && f.wakes === 0);
    f.children[0].exit(matched());
    assert(f.wakes === 1 && !f.controller.busy);
    f.controller.synchronize();
    assert(f.children.length === 1);
});
test('recheck every guard at completion even when notification was missed', () => {
    for (const key of Object.keys(eligible())) {
        const f = fixture();
        f.controller.enable();
        f.state[key] = false;
        f.children[0].exit(matched());
        assert(f.wakes === 0 && !f.controller.busy, key);
    }
});
test('state transition cancels once; late success cannot wake', () => {
    const f = fixture();
    f.controller.enable();
    f.state.awake = false;
    f.controller.synchronize();
    f.controller.synchronize();
    assert(f.children[0].stops === 1 && f.controller.busy);
    f.state = eligible();
    f.controller.synchronize();
    assert(f.children.length === 1);
    f.children[0].exit(matched());
    assert(f.wakes === 0);
});
test('disable/re-enable retains live lease until exit', () => {
    const f = fixture();
    f.controller.enable();
    f.controller.disable();
    f.controller.enable();
    assert(f.children.length === 1 && f.children[0].stops === 1);
    f.children[0].exit(matched());
    assert(f.wakes === 0);
    f.state.blanked = false;
    f.controller.synchronize();
    f.state.blanked = true;
    f.controller.synchronize();
    assert(f.children.length === 2);
    f.children[0].exit(matched());
    assert(f.controller.busy && f.wakes === 0);
    f.children[1].exit(matched());
    assert(f.wakes === 1);
});
test('errors or missing marker never become a wake', () => {
    for (const result of [{...matched(), code: 1}, {...matched(), signal: 15},
        {...matched(), stdout: 'Capture restoration FAILED\n'}]) {
        const f = fixture();
        f.controller.enable();
        f.children[0].exit(result);
        assert(!f.controller.busy && f.wakes === 0);
    }
});
test('unproven exit or cancelled wait does not release handoff gate', () => {
    const f = fixture();
    f.controller.enable();
    f.children[0].exit({exited: false});
    assert(f.controller.busy && f.children[0].stops === 1 && f.wakes === 0);
    f.controller.synchronize();
    assert(f.children.length === 1);
    f.children[0].exit(matched());
    assert(!f.controller.busy && f.wakes === 0);
});
test('normal authentication deferred until exit, no duplicate display wake', () => {
    const f = fixture(), counts = {ran: 0, dropped: 0};
    f.controller.enable();
    f.controller.requestAuth('verifier', request(counts));
    assert(counts.ran === 0 && f.children[0].stops === 1);
    f.children[0].exit({...matched(), code: 130});
    assert(counts.ran === 1 && counts.dropped === 0 && f.wakes === 0);
});
test('superseded, cancelled and stale authentication requests dropped', () => {
    const f = fixture(), counts = {ran: 0, dropped: 0};
    f.controller.enable();
    f.controller.requestAuth('verifier', request(counts));
    f.controller.requestAuth('verifier', request(counts));
    assert(counts.dropped === 1);
    f.controller.cancelAuth('verifier');
    assert(counts.dropped === 2);
    f.controller.requestAuth('stale', request(counts, () => false));
    f.children[0].exit(matched());
    assert(counts.ran === 0 && counts.dropped === 3 && f.wakes === 0);
});
test('disable cannot strand an explicitly requested still-valid normal verifier', () => {
    const f = fixture(), counts = {ran: 0, dropped: 0};
    f.controller.enable();
    f.controller.requestAuth('verifier', request(counts));
    f.controller.disable();
    assert(counts.ran === 0);
    f.children[0].exit({...matched(), code: 130});
    assert(counts.ran === 1 && f.wakes === 0);
});
test('no active child: normal authentication remains immediate', () => {
    const f = fixture(), counts = {ran: 0, dropped: 0};
    f.controller.requestAuth('verifier', request(counts));
    assert(counts.ran === 1 && f.children.length === 0);
});
test('spawning failure does not cause a retry loop or wake', () => {
    const f = fixture();
    f.controller._startProbe = () => { throw new Error('spawn failed'); };
    f.controller.enable();
    f.controller.synchronize();
    assert(!f.controller.busy && f.wakes === 0 && f.errors.length === 1);
});
test('cancellation failure retains lease until real exit', () => {
    const f = fixture();
    f.controller._startProbe = exit => {
        f.children.push({exit});
        return {stop: () => { throw new Error('send failed'); }};
    };
    f.controller.enable();
    f.controller.disable();
    assert(f.controller.busy && f.errors.length === 1);
    f.children[0].exit(matched());
    assert(!f.controller.busy && f.wakes === 0);
});
test('state/logging exceptions fail closed without losing cleanup', () => {
    const f = fixture();
    f.controller.enable();
    f.controller._readState = () => { throw new Error('state unavailable'); };
    f.controller._reportError = () => { throw new Error('logging unavailable'); };
    f.children[0].exit(matched());
    assert(!f.controller.busy && f.wakes === 0);
    f.controller.synchronize();
    assert(f.children.length === 1);
});
test('synchronous process exit cannot revive a finished child', () => {
    const f = fixture();
    f.controller._startProbe = exit => {
        exit(matched());
        return {stop: () => { throw new Error('already exited'); }};
    };
    f.controller.enable();
    assert(!f.controller.busy && f.wakes === 1);
    f.controller.disable();
    assert(f.errors.length === 0);
});
test('cancellation during spawn is sent when handle becomes available', () => {
    const f = fixture();
    f.controller._startProbe = exit => {
        const child = {exit, stops: 0};
        f.children.push(child);
        f.controller.disable();
        return {stop: () => child.stops++};
    };
    f.controller.enable();
    assert(f.controller.busy && f.children[0].stops === 1);
    f.children[0].exit(matched());
    assert(!f.controller.busy && f.wakes === 0);
});
test('failed request validation drops its hold; other requests still run', () => {
    const f = fixture(), counts = {ran: 0, dropped: 0};
    f.controller.enable();
    f.controller.requestAuth('bad', request(counts, () => { throw new Error('destroyed'); }));
    f.controller.requestAuth('good', request(counts));
    f.children[0].exit({...matched(), code: 130});
    assert(counts.ran === 1 && counts.dropped === 1 && f.errors.length === 1);
});
console.log(`${passed} pure wake-handoff tests passed; no physical integration tested.`);
