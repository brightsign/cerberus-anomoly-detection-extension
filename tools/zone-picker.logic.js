// Pure, DOM-free logic for the cerberus zone picker. Kept separate from the HTML
// so it can be unit-tested under node (tools/zone-picker.test.js) and reused by
// the browser page (loaded via <script src>). No framework, no dependencies.
//
// Cerberus ROIs are AXIS-ALIGNED rectangles in ABSOLUTE camera-frame pixels, one
// per screen, identified by a string id (tv1..tvN). This module turns the boxes
// a user drags (in displayed-canvas coordinates) into that rect-mode config.
(function (global) {
  'use strict';

  // Scale a box from one coordinate space to another (e.g. displayed canvas px
  // -> natural frame px), rounding to integer pixels.
  function scaleBox(box, scaleX, scaleY) {
    return {
      x: Math.round(box.x * scaleX),
      y: Math.round(box.y * scaleY),
      w: Math.round(box.w * scaleX),
      h: Math.round(box.h * scaleY),
    };
  }

  // Order boxes top->bottom, left->right, grouping into rows so that boxes on the
  // same physical row (similar center-y) are ordered by center-x. Matches the
  // tvN assignment order in src/wvm/roi.cpp. Returns a new array; input untouched.
  function orderRowMajor(boxes) {
    if (boxes.length === 0) return [];
    const heights = boxes.map(b => b.h).slice().sort((a, b) => a - b);
    const medianH = heights[Math.floor(heights.length / 2)] || 1;
    const rowTol = 0.6 * medianH;

    const items = boxes.map(b => ({ b: b, cy: b.y + b.h / 2, cx: b.x + b.w / 2 }));
    items.sort((p, q) => p.cy - q.cy);

    const rows = [];
    for (const it of items) {
      const last = rows.length ? rows[rows.length - 1] : null;
      if (last && Math.abs(it.cy - last.cyRef) <= rowTol) {
        last.items.push(it);
      } else {
        rows.push({ cyRef: it.cy, items: [it] });
      }
    }

    const ordered = [];
    for (const row of rows) {
      row.items.sort((p, q) => p.cx - q.cx);
      for (const it of row.items) ordered.push(it.b);
    }
    return ordered;
  }

  // Assign tv1..tvN ids to an already-ordered list. Returns new objects carrying
  // id + x/y/w/h (other fields dropped).
  function assignIds(orderedBoxes) {
    return orderedBoxes.map((b, i) => ({
      id: 'tv' + (i + 1),
      x: b.x, y: b.y, w: b.w, h: b.h,
    }));
  }

  // Clamp a box to the frame and report whether it originally extended outside.
  // The engine's crop step silently drops out-of-bounds ROIs, so the UI warns.
  function clampToFrame(box, frameW, frameH) {
    const clampVal = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
    const x = clampVal(box.x, 0, frameW);
    const y = clampVal(box.y, 0, frameH);
    const w = clampVal(box.w, 0, frameW - x);
    const h = clampVal(box.h, 0, frameH - y);
    const oob = box.x < 0 || box.y < 0 ||
                (box.x + box.w) > frameW || (box.y + box.h) > frameH;
    return { x: x, y: y, w: w, h: h, oob: oob };
  }

  // Build the cerberus rect-mode roi block from frame-pixel boxes.
  function buildRoiConfig(frameBoxes) {
    return { mode: 'rect', tvs: assignIds(orderRowMajor(frameBoxes)) };
  }

  const api = { scaleBox, orderRowMajor, assignIds, clampToFrame, buildRoiConfig };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else global.ZonePicker = api;
})(typeof window !== 'undefined' ? window : this);
