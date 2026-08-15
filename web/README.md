# IMU Racer — web app

The browser half of the project. See the [root README](../README.md) for the
full picture, including the firmware.

```bash
npm install
npm run dev      # http://localhost:5180
npm run build    # self-contained dist/
npm run preview  # serve the built dist/
```

## Source layout

| File | Role |
| --- | --- |
| `src/main.js` | bootstrap, UI wiring, game loop |
| `src/game.js` | three.js scene, traffic, difficulty, collisions |
| `src/assets.js` | sprite-sheet slicing, road rotation, texture creation |
| `src/ble.js` | Web Bluetooth client |
| `src/input.js` | merges controller, keyboard and pointer into one axis |
| `src/scores.js` | top-eight table in localStorage |

Gameplay constants sit at the top of `src/game.js`. Tilt sensitivity is
`STEER_RANGE_DEG` in `../firmware/main/main.c`.

## Deploying

`base: './'` is set in `vite.config.js`, so `dist/` works from any sub-folder.
Copy its contents wherever you want them:

```bash
npm run build
# then upload dist/* to e.g. public_html/games/imu-racer/
```

Three things that will catch you out:

**Serve over HTTPS.** Web Bluetooth only exists in a secure context. On plain
http `navigator.bluetooth` is undefined — the connect button disables itself and
says so, and the game still plays on keyboard and touch. `localhost` counts as
secure.

**Link with a trailing slash** — `/games/imu-racer/`, not `/games/imu-racer`.
Relative asset URLs resolve against the directory.

**In an iframe the parent must grant permission**, or connecting fails with a
policy error:

```html
<iframe src="/games/imu-racer/" allow="bluetooth" width="405" height="720"></iframe>
```

## Browser support

Chrome, Edge and Opera on desktop and Android. Web Bluetooth is not implemented
in Firefox, Safari or any iOS browser — those visitors get touch controls, which
is why the fallbacks are not optional.

## Protocol

Defined here in `src/ble.js` and mirrored in
`../firmware/main/ble_controller.c`. Change one, change the other. Full spec in
[`../docs/PROTOCOL.md`](../docs/PROTOCOL.md).
