import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
const source = await fs.readFile(process.argv[2], 'utf8');
const { register } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));
const hooks = new Map(), files = new Map();
let callback, submitted = 0;
const sessionId = '00000000-0000-0000-0000-000000000003';
const control = '/profile/skills/callagent-desktop-bridge/../../callagent-desktop-bridge';
const challenge = 'a'.repeat(64);
const lease = () => files.set(control + '/connection.json', JSON.stringify({version: '1', bundle: '__CALLAGENT_BUNDLE__', challenge, expiresAt: Date.now() + 15000}));
const $ = {
  plugin: {root: '/profile/skills/callagent-desktop-bridge'},
  fs: {read: async path => { assert(files.has(path), path); return files.get(path); }, write: async (path, text) => files.set(path, text), exists: async path => files.has(path)},
  session: {id: async () => sessionId, surfaces: async () => ['desktop'], version: async () => ({version: '2.1.309'})},
  clock: {every: (interval, task) => { assert.equal(interval, 5000); callback = task; return {cancel() {}}; }},
  prompt: {submit: async ({text, asUser}) => { assert.equal(text, 'authorized text'); assert.equal(asUser, true); ++submitted; return {origin: {kind: 'plugin', asUser: true}}; }}
};
register((name, handler) => hooks.set(name, handler));
const next = event => event;
lease();
await hooks.get('session.start')($, {}, next);
await callback();
assert(files.has(control + '/heartbeats/' + sessionId + '.json'));
assert.deepEqual(await hooks.get('session.receive')($, {text: 'ordinary message'}, next), {text: 'ordinary message'});
const frame = {id: 'b'.repeat(64), token: 'c'.repeat(64), challenge, sessionId};
files.set(control + '/requests/' + frame.id + '.json', JSON.stringify({...frame, text: 'authorized text', digest: 'digest', expiresAt: Date.now() + 15000}));
const receive = value => hooks.get('session.receive')($, {text: 'CALLAGENT_SESSION_V1:' + JSON.stringify(value)}, next);
await receive({...frame, sessionId: 'different'});
await callback(); assert.equal(submitted, 0);
await receive(frame); await callback(); assert.equal(submitted, 1);
assert.equal(JSON.parse(files.get(control + '/receipts/' + frame.id + '.json')).status, 'accepted');
await receive(frame); await callback(); assert.equal(submitted, 1, 'receipt prevented replay');
const busy = {...frame, id: 'd'.repeat(64)};
files.set(control + '/requests/' + busy.id + '.json', JSON.stringify({...busy, text: 'authorized text', expiresAt: Date.now() + 15000}));
await hooks.get('turn.start')($, {turnId: 'running'}, next);
await receive(busy); await callback(); assert.equal(submitted, 1);
assert.equal(JSON.parse(files.get(control + '/receipts/' + busy.id + '.json')).status, 'rejected');
console.log('Claude plugin ownership, heartbeat, delivery, busy and replay behavior passed');
