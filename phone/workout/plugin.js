import { createGMPlugin } from './vendor/gm-plugin-web-sdk.esm.js';
import {
  SETTINGS_CHANNEL,
  EVENT_CHANNEL,
  EVENT_WORKOUT_DONE,
  MODE_REPS,
  MODE_TIME,
  MAX_EXERCISES,
  MAX_REPS,
  MAX_TIME_SEC,
  MAX_SETS,
  MAX_REST_SEC,
  NAME_MAX,
  encodeProgram,
  encodeStart,
  encodePause,
  encodeStop,
  decodeEvent,
} from './protocol.js';
import { detectLocale, getStrings } from './translations.js';

const STORAGE_KEY = 'workout';
const S = getStrings(detectLocale());

const gm = createGMPlugin();

const statusEl = document.querySelector('#status');
const listEl = document.querySelector('#exercise-list');
const addButton = document.querySelector('#add-exercise');
const summaryEl = document.querySelector('#program-summary');
const startButton = document.querySelector('#start-button');
const stopButton = document.querySelector('#stop-button');
const templateButtons = Array.from(document.querySelectorAll('[data-template]'));
const todayCountEl = document.querySelector('#today-count');
const weekCountEl = document.querySelector('#week-count');
const weekSetsEl = document.querySelector('#week-sets');
const lastSessionEl = document.querySelector('#last-session');

// Templates mirror common routines; each entry is
// { name, mode, target, sets, rest }.
const TEMPLATES = {
  full: [
    { name: S.exerciseSquats, mode: MODE_REPS, target: 12, sets: 3, rest: 75 },
    { name: S.exercisePushups, mode: MODE_REPS, target: 10, sets: 3, rest: 60 },
    { name: S.exerciseLunges, mode: MODE_REPS, target: 10, sets: 3, rest: 60 },
    { name: S.exerciseRow, mode: MODE_REPS, target: 12, sets: 3, rest: 75 },
    { name: S.exercisePlank, mode: MODE_TIME, target: 45, sets: 3, rest: 45 },
    { name: S.exerciseGluteBridge, mode: MODE_REPS, target: 15, sets: 3, rest: 60 },
  ],
  upper: [
    { name: S.exercisePushups, mode: MODE_REPS, target: 12, sets: 3, rest: 60 },
    { name: S.exerciseDips, mode: MODE_REPS, target: 8, sets: 3, rest: 75 },
    { name: S.exerciseRow, mode: MODE_REPS, target: 12, sets: 3, rest: 75 },
    { name: S.exerciseShoulderRaise, mode: MODE_REPS, target: 12, sets: 3, rest: 60 },
    { name: S.exerciseFrontPlank, mode: MODE_TIME, target: 45, sets: 3, rest: 45 },
  ],
  core: [
    { name: S.exercisePlank, mode: MODE_TIME, target: 45, sets: 3, rest: 40 },
    { name: S.exerciseSidePlank, mode: MODE_TIME, target: 30, sets: 3, rest: 40 },
    { name: S.exerciseCrunch, mode: MODE_REPS, target: 20, sets: 3, rest: 40 },
    { name: S.exerciseLegRaise, mode: MODE_REPS, target: 15, sets: 3, rest: 45 },
    { name: S.exerciseSuperman, mode: MODE_TIME, target: 30, sets: 3, rest: 40 },
  ],
};

let exercises = [];
let history = {}; // { 'YYYY-MM-DD': { sessions, sets, seconds } }
let connected = false;
let running = false;
let paused = false;

function setStatus(text, state = '') {
  statusEl.textContent = text;
  statusEl.className = `status ${state}`.trim();
}

function todayKey() {
  const now = new Date();
  const month = String(now.getMonth() + 1).padStart(2, '0');
  const day = String(now.getDate()).padStart(2, '0');
  return `${now.getFullYear()}-${month}-${day}`;
}

