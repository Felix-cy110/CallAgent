// CallAgent's local bridge only submits plain text to its current Desktop session.
const VERSION = '1';
const BUNDLE = '__CALLAGENT_BUNDLE__';
const PREFIX = 'CALLAGENT_SESSION_V1:';
const LEASE_MS = 20000;
const hex = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
let root, timer, pending, submitting = false, ticking = false, ended = false, generation = 0;
const turns = new Set();
const directory = () => root + '/../../callagent-desktop-bridge';
async function lease($) {
  const bytes = await $.fs.read(directory() + '/connection.json');
  if (bytes.length > 4096) return null;
  const row = JSON.parse(bytes), now = Date.now();
  return row.version === VERSION && row.bundle === BUNDLE && hex(row.challenge) &&
    Number.isSafeInteger(row.expiresAt) && row.expiresAt > now && row.expiresAt <= now + LEASE_MS ? row : null;
}
async function tick($) {
  if (ended || ticking) return;
  ticking = true;
  try {
    const connection = await lease($);
    if (!connection || !(await $.session.surfaces()).includes('desktop')) return;
    const sessionId = await $.session.id();
    if (!/^[a-f0-9-]{36}$/i.test(sessionId)) return;
    const { version: engineVersion } = await $.session.version();
    await $.fs.write(directory() + '/heartbeats/' + sessionId + '.json', JSON.stringify({
      version: VERSION, bundle: BUNDLE, challenge: connection.challenge, sessionId, engineVersion, observedAt: Date.now()
    }));
    if (!pending || submitting) return;
    const frame = pending; pending = undefined; submitting = true;
    const epoch = generation, path = directory() + '/requests/' + frame.id + '.json';
    const receipt = directory() + '/receipts/' + frame.id + '.json';
    if (await $.fs.exists(receipt)) return;
    const bytes = await $.fs.read(path);
    if (bytes.length > 1048576 + 8192) return;
    const request = JSON.parse(bytes);
    if (request.id !== frame.id || request.token !== frame.token || request.sessionId !== sessionId ||
        request.challenge !== connection.challenge || request.expiresAt <= Date.now() ||
        request.expiresAt > Date.now() + LEASE_MS || typeof request.text !== 'string' ||
        !request.text.trim() || request.text.trimStart().startsWith('/')) return;
    const save = status => $.fs.write(receipt, JSON.stringify({ version: VERSION, id: frame.id, sessionId, status }));
    if (turns.size || ended || epoch !== generation) { await save('rejected'); return; }
    // A submitting receipt survives a crash and forbids replaying an uncertain send.
    await save('submitting');
    const fresh = await lease($);
    if (!fresh || fresh.challenge !== frame.challenge || request.expiresAt <= Date.now() || ended || epoch !== generation ||
        !(await $.session.surfaces()).includes('desktop') || await $.session.id() !== sessionId) { await save('rejected'); return; }
    await $.fs.write(path, JSON.stringify({id: frame.id, sessionId, digest: request.digest}));
    const response = await $.prompt.submit({ text: request.text, asUser: true });
    await save(typeof response?.drop === 'string' ? 'rejected' :
      response?.origin?.kind === 'plugin' && response.origin.asUser === true ? 'accepted' : 'uncertain');
  } catch {
    // Files can be absent during setup. Once submitting is durable, failure is uncertain.
  } finally { submitting = false; ticking = false; }
}
// Named hooks must remain at module scope for native Mods validation.
async function attach($, event, next) {
  root = $.plugin.root; ended = false;
  if (!timer) timer = $.clock.every(5000, async () => { await tick($); });
  return next(event);
}
export function register(on) {
  on('session.start', attach); on('session.attach', attach);
  on('turn.start', async ($, event, next) => { turns.add(event.turnId); return next(event); });
  on('turn.complete', async ($, event, next) => { turns.delete(event.turnId); return next(event); });
  on('session.receive', async ($, event, next) => {
    if (typeof event.text !== 'string' || !event.text.startsWith(PREFIX)) return next(event);
    try {
      if (ended || pending || submitting || event.text.length > 4096) return {consumed: 'CallAgent bridge busy'};
      const frame = JSON.parse(event.text.slice(PREFIX.length)), connection = await lease($);
      if (!connection || !hex(frame.id) || !hex(frame.token) || frame.challenge !== connection.challenge ||
          frame.sessionId !== await $.session.id() || !(await $.session.surfaces()).includes('desktop'))
        return {consumed: 'CallAgent authorization rejected'};
      pending = frame;
    } catch { return {consumed: 'CallAgent invalid frame'}; }
    return {consumed: 'CallAgent pending receipt'};
  });
  on('session.detach', async ($, event, next) => { ++generation; pending = undefined; return next(event); });
  on('session.end', async ($, event, next) => {
    ++generation; pending = undefined; turns.clear();
    if (event.reason !== 'clear' && event.reason !== 'resume') { ended = true; timer?.cancel(); timer = undefined; }
    return next(event);
  });
}
