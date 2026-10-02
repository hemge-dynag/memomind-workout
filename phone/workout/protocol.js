// Binary wire protocol shared with GlassSDK/examples/workout/workout.c.
// Keep both sides in sync if you change any layout or constant here.

export const SETTINGS_CHANNEL = 0x574b; // 'WK', phone -> glasses
export const EVENT_CHANNEL = 0x574c; // 'WL', glasses -> phone
export const PROTOCOL_VERSION = 1;

export const COMMAND_PROGRAM = 1;
export const COMMAND_START = 2;
export const COMMAND_PAUSE = 3;
export const COMMAND_STOP = 4;
export const COMMAND_SKIP = 5;

export const EVENT_WORKOUT_DONE = 1;

export const MODE_REPS = 0;
export const MODE_TIME = 1;

export const MAX_EXERCISES = 10;
export const NAME_MAX = 16;
export const MIN_TARGET = 1;
export const MAX_REPS = 99;
export const MAX_TIME_SEC = 300;
export const MIN_SETS = 1;
export const MAX_SETS = 20;
export const MAX_REST_SEC = 300;

// Encodes a program as:
//   [version, COMMAND_PROGRAM, count,
//    (name_len, name..., mode, target_lo, target_hi, sets, rest_lo, rest_hi)*]
// Returns null when there is nothing to send.
export function encodeProgram(exercises) {
  if (!Array.isArray(exercises) || exercises.length === 0) return null;
  const list = exercises.slice(0, MAX_EXERCISES);
  const bytes = [PROTOCOL_VERSION, COMMAND_PROGRAM, list.length];
  for (const exercise of list) {
    const nameBytes = [];
    const name = String(exercise.name || '').slice(0, NAME_MAX);
    for (let i = 0; i < name.length; i += 1) {
      const code = name.charCodeAt(i);
      nameBytes.push(code > 0xff ? 0x3f : code); // '?' for non-Latin-1
    }
    bytes.push(nameBytes.length);
    bytes.push(...nameBytes);
    const mode = exercise.mode === MODE_TIME ? MODE_TIME : MODE_REPS;
    const target = Math.round(exercise.target || 0);
    const sets = Math.round(exercise.sets || 1);
    const rest = Math.round(exercise.rest || 0);
    bytes.push(mode);
    bytes.push(target & 0xff);
    bytes.push((target >> 8) & 0xff);
    bytes.push(sets & 0xff);
    bytes.push(rest & 0xff);
    bytes.push((rest >> 8) & 0xff);
  }
  return new Uint8Array(bytes);
}

function command(opcode) {
  return new Uint8Array([PROTOCOL_VERSION, opcode]);
}

export function encodeStart() {
  return command(COMMAND_START);
}

export function encodePause() {
  return command(COMMAND_PAUSE);
}

export function encodeStop() {
  return command(COMMAND_STOP);
}

export function encodeSkip() {
  return command(COMMAND_SKIP);
}

// Decodes a glasses -> phone event. Returns null for anything unrecognized.
export function decodeEvent(data) {
  if (!data || data.length < 6 || data[0] !== PROTOCOL_VERSION) return null;
  if (data[1] === EVENT_WORKOUT_DONE) {
    const sets = data[2] | (data[3] << 8);
    const seconds = data[4] | (data[5] << 8);
    return { event: EVENT_WORKOUT_DONE, sets, seconds };
  }
  return null;
}
