export const AUDIO_SAMPLE_RATE = 16000;
export const AUDIO_PACKET_SAMPLES = 320;
export const AUDIO_WIRE_HEADER_SIZE = 16;
export const AUDIO_WIRE_MAX_PAYLOAD = 1024;
export const AUDIO_WS_PROTOCOL = 'jaud.v1';
export const AUDIO_WS_AUTH_PREFIX = 'jaud.auth.';
export const AUDIO_MAX_TALK_MS = 60000;
export const AUDIO_BACKPRESSURE_BYTES = 64 * 1024;

export const AudioWireType = Object.freeze({
  PTT_ACQUIRE: 0x01,
  PTT_PCMA: 0x02,
  PTT_RELEASE: 0x03,
  MIC_PCMA: 0x81,
  STATE: 0x82,
  ERROR: 0xff,
});

export const AudioState = Object.freeze({
  ACQUIRED: 1,
  RELEASED: 2,
  LEASE_EXPIRED: 3,
});

const segmentEnds = [
  0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff,
];

export function alawEncodeSample(sample) {
  let pcm = Math.max(-32768, Math.min(32767, sample | 0));
  const mask = pcm >= 0 ? 0xd5 : 0x55;
  if (pcm < 0) pcm = -pcm - 1;
  pcm = Math.min(pcm, 32635) >> 3;
  let segment = 0;
  while (segment < 8 && pcm > segmentEnds[segment]) segment += 1;
  if (segment >= 8) return (0x7f ^ mask) & 0xff;
  const quantized = segment < 2 ? pcm >> 1 : pcm >> segment;
  return (((segment << 4) | (quantized & 0x0f)) ^ mask) & 0xff;
}

export function alawDecodeSample(encoded) {
  const value = (encoded & 0xff) ^ 0x55;
  const segment = (value & 0x70) >> 4;
  let pcm = (value & 0x0f) << 4;
  if (segment === 0) {
    pcm += 8;
  } else {
    pcm += 0x108;
    if (segment > 1) pcm <<= segment - 1;
  }
  return (value & 0x80) !== 0 ? pcm : -pcm;
}

export function encodeAlaw(pcm) {
  const encoded = new Uint8Array(pcm.length);
  for (let i = 0; i < pcm.length; i += 1) {
    encoded[i] = alawEncodeSample(pcm[i]);
  }
  return encoded;
}

export function decodeAlaw(encoded) {
  const bytes = encoded instanceof Uint8Array ? encoded : new Uint8Array(encoded);
  const pcm = new Int16Array(bytes.length);
  for (let i = 0; i < bytes.length; i += 1) {
    pcm[i] = alawDecodeSample(bytes[i]);
  }
  return pcm;
}

function validWireType(type) {
  return Object.values(AudioWireType).includes(type);
}

export function buildAudioFrame(type, sequence, payload = new Uint8Array(0), flags = 0) {
  const bytes = payload instanceof Uint8Array ? payload : new Uint8Array(payload);
  if (!validWireType(type) || !Number.isInteger(sequence) || sequence <= 0 ||
      sequence > 0xffffffff || bytes.byteLength > AUDIO_WIRE_MAX_PAYLOAD) {
    throw new TypeError('invalid audio frame');
  }
  if ((type === AudioWireType.PTT_ACQUIRE || type === AudioWireType.PTT_RELEASE) &&
      bytes.byteLength !== 0) {
    throw new TypeError('control frame has a payload');
  }
  if ((type === AudioWireType.PTT_PCMA || type === AudioWireType.MIC_PCMA) &&
      bytes.byteLength === 0) {
    throw new TypeError('audio frame has no payload');
  }
  const result = new Uint8Array(AUDIO_WIRE_HEADER_SIZE + bytes.byteLength);
  result.set([0x4a, 0x41, 0x55, 0x44, 1, type, flags & 0xff, AUDIO_WIRE_HEADER_SIZE]);
  const view = new DataView(result.buffer);
  view.setUint32(8, sequence, false);
  view.setUint16(12, bytes.byteLength, false);
  view.setUint16(14, 0, false);
  result.set(bytes, AUDIO_WIRE_HEADER_SIZE);
  return result;
}

export function parseAudioFrame(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  if (bytes.byteLength < AUDIO_WIRE_HEADER_SIZE || bytes[0] !== 0x4a ||
      bytes[1] !== 0x41 || bytes[2] !== 0x55 || bytes[3] !== 0x44 ||
      bytes[4] !== 1 || bytes[7] !== AUDIO_WIRE_HEADER_SIZE ||
      !validWireType(bytes[5])) {
    throw new TypeError('invalid audio frame header');
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const payloadLength = view.getUint16(12, false);
  if (view.getUint16(14, false) !== 0 ||
      payloadLength > AUDIO_WIRE_MAX_PAYLOAD ||
      bytes.byteLength !== AUDIO_WIRE_HEADER_SIZE + payloadLength) {
    throw new TypeError('invalid audio frame length');
  }
  const type = bytes[5];
  if ((type === AudioWireType.PTT_ACQUIRE || type === AudioWireType.PTT_RELEASE) &&
      payloadLength !== 0) {
    throw new TypeError('control frame has a payload');
  }
  if ((type === AudioWireType.PTT_PCMA || type === AudioWireType.MIC_PCMA) &&
      payloadLength === 0) {
    throw new TypeError('audio frame has no payload');
  }
  return {
    type,
    flags: bytes[6],
    sequence: view.getUint32(8, false),
    payload: bytes.subarray(AUDIO_WIRE_HEADER_SIZE),
  };
}
