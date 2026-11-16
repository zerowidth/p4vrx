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

  function push() {
    const usb = Number(document.querySelector("input[name=usb]:checked").value);
    ui.sim_set_channel($("band").value.charCodeAt(0), $("number").value.charCodeAt(0));
    ui.sim_set($("decoding").checked, $("locked").checked, Number($("level").value), usb, ui.sim_channel_mhz());
    $("level-out").textContent = $("level").value;
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

  let frame = 0;
  function draw() {
    const locked = $("locked").checked && $("decoding").checked;
    if (locked && clip.readyState >= 2) {
      ctx.drawImage(clip, 0, 0, PIC_W, H);
      const kHz = Number($("noise").value);
      ctx.globalAlpha = Math.min(0.85, Math.max(0, (kHz - 100) / 1100));
      ctx.drawImage(noise[frame++ % noise.length], 0, 0);
      ctx.globalAlpha = 1;
    } else {
      ctx.fillStyle = "#1c1d21";
      ctx.fillRect(0, 0, PIC_W, H);
    }
    if (ui.sim_frame()) {
      const px = fb();
      for (let y = 0; y < H; ++y) {
        for (let x = 0; x < STRIP_W; ++x) {
          const p = px[y * W + PIC_W + x], o = (y * STRIP_W + x) * 4;
          strip.data[o] = (p >> 11) << 3;
          strip.data[o + 1] = ((p >> 5) & 63) << 2;
          strip.data[o + 2] = (p & 31) << 3;
          strip.data[o + 3] = 255;
        }
      }
    }
    ctx.putImageData(strip, PIC_W, 0);
    requestAnimationFrame(draw);
  }
  draw();
  clip.play().catch(() => {});
}

// The dev server says when ui/ changed; reload to pick it up.
new EventSource("/events").onmessage = () => location.reload();
start();