function formatTime(iso) {
  const date = new Date(iso);
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function sanitizeExercise(raw) {
  const mode = raw.mode === MODE_TIME ? MODE_TIME : MODE_REPS;
  const targetMax = mode === MODE_TIME ? MAX_TIME_SEC : MAX_REPS;
  return {
    name: String(raw.name || S.defaultExercise).slice(0, NAME_MAX),
    mode,
    target: clamp(Math.round(Number(raw.target) || 1), 1, targetMax),
    sets: clamp(Math.round(Number(raw.sets) || 1), 1, MAX_SETS),
    rest: clamp(Math.round(Number(raw.rest) || 0), 0, MAX_REST_SEC),
  };
}

async function loadState() {
  const result = await gm.storage.get(STORAGE_KEY);
  if (result.value) {
    if (Array.isArray(result.value.exercises)) {
      exercises = result.value.exercises.map(sanitizeExercise);
    }
    if (result.value.history) history = result.value.history;
  }
}

async function persistState() {
  await gm.storage.set(STORAGE_KEY, { exercises, history });
}

function renderExercises() {
  listEl.innerHTML = '';
  exercises.forEach((exercise, index) => {
    const card = document.createElement('div');
    card.className = 'exercise-card';

    const nameRow = document.createElement('div');
    nameRow.className = 'row name-row';
    const nameInput = document.createElement('input');
    nameInput.type = 'text';
    nameInput.maxLength = NAME_MAX;
    nameInput.value = exercise.name;
    nameInput.placeholder = S.placeholderName;
    nameInput.disabled = !connected;
    nameInput.addEventListener('input', () => {
      exercise.name = nameInput.value;
      void persistState();
      void sendProgram();
    });
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'remove-button';
    remove.textContent = '✕';
    remove.disabled = !connected;
    remove.addEventListener('click', () => {
      exercises.splice(index, 1);
      void onProgramChanged();
    });
    nameRow.append(nameInput, remove);

    const fieldsRow = document.createElement('div');
    fieldsRow.className = 'row';

    const modeField = document.createElement('div');
    modeField.className = 'field';
    const modeLabel = document.createElement('label');
    modeLabel.textContent = S.labelType;
    const modeSelect = document.createElement('select');
    modeSelect.disabled = !connected;
    modeSelect.innerHTML =
      `<option value="reps">${S.optionReps}</option><option value="time">${S.optionTime}</option>`;
    modeSelect.value = exercise.mode === MODE_TIME ? 'time' : 'reps';
    modeSelect.addEventListener('change', () => {
      exercise.mode = modeSelect.value === 'time' ? MODE_TIME : MODE_REPS;
      exercise.target = exercise.mode === MODE_TIME ? 45 : 12;
      void onProgramChanged();
    });
    modeField.append(modeLabel, modeSelect);

    const isTime = exercise.mode === MODE_TIME;
    const targetField = document.createElement('div');
    targetField.className = 'field';
    const targetLabel = document.createElement('label');
    targetLabel.textContent = isTime ? S.labelSeconds : S.labelReps;
    const targetInput = document.createElement('input');
    targetInput.type = 'number';
    targetInput.min = '1';
    targetInput.max = String(isTime ? MAX_TIME_SEC : MAX_REPS);
    targetInput.value = String(exercise.target);
    targetInput.disabled = !connected;
    targetInput.addEventListener('input', () => {
      exercise.target = clamp(
        Math.round(Number(targetInput.value) || 1),
        1,
        isTime ? MAX_TIME_SEC : MAX_REPS,
      );
      targetInput.value = String(exercise.target);
      void persistState();
      void sendProgram();
    });
    targetField.append(targetLabel, targetInput);

    const setsField = document.createElement('div');
    setsField.className = 'field';
    const setsLabel = document.createElement('label');
    setsLabel.textContent = S.labelSets;
    const setsInput = document.createElement('input');
    setsInput.type = 'number';
    setsInput.min = '1';
    setsInput.max = String(MAX_SETS);
    setsInput.value = String(exercise.sets);
    setsInput.disabled = !connected;
    setsInput.addEventListener('input', () => {
      exercise.sets = clamp(Math.round(Number(setsInput.value) || 1), 1, MAX_SETS);
      setsInput.value = String(exercise.sets);
      void persistState();
      void sendProgram();
    });
    setsField.append(setsLabel, setsInput);

    const restField = document.createElement('div');
    restField.className = 'field';
    const restLabel = document.createElement('label');
    restLabel.textContent = S.labelRest;
    const restInput = document.createElement('input');
    restInput.type = 'number';
    restInput.min = '0';
    restInput.max = String(MAX_REST_SEC);
    restInput.value = String(exercise.rest);
    restInput.disabled = !connected;
    restInput.addEventListener('input', () => {
      exercise.rest = clamp(Math.round(Number(restInput.value) || 0), 0, MAX_REST_SEC);
      restInput.value = String(exercise.rest);
      void persistState();
      void sendProgram();
    });
    restField.append(restLabel, restInput);

    fieldsRow.append(modeField, targetField, setsField, restField);
    card.append(nameRow, fieldsRow);
    listEl.append(card);
  });
}

function renderSummary() {
  const totalSets = exercises.reduce((sum, exercise) => sum + exercise.sets, 0);
  summaryEl.textContent = S.summary(exercises.length, totalSets);
}

function renderStats() {
  const today = history[todayKey()] || { sessions: 0, sets: 0, seconds: 0 };
  let weekSessions = 0;
  let weekSets = 0;
  const now = new Date();
  for (let i = 0; i < 7; i += 1) {
    const d = new Date(now);
    d.setDate(now.getDate() - i);
    const month = String(d.getMonth() + 1).padStart(2, '0');
    const day = String(d.getDate()).padStart(2, '0');
    const entry = history[`${d.getFullYear()}-${month}-${day}`];
    if (entry) {
      weekSessions += entry.sessions;
      weekSets += entry.sets;
    }
  }
  todayCountEl.textContent = String(today.sessions);
  weekCountEl.textContent = String(weekSessions);
  weekSetsEl.textContent = String(weekSets);
}

function applyStrings() {
  document.title = S.appTitle;
  document.documentElement.lang = detectLocale();
  const set = (id, text) => {
    const el = document.querySelector(`#${id}`);
    if (el) el.textContent = text;
  };
  set('app-title', S.appTitle);
  set('templates-title', S.sectionTemplates);
  set('program-title', S.sectionProgram);
  set('stats-title', S.sectionStats);
  set('add-exercise', S.addExercise);
  set('stop-button', S.stopButton);
  set('program-summary', S.summary(0, 0));
  set('last-session', S.noSession);
  set('stat-today-label', S.statToday);
  set('stat-week-label', S.statWeek);
  set('stat-week-sets-label', S.statWeekSets);
  const templateFull = document.querySelector('#template-full');
  const templateUpper = document.querySelector('#template-upper');
  const templateCore = document.querySelector('#template-core');
  if (templateFull) templateFull.innerHTML = `${S.templateFull}<br><small>${S.templateExercises(6)}</small>`;
  if (templateUpper) templateUpper.innerHTML = `${S.templateUpper}<br><small>${S.templateExercises(5)}</small>`;
  if (templateCore) templateCore.innerHTML = `${S.templateCore}<br><small>${S.templateExercises(5)}</small>`;
}

function renderControls() {
  const busy = running;
  addButton.disabled = !connected || exercises.length >= MAX_EXERCISES;
  templateButtons.forEach((button) => {
    button.disabled = !connected || busy;
  });
  startButton.disabled = !connected || exercises.length === 0;
  stopButton.disabled = !connected || !running;
  startButton.textContent = paused ? S.resumeButton : running ? S.pauseButton : S.startButton;
}

async function sendProgram() {
  if (!connected) return;
  const payload = encodeProgram(exercises);
  if (!payload) return;
  try {
    await gm.plugin.sendMessage(SETTINGS_CHANNEL, payload);
  } catch (error) {
    setStatus(error.message || String(error), 'error');
  }
}

async function onProgramChanged() {
  await persistState();
  renderExercises();
  renderSummary();
  renderControls();
  await sendProgram();
}

async function sendCommand(encoder) {
  if (!connected) return;
  try {
    await gm.plugin.sendMessage(SETTINGS_CHANNEL, encoder());
  } catch (error) {
    setStatus(error.message || String(error), 'error');
  }
}

templateButtons.forEach((button) => {
  button.addEventListener('click', () => {
    const template = TEMPLATES[button.dataset.template];
    if (!template) return;
    exercises = template.map(sanitizeExercise);
    void onProgramChanged();
  });
});

addButton.addEventListener('click', () => {
  if (exercises.length >= MAX_EXERCISES) return;
  exercises.push(sanitizeExercise({ name: S.newExercise, mode: MODE_REPS, target: 12, sets: 3, rest: 60 }));
  void onProgramChanged();
});

startButton.addEventListener('click', () => {
  if (!running) {
    running = true;
    paused = false;
    renderControls();
    void sendCommand(encodeStart);
  } else {
    paused = !paused;
    renderControls();
    void sendCommand(encodePause);
  }
});

stopButton.addEventListener('click', () => {
  running = false;
  paused = false;
  renderControls();
  void sendCommand(encodeStop);
});

const offMessages = gm.plugin.onMessage((message) => {
  if (message.channel !== EVENT_CHANNEL) return;
  const decoded = decodeEvent(message.data);
  if (!decoded) return;
  if (decoded.event === EVENT_WORKOUT_DONE) {
    running = false;
    paused = false;
    const key = todayKey();
    const entry = history[key] || { sessions: 0, sets: 0, seconds: 0 };
    entry.sessions += 1;
    entry.sets += decoded.sets;
    entry.seconds += decoded.seconds;
    history[key] = entry;
    void persistState();
    renderStats();
    renderControls();
    const minutes = Math.round(decoded.seconds / 60);
    lastSessionEl.textContent = S.lastSession(
      formatTime(new Date().toISOString()),
      decoded.sets,
      minutes,
    );
  }
});

async function start() {
  try {
    await gm.ready();
    await loadState();
    applyStrings();
    renderExercises();
    renderSummary();
    renderStats();
    renderControls();
    const info = await gm.device.getInfo();
    connected = Boolean(info.connected);
    setStatus(connected ? S.statusConnected : S.statusDisconnected, connected ? 'ready' : 'error');
    renderExercises();
    renderControls();
    await sendProgram();
    await gm.device.subscribeEvents(['connection']);
    gm.device.onConnection((event) => {
      connected = Boolean(event.connected);
      setStatus(connected ? S.statusConnected : S.statusDisconnected, connected ? 'ready' : 'error');
      renderExercises();
      renderControls();
      if (connected) void sendProgram();
    });
  } catch (error) {
    setStatus(error.message || String(error), 'error');
  }
}

window.addEventListener('pagehide', () => {
  offMessages();
  gm.close();
});

void start();
