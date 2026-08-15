import * as THREE from 'three';

/* ------------------------------- Tuning ----------------------------------- */
const VIEW_H = 16;            // world units visible vertically
const ROAD_W = 6;             // three lanes of 2 units
const LANES = [-2, 0, 2];
const SHOULDER = 1.35;        // grass either side of the tarmac

const CAR_W = 1.45;
const PLAYER_Y = -VIEW_H / 2 + 3.2;
const STEER_RANGE = ROAD_W / 2 - CAR_W / 2 + 0.15;  // keep a wheel on the road

const BASE_SPEED = 11;        // world units per second at 1.0x
const MAX_SPEED_MUL = 3.6;
const RAMP = 0.030;           // speed multiplier gained per second
const SPAWN_BASE = 1.35;      // seconds between waves at 1.0x

/**
 * Traffic closes at a fixed fraction of the player's speed, so you overtake
 * rather than the road sliding past parked cars.
 *
 * This has to be the same for every car. Giving each one its own speed lets a
 * slow car from one wave drift alongside a fast car from the next, which can
 * block all three lanes at once and make the run unwinnable.
 */
const TRAFFIC_REL = 0.5;
const CAR_LEN = CAR_W * 2.0;      // the sprites are almost exactly 1:2
const MIN_WAVE_GAP = CAR_LEN * 1.6;

const HIT_W = CAR_W * 0.72;   // forgiving collision box
const HIT_H = CAR_W * 2.0 * 0.78;

const PLAYER_COLOR = 0;       // red
const OBSTACLE_COLORS = [1, 2, 3]; // green, yellow, blue

export const State = { READY: 'ready', RUNNING: 'running', CRASHED: 'crashed', OVER: 'over' };

export class Game {
  constructor(canvas, assets, stage) {
    this.canvas = canvas;
    this.assets = assets;
    this.stage = stage;
    this.state = State.READY;
    this.onGameOver = null;

    this.renderer = new THREE.WebGLRenderer({
      canvas,
      antialias: true,
      powerPreference: 'high-performance',
    });
    this.renderer.setClearColor(0x0a1410, 1);
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;

    this.scene = new THREE.Scene();
    this.camera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.1, 100);
    this.camera.position.z = 10;

    this._buildRoad();
    this._buildScenery();
    this._buildPlayer();
    this._buildExplosion();

    this.obstacles = [];
    this.pool = [];

    // Observing the element rather than listening for window resize: the stage
    // can still be zero-sized when the constructor runs, and a window event
    // would never arrive to correct it.
    this._resizeObserver = new ResizeObserver(() => this.resize());
    this._resizeObserver.observe(this.stage);
    this.resize();

