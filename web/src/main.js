import { loadAssets } from './assets.js';
import { Game, State } from './game.js';
import { Input } from './input.js';
import { Controller, unsupportedReason } from './ble.js';
import * as scores from './scores.js';

const $ = (id) => document.getElementById(id);

const el = {
  stage: $('stage'),
  canvas: $('scene'),
  hud: $('hud'),
  score: $('score'),
  speed: $('speed'),
  tilt: $('tilt'),
  thumb: $('tilt-thumb'),
  menu: $('menu'),
  gameover: $('gameover'),
  loading: $('loading'),
  play: $('btn-play'),
  connect: $('btn-connect'),
  again: $('btn-again'),
  toMenu: $('btn-menu'),
  bleStatus: $('ble-status'),
  scoreBody: $('score-table').querySelector('tbody'),
  finalScore: $('final-score'),
  finalDodged: $('final-dodged'),
  finalSpeed: $('final-speed'),
  newBest: $('new-best'),
};

const show = (node) => node.classList.remove('hidden');
const hide = (node) => node.classList.add('hidden');

async function boot() {
  let assets;
  try {
    assets = await loadAssets();
  } catch (err) {
    el.loading.innerHTML = `<p class="status err">Could not load assets.<br>${err.message}</p>`;
    return;
  }

  const game = new Game(el.canvas, assets, el.stage);
  const input = new Input(el.stage);
  const controller = new Controller();
  input.attachController(controller);

  hide(el.loading);
  scores.render(el.scoreBody, scores.load());

  /* ------------------------------ Bluetooth ------------------------------ */
  const reason = unsupportedReason();
  if (reason) {
    el.connect.disabled = true;
    el.connect.textContent = 'CONTROLLER UNAVAILABLE';
    el.bleStatus.textContent = reason;
  }

  el.connect.addEventListener('click', async () => {
    el.connect.disabled = true;
    el.bleStatus.className = 'status';
    el.bleStatus.textContent = 'Scanning…';
    try {
      const name = await controller.connect();
      el.bleStatus.className = 'status ok';
      el.bleStatus.textContent = `Connected to ${name}. Hold it level, then tilt to steer.`;
      el.connect.textContent = 'CONNECTED';
      el.thumb.classList.add('ble');
    } catch (err) {
      el.connect.disabled = false;
      el.bleStatus.className = 'status err';
      // NotFoundError covers both "user cancelled" and "the list was empty",
      // which the page cannot tell apart -- but an empty list almost always
      // means something else is holding the connection, since a peripheral
      // stops advertising while connected.
      el.bleStatus.textContent = err.name === 'NotFoundError'
        ? 'No controller picked. If the list was empty: the board only talks to '
          + 'one thing at a time, so disconnect it from any other Bluetooth page '
          + 'or the OS settings, and check it is still advertising.'
        : err.message;
    }
  });

  controller.addEventListener('disconnected', () => {
    el.connect.disabled = false;
    el.connect.textContent = 'CONNECT CONTROLLER';
    el.bleStatus.className = 'status err';
    el.bleStatus.textContent = 'Controller disconnected. Keyboard still works.';
    el.thumb.classList.remove('ble');
  });

  /* -------------------------------- Flow --------------------------------- */
  function startRun() {
    input.reset();
    hide(el.menu);
    hide(el.gameover);
    show(el.hud);
    show(el.tilt);
    game.start();
  }

  function toMenu() {
    hide(el.gameover);
    hide(el.hud);
    hide(el.tilt);
    show(el.menu);
    scores.render(el.scoreBody, scores.load());
    game.reset();
  }

  el.play.addEventListener('click', startRun);
  el.again.addEventListener('click', startRun);
  el.toMenu.addEventListener('click', toMenu);

  addEventListener('keydown', (e) => {
    if (e.key !== ' ' && e.key !== 'Enter') return;
    e.preventDefault();
    if (game.state === State.READY || game.state === State.OVER) startRun();
  });

  game.onGameOver = (result) => {
    const { table, rank, isBest } = scores.submit(result);
    el.finalScore.textContent = result.score.toLocaleString();
    el.finalDodged.textContent = result.dodged;
    el.finalSpeed.textContent = `${result.speed.toFixed(1)}x`;
    el.newBest.classList.toggle('hidden', !isBest);
    scores.render(el.scoreBody, table, rank);
    hide(el.hud);
    hide(el.tilt);
    show(el.gameover);
  };

  // Handles for poking at a running game from the console during development.
  if (import.meta.env.DEV) Object.assign(window, { __game: game, __input: input });

  /* -------------------------------- Loop --------------------------------- */
  let last = performance.now();

  function frame(now) {
    // Clamp dt so a backgrounded tab does not teleport the player into traffic.
    const dt = Math.min(0.05, (now - last) / 1000);
    last = now;

    const steer = input.update(dt);
    game.update(dt, steer);

    if (game.state === State.RUNNING) {
      el.score.textContent = Math.round(game.score).toLocaleString();
      el.speed.textContent = game.speedMul.toFixed(1);
      el.thumb.style.left = `${50 + steer * 45}%`;
    }

    requestAnimationFrame(frame);
  }

  requestAnimationFrame(frame);
}

boot();
