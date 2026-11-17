// Runs ui/ (compiled to ui.wasm) against fake receiver state. Never touches a board or serial port.
const W = 800, H = 480, PIC_W = 630, STRIP_W = W - PIC_W;
const $ = (id) => document.getElementById(id);
const panel = $("panel"), ctx = panel.getContext("2d"), clip = $("clip");
const strip = ctx.createImageData(STRIP_W, H);
const log = [];

function say(line) {
  log.unshift(line);
  $("log").textContent = log.slice(0, 4).join("\n");
}

// A few frames of grey noise, laid over the picture as the sync noise rises.
const noise = Array.from({ length: 4 }, () => {
  const c = new OffscreenCanvas(PIC_W, H), n = c.getContext("2d"), img = n.createImageData(PIC_W, H);
  for (let i = 0; i < img.data.length; i += 4) {
    const v = Math.random() * 255;
    img.data[i] = img.data[i + 1] = img.data[i + 2] = v;
    img.data[i + 3] = 255;
  }
  n.putImageData(img, 0, 0);
  return c;
});

async function start() {
  const { instance } = await WebAssembly.instantiateStreaming(fetch("ui.wasm"));
  const ui = instance.exports;
  const fb = () => new Uint16Array(ui.memory.buffer, ui.sim_fb(), W * H);
  // The pause sign as the P4 blends it: the halo darkens to 85%, then a light grey fill.
  const pause = (() => {
    const w = ui.sim_pause_w(), h = ui.sim_pause_h();
    const fill = new Uint8Array(ui.memory.buffer, ui.sim_pause_fill(), w * h);
    const halo = new Uint8Array(ui.memory.buffer, ui.sim_pause_halo(), w * h);
    const img = new ImageData(w, h), c = document.createElement("canvas");
    for (let i = 0; i < w * h; ++i) {
      const f = fill[i] / 255, a = halo[i] / 255 * 0.85, total = f + a * (1 - f);
      img.data.set([240, 240, 240, Math.round(total * 255)].map((v, k) => (k < 3 ? (total ? v * f / total : 0) : v)), i * 4);
    }
    c.width = w, c.height = h;
    c.getContext("2d").putImageData(img, 0, 0);
    return c;
  })();

  function push() {
    const usb = Number(document.querySelector("input[name=usb]:checked").value);
    ui.sim_set_channel($("band").value.charCodeAt(0), $("number").value.charCodeAt(0));
    ui.sim_set($("decoding").checked, $("locked").checked, Number($("level").value), usb, ui.sim_channel_mhz(), $("hdzero").checked);
    ui.sim_set_status($("console").checked, $("touch-off").checked, false, false);
    ui.sim_set_strip_left($("strip-left").checked);
    ui.sim_set_hdzero(Number($("hdz-format").value), Number($("lq").value));
    $("level-out").textContent = $("level").value;
    $("lq-out").textContent = $("lq").value;
    $("noise-out").textContent = $("noise").value;
  }
  $("controls").addEventListener("input", push);
  push();

  let down = false;
  function touch(e, isDown) {
    const r = panel.getBoundingClientRect();
    const x = Math.round((e.clientX - r.left) * W / r.width), y = Math.round((e.clientY - r.top) * H / r.height);
    const action = ui.sim_touch(x, y, isDown);
    if (action) {
      const channel = String.fromCharCode(action >> 8, action & 255);
      $("band").value = channel[0];
      $("number").value = channel[1];
      push();
      say(`tap: tune ${channel}`);
    }
  }
  panel.addEventListener("pointerdown", (e) => { down = true; panel.setPointerCapture(e.pointerId); touch(e, true); });
  panel.addEventListener("pointermove", (e) => { if (down) touch(e, true); });
  panel.addEventListener("pointerup", (e) => { down = false; touch(e, false); });

  const full = ctx.createImageData(W, H);
  function copy(img, x0, w) {
    const px = fb();
    for (let y = 0; y < H; ++y) {
      for (let x = 0; x < w; ++x) {
        const p = px[y * W + x0 + x], o = (y * w + x) * 4;
        img.data[o] = (p >> 11) << 3;
        img.data[o + 1] = ((p >> 5) & 63) << 2;
        img.data[o + 2] = (p & 31) << 3;
        img.data[o + 3] = 255;
      }
    }
  }

  // The level wanders a few dB about the slider, as a real one does, so the history has something to show.
  let wander = 0;
  setInterval(() => {
    wander = Math.max(-8, Math.min(8, wander + (Math.random() - 0.5) * 3));
    const usb = Number(document.querySelector("input[name=usb]:checked").value);
    ui.sim_set($("decoding").checked, $("locked").checked, Number($("level").value) + Math.round(wander), usb,
               ui.sim_channel_mhz(), $("hdzero").checked);
  }, 100);

  // While a menu scans, fake each channel's level: the tuned channel at the slider's level, a neighbour a few dB
  // under it, the rest noise around 22 dB; unscanned channels go back to unread.
  const BANDS = "RABEFL";
  setInterval(() => {
    const scan = ui.sim_scan();
    const tuned = BANDS.indexOf($("band").value) * 8 + Number($("number").value) - 1;
    for (let i = 0; i < 48; ++i) {
      const band = Math.floor(i / 8);
      if (!scan || (scan !== 7 && band !== scan - 1)) { ui.sim_set_level(i, -128); continue; }
      const base = i === tuned ? Number($("level").value) + 8 : Math.abs(i - tuned) === 1 ? 30 : 22;
      ui.sim_set_level(i, Math.round(base + (Math.random() - 0.5) * 4));
    }
  }, 120);

  let frame = 0;
  function draw(now) {
    const redrawn = ui.sim_frame(now || 0);
    if (ui.sim_full()) {
      if (redrawn) copy(full, 0, W);
      ctx.putImageData(full, 0, 0);
      requestAnimationFrame(draw);
      return;
    }
    const px0 = ui.sim_picture_x(), sx = ui.sim_strip_x();
    // Decoding pauses while a menu scans, so the picture holds its last frame.
    if (ui.sim_scan()) clip.pause();
    else if (clip.paused) clip.play().catch(() => {});
    const locked = $("locked").checked && $("decoding").checked;
    if (locked && clip.readyState >= 2 && $("hdzero").checked) {
      // HDZero's 16:9 across the picture's width, letterboxed. No HDZero footage is decoded yet, so this is the
      // analog clip cropped to 16:9; a digital link shows no analog noise.
      const h = Math.round(PIC_W * 9 / 16), y = (H - h) / 2;
      ctx.fillStyle = "#000";
      ctx.fillRect(px0, 0, PIC_W, H);
      const sh = clip.videoWidth * 9 / 16;
      ctx.drawImage(clip, 0, (clip.videoHeight - sh) / 2, clip.videoWidth, sh, px0, y, PIC_W, h);
      // The stream's format in the bottom letterbox's right corner; the placeholder note on the left.
      ctx.fillStyle = "#8a8d96";
      ctx.font = "16px system-ui";
      ctx.textAlign = "right";
      ctx.fillText($("hdz-format").selectedOptions[0].text, px0 + PIC_W - 12, H - (H - y - h) / 2 + 6);
      ctx.textAlign = "left";
      ctx.font = "12px system-ui";
      ctx.fillText("placeholder: analog clip cropped to 16:9", px0 + 12, H - (H - y - h) / 2 + 4);
    } else if (locked && clip.readyState >= 2) {
      ctx.drawImage(clip, px0, 0, PIC_W, H);
      const kHz = Number($("noise").value);
      ctx.globalAlpha = Math.min(0.85, Math.max(0, (kHz - 100) / 1100));
      ctx.drawImage(noise[frame++ % noise.length], px0, 0);
      ctx.globalAlpha = 1;
    } else {
      ctx.fillStyle = "#1c1d21";
      ctx.fillRect(px0, 0, PIC_W, H);
    }
    if (ui.sim_scan()) ctx.drawImage(pause, px0 + (PIC_W - pause.width) / 2, (H - pause.height) / 2);
    if (redrawn) copy(strip, sx, STRIP_W);
    ctx.putImageData(strip, sx, 0);
    requestAnimationFrame(draw);
  }
  draw();
  clip.play().catch(() => {});
}

// The dev server says when ui/ changed; reload to pick it up.
new EventSource("/events").onmessage = () => location.reload();
start();