    this.reset();
  }

  /* ----------------------------- Scene build ------------------------------ */

  _buildRoad() {
    const { texture } = this.assets.road;
    this.roadTexture = texture;

    // One texture tile spans the full road width; repeat.y follows from the
    // texture's own aspect so the lane dashes keep their proportions.
    const { width: tw, height: th } = this.assets.road;
    const tileWorldH = ROAD_W * (th / tw);
    const planeH = VIEW_H + 8;
    texture.repeat.set(1, planeH / tileWorldH);
    this.scrollPerUnit = 1 / tileWorldH;

    const geo = new THREE.PlaneGeometry(ROAD_W, planeH);
    const mat = new THREE.MeshBasicMaterial({ map: texture });
    this.road = new THREE.Mesh(geo, mat);
    this.road.position.z = 0;
    this.scene.add(this.road);
  }

  /**
   * Verge strips either side. They scroll with the road so there is a sense of
   * motion even where the tarmac texture is flat grey.
   */
  _buildScenery() {
    this.verges = [];
    for (const side of [-1, 1]) {
      const group = new THREE.Group();
      for (let i = 0; i < 14; i++) {
        const post = new THREE.Mesh(
          new THREE.PlaneGeometry(0.22, 1.1),
          new THREE.MeshBasicMaterial({ color: 0xdfe8ef })
        );
        post.position.set(side * (ROAD_W / 2 + SHOULDER * 0.45), i * 2.2 - VIEW_H, 0.2);
        group.add(post);
      }
      this.scene.add(group);
      this.verges.push(group);
    }
  }

  _makeCar(colorIndex) {
    const { texture, aspect } = this.assets.cars[colorIndex];
    const h = CAR_W / aspect;
    const mesh = new THREE.Mesh(
      new THREE.PlaneGeometry(CAR_W, h),
      new THREE.MeshBasicMaterial({ map: texture, transparent: true, alphaTest: 0.35 })
    );
    mesh.position.z = 1;
    return mesh;
  }

  _buildPlayer() {
    this.player = this._makeCar(PLAYER_COLOR);
    this.player.position.set(0, PLAYER_Y, 1);
    this.scene.add(this.player);
  }

  _buildExplosion() {
    this.boom = new THREE.Mesh(
      new THREE.PlaneGeometry(3.4, 3.4),
      new THREE.MeshBasicMaterial({
        map: this.assets.explosion[0],
        transparent: true,
        depthWrite: false,
      })
    );
    this.boom.position.z = 2;
    this.boom.visible = false;
    this.scene.add(this.boom);
  }

  /* -------------------------------- Sizing -------------------------------- */

  resize() {
    const w = this.stage.clientWidth;
    const h = this.stage.clientHeight;
    if (!w || !h) return;

    this.renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
    this.renderer.setSize(w, h, false);

    const viewW = VIEW_H * (w / h);
    this.camera.left = -viewW / 2;
    this.camera.right = viewW / 2;
    this.camera.top = VIEW_H / 2;
    this.camera.bottom = -VIEW_H / 2;
    this.camera.updateProjectionMatrix();
  }

  /* -------------------------------- Rounds -------------------------------- */

  reset() {
    for (const o of this.obstacles) {
      o.mesh.visible = false;
      this.pool.push(o);
    }
    this.obstacles.length = 0;

    this.elapsed = 0;
    this.distance = 0;
    this.score = 0;
    this.dodged = 0;
    this.speedMul = 1;
    this.topSpeedMul = 1;
    this.spawnTimer = 0.8;
    this.freeLane = 1;
    this.crashTimer = 0;

    this.player.position.set(0, PLAYER_Y, 1);
    this.player.rotation.z = 0;
    this.player.visible = true;
    this.boom.visible = false;
    this.roadTexture.offset.y = 0;

    this.state = State.READY;
  }

  start() {
    this.reset();
    this.state = State.RUNNING;
  }

  /* -------------------------------- Spawning ------------------------------ */

  _takeObstacle(colorIndex) {
    const found = this.pool.findIndex((o) => o.colorIndex === colorIndex);
    if (found >= 0) {
      const o = this.pool.splice(found, 1)[0];
      o.mesh.visible = true;
      return o;
    }
    const mesh = this._makeCar(colorIndex);
    this.scene.add(mesh);
    return { mesh, colorIndex, lane: 0, counted: false };
  }

  /**
   * Spawn one or two cars, always leaving a gap. The free lane only ever moves
   * by one, so consecutive waves are always reachable however fast it gets.
   */
  _spawnWave() {
    // Never crowd the previous wave, whatever the tuning above says.
    const highest = this.obstacles.reduce((m, o) => Math.max(m, o.mesh.position.y), -Infinity);
    const spawnY = VIEW_H / 2 + 4;
    if (highest > spawnY - MIN_WAVE_GAP) return;

    const shift = Math.floor(Math.random() * 3) - 1;
    this.freeLane = clamp(this.freeLane + shift, 0, 2);

    const blocked = [0, 1, 2].filter((l) => l !== this.freeLane);
    // Pairs stay rare early on and become the norm once you are up to speed.
    const pairChance = Math.min(0.55, 0.15 + this.elapsed / 150);
    const chosen = Math.random() < pairChance
      ? blocked
      : [blocked[Math.floor(Math.random() * blocked.length)]];

    for (const lane of chosen) {
      const colorIndex = OBSTACLE_COLORS[Math.floor(Math.random() * OBSTACLE_COLORS.length)];
      const o = this._takeObstacle(colorIndex);
      o.lane = lane;
      o.counted = false;
      // Small jitter within the wave, kept well under MIN_WAVE_GAP.
      o.mesh.position.set(LANES[lane], spawnY + Math.random() * 0.8, 1);
      this.obstacles.push(o);
    }
  }

  /* --------------------------------- Loop --------------------------------- */

  update(dt, steer) {
    if (this.state === State.RUNNING) this._updateRunning(dt, steer);
    else if (this.state === State.CRASHED) this._updateCrash(dt);

    this.renderer.render(this.scene, this.camera);
  }

  _updateRunning(dt, steer) {
    this.elapsed += dt;
    this.speedMul = Math.min(MAX_SPEED_MUL, 1 + this.elapsed * RAMP);
    this.topSpeedMul = Math.max(this.topSpeedMul, this.speedMul);

    const speed = BASE_SPEED * this.speedMul;
    const travel = speed * dt;
    this.distance += travel;

    // Surviving at speed is worth more than surviving slowly.
    this.score += dt * 12 * this.speedMul;

    this.roadTexture.offset.y -= travel * this.scrollPerUnit;

    for (const group of this.verges) {
      for (const post of group.children) {
        post.position.y -= travel;
        if (post.position.y < -VIEW_H / 2 - 2) post.position.y += 14 * 2.2;
      }
    }

    // Steering: position follows the stick, and the sprite banks into the turn.
    const targetX = steer * STEER_RANGE;
    const dx = targetX - this.player.position.x;
    this.player.position.x += dx * Math.min(1, dt * 12);
    this.player.rotation.z = -clamp(dx * 0.5, -0.32, 0.32);

    this.spawnTimer -= dt;
    if (this.spawnTimer <= 0) {
      this._spawnWave();
      this.spawnTimer = Math.max(0.36, SPAWN_BASE / this.speedMul);
    }

    for (let i = this.obstacles.length - 1; i >= 0; i--) {
      const o = this.obstacles[i];
      o.mesh.position.y -= travel * (1 - TRAFFIC_REL);

      if (!o.counted && o.mesh.position.y < PLAYER_Y - 1.6) {
        o.counted = true;
        this.dodged++;
        this.score += 60;
      }

      if (o.mesh.position.y < -VIEW_H / 2 - 4) {
        o.mesh.visible = false;
        this.pool.push(o);
        this.obstacles.splice(i, 1);
        continue;
      }

      if (this._hits(o)) {
        this._crash(o);
        return;
      }
    }
  }

  _hits(o) {
    const px = this.player.position.x;
    const ox = o.mesh.position.x;
    const oy = o.mesh.position.y;
    return Math.abs(px - ox) < HIT_W && Math.abs(PLAYER_Y - oy) < HIT_H;
  }

  _crash(o) {
    this.state = State.CRASHED;
    this.crashTimer = 0;

    this.boom.position.set(
      (this.player.position.x + o.mesh.position.x) / 2,
      (PLAYER_Y + o.mesh.position.y) / 2,
      2
    );
    this.boom.material.map = this.assets.explosion[0];
    this.boom.material.opacity = 1;
    this.boom.material.needsUpdate = true;
    this.boom.visible = true;
  }

  _updateCrash(dt) {
    this.crashTimer += dt;

    const FRAME = 0.13;
    const frame = Math.floor(this.crashTimer / FRAME);
    if (frame < this.assets.explosion.length) {
      this.boom.material.map = this.assets.explosion[frame];
      this.boom.material.needsUpdate = true;
      const grow = 1 + this.crashTimer * 1.4;
      this.boom.scale.setScalar(grow);
    } else {
      this.boom.material.opacity = Math.max(0, 1 - (this.crashTimer - 0.39) * 3);
    }

    this.player.visible = Math.floor(this.crashTimer * 20) % 2 === 0;

    if (this.crashTimer > 0.85) {
      this.boom.visible = false;
      this.player.visible = true;
      this.boom.scale.setScalar(1);
      this.state = State.OVER;
      this.onGameOver?.({
        score: Math.round(this.score),
        dodged: this.dodged,
        speed: this.topSpeedMul,
      });
    }
  }
}

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
