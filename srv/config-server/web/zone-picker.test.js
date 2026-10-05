// Host test for tools/zone-picker.logic.js (the pure, DOM-free logic behind the
// zone picker). Run: node tools/zone-picker.test.js
const ZP = require('./zone-picker.logic.js');

let pass = 0, fail = 0;
function check(name, cond) {
  if (cond) { console.log('  ok   ' + name); pass++; }
  else { console.log('  FAIL ' + name); fail++; }
}

// Row-major numbering on a scrambled 2x2 grid (frame pixels): tv1 TL, tv2 TR,
// tv3 BL, tv4 BR -- matching roi.cpp's top->bottom, left->right ordering.
const scrambled = [
  { x: 600, y: 400, w: 100, h: 100 }, // BR
  { x: 50,  y: 40,  w: 100, h: 100 }, // TL
  { x: 600, y: 40,  w: 100, h: 100 }, // TR
  { x: 50,  y: 400, w: 100, h: 100 }, // BL
];
const ids = ZP.assignIds(ZP.orderRowMajor(scrambled));
check('tv1 is top-left',     ids[0].x === 50  && ids[0].y === 40  && ids[0].id === 'tv1');
check('tv2 is top-right',    ids[1].x === 600 && ids[1].y === 40  && ids[1].id === 'tv2');
check('tv3 is bottom-left',  ids[2].x === 50  && ids[2].y === 400 && ids[2].id === 'tv3');
check('tv4 is bottom-right', ids[3].x === 600 && ids[3].y === 400 && ids[3].id === 'tv4');

// A single row (slightly jittered y) stays ordered left-to-right.
const row = ZP.assignIds(ZP.orderRowMajor([
  { x: 300, y: 50, w: 80, h: 80 },
  { x: 50,  y: 55, w: 80, h: 80 },
  { x: 600, y: 48, w: 80, h: 80 },
]));
check('single row left-to-right', row.map(b => b.x).join(',') === '50,300,600');

// scaleBox: a box in displayed-canvas coords -> natural-frame pixels.
const sb = ZP.scaleBox({ x: 10, y: 20, w: 30, h: 40 }, 1280 / 640, 720 / 360);
check('scaleBox doubles', sb.x === 20 && sb.y === 40 && sb.w === 60 && sb.h === 80);

// clampToFrame: a box extending past the right edge is flagged and clamped.
const c = ZP.clampToFrame({ x: 1200, y: 50, w: 200, h: 100 }, 1280, 720);
check('out-of-bounds flagged', c.oob === true);
check('clamped to frame width', c.x === 1200 && c.w === 80); // 1280 - 1200
const inb = ZP.clampToFrame({ x: 10, y: 10, w: 100, h: 100 }, 1280, 720);
check('in-bounds not flagged', inb.oob === false && inb.w === 100);

// buildRoiConfig emits the cerberus rect-mode block.
const cfg = ZP.buildRoiConfig([{ x: 10, y: 10, w: 100, h: 100 }]);
check('roi block is rect-mode', cfg.mode === 'rect' && cfg.tvs.length === 1 && cfg.tvs[0].id === 'tv1');

// configToBoxes: pull existing rect zones out of a loaded config for editing.
const loaded = {
  device: { camera_device: 'usb_camera' },
  roi: { mode: 'rect', tvs: [
    { id: 'tv1', x: 50, y: 80, w: 560, h: 620, _comment: 'left' },
    { id: 'tv2', x: 650, y: 80, w: 560, h: 620 },
  ] },
  mqtt: { host: '127.0.0.1' },
};
const pulled = ZP.configToBoxes(loaded);
check('configToBoxes count', pulled.length === 2);
check('configToBoxes coords', pulled[0].x === 50 && pulled[0].w === 560 && pulled[1].x === 650);
check('configToBoxes ignores non-rect', ZP.configToBoxes({ roi: { mode: 'auto' } }).length === 0);
check('configToBoxes handles missing roi', ZP.configToBoxes({}).length === 0);

// mergeRoi: return a NEW config with roi replaced, every other key preserved, input untouched.
const merged = ZP.mergeRoi(loaded, [{ x: 0, y: 0, w: 100, h: 100 }]);
check('mergeRoi replaces roi', merged.roi.mode === 'rect' && merged.roi.tvs.length === 1 && merged.roi.tvs[0].id === 'tv1');
check('mergeRoi preserves other keys', merged.device.camera_device === 'usb_camera' && merged.mqtt.host === '127.0.0.1');
check('mergeRoi does not mutate input', loaded.roi.tvs.length === 2);

console.log(fail === 0 ? 'ALL PASS' : (fail + ' FAILURES'));
process.exit(fail === 0 ? 0 : 1);
