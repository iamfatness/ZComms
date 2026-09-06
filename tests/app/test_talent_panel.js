// Exercise the actual embedded panel function without a browser or Zoom SDK.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../../src/app/ui_html.h'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script); // Syntax-check the complete panel, not just the function.
const fn = script.slice(script.indexOf('function cellRows(){'), script.indexOf('function buildGrid('));
function rows(state) {
  const context = vm.createContext({ S: state });
  return JSON.parse(JSON.stringify(vm.runInContext(fn + '\ncellRows()', context)));
}
assert.deepEqual(rows({ roster: [{ uid: 2, name: 'Web guest', tb: false }], channels: [] }), []);
assert.deepEqual(rows({ roster: [{ uid: 2, name: 'Web guest', tb: false }],
  channels: [{ listeners: 0, label: '', keyed: false, latched: false }] }), []);
assert.deepEqual(rows({ roster: [{ uid: 1, name: 'Pat', tb: true }, { uid: 2, name: 'Pat', tb: false }],
  channels: [{ listeners: 1, label: 'Pat' }] }), [{ slot: 0, name: 'Pat', group: false }]);
assert.deepEqual(rows({ channels: [{ listeners: 2, label: '' }] }),
  [{ slot: 0, name: 'CH 1', group: true }]);
console.log('Talent panel regression tests passed.');
