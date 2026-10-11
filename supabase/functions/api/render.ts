// Server-side text rendering for scripts the box can't draw itself.
// LVGL has no complex text shaping, so Devanagari conjuncts and matras come out
// broken on the device. Here HarfBuzz shapes the text, the glyph outlines go into
// an SVG, resvg rasterises it, and the box gets a ready-made RGB565 bitmap.

import * as hb from "npm:harfbuzzjs@1.6.2";
import { initWasm, Resvg } from "npm:@resvg/resvg-wasm@2.6.2";

const CDN = "https://cdn.jsdelivr.net/npm";
const WASM_URL = `${CDN}/@resvg/resvg-wasm@2.6.2/index_bg.wasm`;
// Noto Sans Devanagari also contains Latin, digits and punctuation
const FONT_URL = `${CDN}/@expo-google-fonts/noto-sans-devanagari@0.4.1/400Regular/NotoSansDevanagari_400Regular.ttf`;

type Font = { font: any; upem: number; paths: Map<number, string> };
let ready: Promise<Font> | null = null;

// Loaded once per function instance. Sources can be overridden for local tests.
export function setupRender(wasm?: Uint8Array, fontData?: Uint8Array) {
  ready = (async () => {
    const w = wasm ?? new Uint8Array(await (await fetch(WASM_URL)).arrayBuffer());
    await initWasm(w);
    const data = fontData ?? new Uint8Array(await (await fetch(FONT_URL)).arrayBuffer());
    const face = new hb.Face(new hb.Blob(data.slice().buffer));
    return { font: new hb.Font(face), upem: face.upem, paths: new Map() };
  })();
  return ready;
}

async function getFont(): Promise<Font> {
  if (!ready) setupRender();
  try {
    return await ready!;
  } catch (e) {
    ready = null;   // retry on next call
    throw e;
  }
}

type Glyph = { gid: number; x: number; y: number };

function shape(f: Font, text: string): { glyphs: Glyph[]; width: number } {
  const buf = new hb.Buffer();
  buf.addText(text);
  buf.guessSegmentProperties();
  hb.shape(f.font, buf);
  const infos = buf.getGlyphInfos();
  const pos = buf.getGlyphPositions();
  const glyphs: Glyph[] = [];
  let pen = 0;
  for (let i = 0; i < infos.length; i++) {
    glyphs.push({ gid: infos[i].codepoint, x: pen + pos[i].xOffset, y: pos[i].yOffset });
    pen += pos[i].xAdvance;
  }
  return { glyphs, width: pen };
}

function wrap(f: Font, text: string, size: number, maxW: number): string[] {
  const k = size / f.upem;
  const lines: string[] = [];
  for (const para of text.split(/\n+/)) {
    let line = "";
    for (const w of para.split(/\s+/).filter(Boolean)) {
      const next = line ? `${line} ${w}` : w;
      if (!line || shape(f, next).width * k <= maxW) line = next;
      else {
        lines.push(line);
        line = w;
      }
    }
    if (line) lines.push(line);
  }
  return lines;
}

// Anything beyond Latin Extended / common punctuation needs server rendering
export function needsRender(...texts: (string | null | undefined)[]): boolean {
  return texts.some((t) => t && /[^\u0000-ɏ -⁯₠-⃏]/.test(t));
}

export type Block = { text: string; size: number; color: string };

// Returns [width u16 LE][height u16 LE] followed by RGB565 LE pixels.
export async function renderBlocks(blocks: Block[], width = 300, bg = "#0f1115", maxHeight = 1200) {
  const f = await getFont();
  const pad = 2;
  const used = new Set<number>();
  let body = "";
  let y = pad;
  outer:
  for (const b of blocks) {
    if (!b.text) continue;
    const k = b.size / f.upem;
    const lh = Math.round(b.size * 1.6);
    for (const line of wrap(f, b.text, b.size, width - pad * 2)) {
      if (y + lh > maxHeight) break outer;
      const base = y + Math.round(lh * 0.72);
      const { glyphs } = shape(f, line);
      body += `<g fill="${b.color}">`;
      for (const g of glyphs) {
        used.add(g.gid);
        const gx = (pad + g.x * k).toFixed(2), gy = (base - g.y * k).toFixed(2);
        body += `<use href="#g${g.gid}" transform="translate(${gx} ${gy}) scale(${k.toFixed(5)} ${(-k).toFixed(5)})"/>`;
      }
      body += `</g>`;
      y += lh;
    }
    y += 6;
  }
  let defs = "";
  for (const gid of used) {
    let d = f.paths.get(gid);
    if (d === undefined) {
      d = f.font.glyphToPath(gid) as string;
      f.paths.set(gid, d);
    }
    if (d) defs += `<path id="g${gid}" d="${d}"/>`;
  }
  const height = Math.min(Math.max(y + pad, 20), maxHeight);
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="${width}" height="${height}">` +
    `<defs>${defs}</defs><rect width="100%" height="100%" fill="${bg}"/>${body}</svg>`;
  const r = new Resvg(svg, { font: { loadSystemFonts: false } });
  const img = r.render();
  const rgba = img.pixels;
  const w = img.width, h = img.height;
  img.free();
  r.free();

  const out = new Uint8Array(4 + w * h * 2);
  const dv = new DataView(out.buffer);
  dv.setUint16(0, w, true);
  dv.setUint16(2, h, true);
  for (let i = 0, o = 4; i < w * h; i++, o += 2) {
    const r8 = rgba[i * 4], g8 = rgba[i * 4 + 1], b8 = rgba[i * 4 + 2];
    dv.setUint16(o, ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3), true);
  }
  return out;
}
