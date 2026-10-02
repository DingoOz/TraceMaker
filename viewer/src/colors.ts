// Colour theme: KiCad-familiar layer colours, tuned for a near-black navy background.
export type RGBA = [number, number, number, number];

const hex = (h: string, a = 1): RGBA => {
  const v = parseInt(h.slice(1), 16);
  return [((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255, a];
};

// KiCad defaults (F.Cu, In1…In30 cycle, B.Cu), slightly brightened for the dark theme.
const INNER = ['#7fc87f', '#ce7d2c', '#4fcbcb', '#db628b', '#a7a5c6', '#28cce0', '#e8b2a7', '#f2eda1', '#8ce099', '#c27ad6'];

export function layerColor(index: number, count: number): RGBA {
  if (index === 0) return hex('#d63c3c');
  if (index === count - 1) return hex('#4f86d6');
  return hex(INNER[(index - 1) % INNER.length]);
}

export function css(c: RGBA): string {
  return `rgb(${Math.round(c[0] * 255)} ${Math.round(c[1] * 255)} ${Math.round(c[2] * 255)})`;
}

export function lighten(c: RGBA, t: number): RGBA {
  return [c[0] + (1 - c[0]) * t, c[1] + (1 - c[1]) * t, c[2] + (1 - c[2]) * t, c[3]];
}

export const theme = {
  background: hex('#060a14'),
  substrate: hex('#0d1424'),
  outline: hex('#d9cf8a', 0.95),
  footprint: hex('#8ea3cc', 0.16),
  padThrough: hex('#d4ad3f'),
  via: hex('#c4c9d4'),
  hole: hex('#05070d'),
  ratsnest: hex('#c9d6f0', 0.5),
  frontier: hex('#4fe3ff'),
  pathTry: hex('#ffd36b'),
  failure: hex('#ff4d5e'),
  highlight: hex('#ffffff'),
};
