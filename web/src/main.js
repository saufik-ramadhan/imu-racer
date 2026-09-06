import { loadAssets } from './assets.js';
import { Game, State } from './game.js';
import { Input } from './input.js';
import * as ble from './ble.js';
import * as usb from './serial.js';
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
  connectUsb: $('btn-connect-usb'),
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

  // Two boards, two transports. The ESP32-S3 build advertises over BLE; the
  // STM32H7 build has no radio and streams the same packets down the ST-LINK
  // virtual COM port. Whichever connects first becomes the active controller.
  const controllers = {
    ble: new ble.Controller(),
    usb: new usb.Controller(),
  };
  input.attachController(controllers.ble);

  hide(el.loading);
  scores.render(el.scoreBody, scores.load());

  /* ----------------------------- Controllers ----------------------------- */
  function wireLink(button, controller, mod, scanning, idleLabel) {
    const reason = mod.unsupportedReason();
    if (reason) {
      button.disabled = true;
      button.textContent = 'UNAVAILABLE';
      el.bleStatus.textContent = reason;
      return;
    }

    button.addEventListener('click', async () => {
      button.disabled = true;
      el.bleStatus.className = 'status';
      el.bleStatus.textContent = scanning;
      try {
        const name = await controller.connect();
        input.attachController(controller);
        el.bleStatus.className = 'status ok';
        el.bleStatus.textContent = `Connected to ${name}. Hold it level, then tilt to steer.`;
        button.textContent = 'CONNECTED';
        el.thumb.classList.add('ble');
      } catch (err) {
        button.disabled = false;
        el.bleStatus.className = 'status err';
        // NotFoundError covers both "user cancelled" and "the list was empty",
        // which the page cannot tell apart, so each transport explains its own
        // most likely cause.
        el.bleStatus.textContent = err.name === 'NotFoundError' ? mod.notFoundHint : err.message;
      }
    });

    controller.addEventListener('disconnected', () => {
      button.disabled = false;
      button.textContent = idleLabel;
      el.bleStatus.className = 'status err';
      el.bleStatus.textContent = 'Controller disconnected. Keyboard still works.';
      el.thumb.classList.remove('ble');
    });
  }

  wireLink(el.connect, controllers.ble, ble, 'Scanning…', 'CONNECT CONTROLLER');
  if (el.connectUsb) {
    wireLink(el.connectUsb, controllers.usb, usb, 'Choose the port…', 'CONNECT VIA USB');
  }

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
