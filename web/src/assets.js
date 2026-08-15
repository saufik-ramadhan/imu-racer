import * as THREE from 'three';

const BASE = import.meta.env.BASE_URL;

function loadImage(url) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.crossOrigin = 'anonymous';
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error(`failed to load ${url}`));
    img.src = url;
  });
}

function canvasOf(w, h) {
  const c = document.createElement('canvas');
  c.width = w;
  c.height = h;
  return c;
}

function textureOf(canvas) {
  const t = new THREE.CanvasTexture(canvas);
  t.colorSpace = THREE.SRGBColorSpace;
  t.anisotropy = 4;
  t.minFilter = THREE.LinearMipmapLinearFilter;
  t.magFilter = THREE.LinearFilter;
  t.needsUpdate = true;
  return t;
}

/**
 * Split a horizontal sprite sheet on fully transparent columns, then crop each
 * sprite to its tight bounding box. The car sheet has generous gaps between the
 * four vehicles, so this is more robust than assuming an even grid.
 */
function sliceByGaps(img, expected) {
  const w = img.naturalWidth;
  const h = img.naturalHeight;
  const sheet = canvasOf(w, h);
  const ctx = sheet.getContext('2d', { willReadFrequently: true });
  ctx.drawImage(img, 0, 0);
  const data = ctx.getImageData(0, 0, w, h).data;

  const occupied = new Array(w).fill(false);
  for (let x = 0; x < w; x++) {
    for (let y = 0; y < h; y++) {
      if (data[(y * w + x) * 4 + 3] > 16) {
        occupied[x] = true;
        break;
      }
    }
  }

  const spans = [];
  let start = -1;
  for (let x = 0; x <= w; x++) {
    if (x < w && occupied[x]) {
      if (start < 0) start = x;
    } else if (start >= 0) {
      if (x - start > w * 0.02) spans.push([start, x - 1]);
      start = -1;
    }
  }

  // If the sheet ever changes shape, fall back to an even split rather than
  // rendering nothing.
  if (expected && spans.length !== expected) {
    spans.length = 0;
    const step = Math.floor(w / expected);
    for (let i = 0; i < expected; i++) spans.push([i * step, (i + 1) * step - 1]);
  }

  return spans.map(([x0, x1]) => {
    let y0 = h;
    let y1 = 0;
    for (let x = x0; x <= x1; x++) {
      for (let y = 0; y < h; y++) {
        if (data[(y * w + x) * 4 + 3] > 16) {
          if (y < y0) y0 = y;
          if (y > y1) y1 = y;
        }
      }
    }
    if (y1 < y0) { y0 = 0; y1 = h - 1; }
    const sw = x1 - x0 + 1;
    const sh = y1 - y0 + 1;
    const out = canvasOf(sw, sh);
    out.getContext('2d').drawImage(sheet, x0, y0, sw, sh, 0, 0, sw, sh);
    return { canvas: out, width: sw, height: sh };
  });
}

/** Slice a strip of N equally sized square-ish frames (the explosion). */
function sliceStrip(img, frames) {
  const fw = Math.floor(img.naturalWidth / frames);
  const fh = img.naturalHeight;
  const out = [];
  for (let i = 0; i < frames; i++) {
    const c = canvasOf(fw, fh);
    c.getContext('2d').drawImage(img, i * fw, 0, fw, fh, 0, 0, fw, fh);
    out.push(textureOf(c));
  }
  return out;
}

/**
 * The supplied road texture runs horizontally (lane dashes left-to-right, the
 * yellow shoulder lines along the top and bottom). The track scrolls
 * vertically, so bake a 90 degree rotation in once instead of fighting UV
 * rotation at render time.
 */
function rotateRoad(img) {
  const w = img.naturalWidth;
  const h = img.naturalHeight;
  const c = canvasOf(h, w);
  const ctx = c.getContext('2d');
  ctx.translate(h / 2, w / 2);
  ctx.rotate(Math.PI / 2);
  ctx.drawImage(img, -w / 2, -h / 2);
  const t = textureOf(c);
  t.wrapS = THREE.ClampToEdgeWrapping;
  t.wrapT = THREE.RepeatWrapping;
  return { texture: t, width: h, height: w };
}

export async function loadAssets() {
  const [carsImg, roadImg, boomImg] = await Promise.all([
    loadImage(`${BASE}assets/cars.svg`),
    loadImage(`${BASE}assets/road.png`),
    loadImage(`${BASE}assets/explosion.png`),
  ]);

  const carSlices = sliceByGaps(carsImg, 4);
  const cars = carSlices.map((s) => ({
    texture: textureOf(s.canvas),
    aspect: s.width / s.height,
  }));

  return {
    cars,                              // [red, green, yellow, blue]
    road: rotateRoad(roadImg),
    explosion: sliceStrip(boomImg, 3),
  };
}
