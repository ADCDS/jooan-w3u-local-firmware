import assert from 'node:assert/strict';
import {
  AudioWireType,
  alawDecodeSample,
  alawEncodeSample,
  buildAudioFrame,
  parseAudioFrame,
} from './audio-codec.js';

const vectors = [
  [0, 0xd5, 8], [-1, 0x55, -8], [16, 0xd4, 24], [-31, 0x54, -24],
  [1000, 0xfa, 1008], [-1000, 0x7a, -1008],
  [4096, 0x85, 4224], [-4096, 0x1a, -4032],
  [32767, 0xaa, 32256], [-32768, 0x2a, -32256],
];
for (const [pcm, encoded, decoded] of vectors) {
  assert.equal(alawEncodeSample(pcm), encoded);
  assert.equal(alawDecodeSample(encoded), decoded);
}

const payload = Uint8Array.of(0xd5, 0x55, 0xfa);
const encoded = buildAudioFrame(AudioWireType.PTT_PCMA, 7, payload);
const parsed = parseAudioFrame(encoded);
assert.equal(parsed.type, AudioWireType.PTT_PCMA);
assert.equal(parsed.sequence, 7);
assert.deepEqual([...parsed.payload], [...payload]);
assert.throws(() => parseAudioFrame(encoded.subarray(0, encoded.length - 1)));
assert.throws(() => buildAudioFrame(AudioWireType.PTT_RELEASE, 8, payload));

console.log('browser audio tests: ok');
