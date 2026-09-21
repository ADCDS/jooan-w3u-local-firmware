import assert from 'node:assert/strict';
import test from 'node:test';
import { pick, direction, isBack } from './spatial.js';

/* pick() is pure geometry, so it needs nothing but rectangles. */
const box = (x, y, w, h) => ({
  name: `${x},${y}`,
  getBoundingClientRect: () => ({ left: x, right: x + w, top: y, bottom: y + h, width: w, height: h }),
});

test('down lands on the nearest row, not the better-centred one below it', () => {
  // The layout that broke naive nearest-neighbour: a narrow chip, a wide
  // button just beneath it slightly off-centre, and a small control two rows
  // down that happens to sit almost exactly under the chip.
  const chip = box(456, 3767, 94, 41);
  const wideButton = box(376, 3821, 411, 44);
  const wellCentred = box(559, 3935, 37, 44);
  const chosen = pick(chip, [wideButton, wellCentred], 'down', false);
  assert.equal(chosen, wideButton, 'must not skip the row directly beneath');
});

test('left and right stay inside their row', () => {
  const button = box(100, 100, 200, 40);
  const elsewhere = box(400, 400, 100, 40);   // to the right, but a different row
  assert.equal(pick(button, [elsewhere], 'right', false), null,
    'nothing in the row means nothing happens, not a diagonal jump');
});

test('a rail may escape its row to reach the content beside it', () => {
  const railItem = box(0, 100, 180, 44);
  const content = box(400, 400, 100, 40);
  assert.equal(pick(railItem, [content], 'right', true), content);
});

test('up and down may always cross rows', () => {
  const from = box(100, 100, 40, 40);
  const below = box(600, 300, 40, 40);        // far off-axis, but below
  assert.equal(pick(from, [below], 'down', false), below);
});

test('candidates behind the direction of travel are ignored', () => {
  const from = box(100, 100, 40, 40);
  const above = box(100, 20, 40, 40);
  assert.equal(pick(from, [above], 'down', false), null);
});

test('nearest wins among several in the same column', () => {
  const from = box(100, 100, 40, 40);
  const near = box(100, 200, 40, 40);
  const far = box(100, 400, 40, 40);
  assert.equal(pick(from, [far, near], 'down', false), near);
});

test('key map covers arrows, legacy names and Android key codes', () => {
  assert.equal(direction({ key: 'ArrowLeft' }), 'left');
  assert.equal(direction({ key: 'Left' }), 'left');          // older WebKit
  assert.equal(direction({ key: 'Unidentified', keyCode: 22 }), 'right');
  assert.equal(direction({ key: 'Unidentified', keyCode: 19 }), 'up');
  assert.equal(direction({ key: 'a' }), null);
  assert.ok(isBack({ key: 'Escape' }));
  assert.ok(isBack({ key: 'Unidentified', keyCode: 4 }));    // Android BACK
  assert.ok(!isBack({ key: 'Enter' }));
});
