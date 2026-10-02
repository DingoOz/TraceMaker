// GLSL ES 3.0 shaders. World units are millimetres; u_view = (centre x, centre y, clip-per-mm x, clip-per-mm y),
// u_px = millimetres per device pixel (for signed-distance anti-aliasing).

const VIEW = /* glsl */ `
uniform vec4 u_view;
uniform float u_px;
vec4 toClip(vec2 p) { return vec4((p.x - u_view.x) * u_view.z, -(p.y - u_view.y) * u_view.w, 0.0, 1.0); }
`;

const SEG = /* glsl */ `
float sdSeg(vec2 p, vec2 a, vec2 b) {
  vec2 pa = p - a, ba = b - a;
  float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
  return length(pa - ba * h);
}
`;

const COLOR = /* glsl */ `
uniform vec4 u_color;
uniform vec4 u_hlColor;
uniform float u_hl;
vec4 pick(float net) { return (u_hl > 0.0 && abs(net - u_hl) < 0.5) ? u_hlColor : u_color; }
`;

/** Instanced capsules: tracks, round and oval pads, outline and ratsnest lines (radius 0 + minimum width). */
export const capsuleVS = /* glsl */ `#version 300 es
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 i_ab;
layout(location = 2) in vec2 i_rn;
${VIEW}
uniform float u_minPx;
out vec2 v_p;
flat out vec4 v_ab;
flat out float v_r;
flat out float v_thin;
flat out float v_net;
void main() {
  vec2 a = i_ab.xy, b = i_ab.zw;
  float r = max(i_rn.x, u_minPx * u_px);
  v_thin = i_rn.x > 0.0 ? clamp(i_rn.x / r, 0.35, 1.0) : 1.0;
  float m = r + 1.5 * u_px;
  vec2 d = b - a;
  float len = length(d);
  vec2 dir = len > 1e-9 ? d / len : vec2(1.0, 0.0);
  vec2 nrm = vec2(-dir.y, dir.x);
  vec2 p = (a + b) * 0.5 + dir * a_corner.x * (len * 0.5 + m) + nrm * a_corner.y * m;
  v_p = p; v_ab = i_ab; v_r = r; v_net = i_rn.y;
  gl_Position = toClip(p);
}`;

export const capsuleFS = /* glsl */ `#version 300 es
precision highp float;
in vec2 v_p;
flat in vec4 v_ab;
flat in float v_r;
flat in float v_thin;
flat in float v_net;
uniform float u_px;
${COLOR}
${SEG}
out vec4 o;
void main() {
  float d = sdSeg(v_p, v_ab.xy, v_ab.zw) - v_r;
  float cov = clamp(0.5 - d / u_px, 0.0, 1.0);
  if (cov <= 0.0) discard;
  vec4 c = pick(v_net);
  float a = c.a * cov * v_thin;
  o = vec4(c.rgb * a, a);
}`;

/** Instanced rings: vias (annulus with drill hole). */
export const ringVS = /* glsl */ `#version 300 es
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 i_c; // x, y, outer r, hole r
layout(location = 2) in float i_net;
${VIEW}
out vec2 v_p;
flat out vec4 v_c;
flat out float v_net;
void main() {
  float r = max(i_c.z, 1.2 * u_px) + 1.5 * u_px;
  v_p = i_c.xy + a_corner * r;
  v_c = i_c; v_net = i_net;
  gl_Position = toClip(v_p);
}`;

export const ringFS = /* glsl */ `#version 300 es
precision highp float;
in vec2 v_p;
flat in vec4 v_c;
flat in float v_net;
uniform float u_px;
uniform vec4 u_holeColor;
${COLOR}
out vec4 o;
void main() {
  float d = length(v_p - v_c.xy);
  float ro = max(v_c.z, 1.2 * u_px);
  float outer = clamp(0.5 - (d - ro) / u_px, 0.0, 1.0);
  if (outer <= 0.0) discard;
  float hole = v_c.w > 0.6 * u_px ? clamp(0.5 - (d - v_c.w) / u_px, 0.0, 1.0) : 0.0;
  vec4 c = pick(v_net);
  // A faint radial sheen keeps vias readable as rings rather than flat discs.
  float sheen = 0.85 + 0.15 * smoothstep(v_c.w, ro, d);
  vec3 rgb = mix(c.rgb * sheen, u_holeColor.rgb, hole);
  o = vec4(rgb * outer, outer);
}`;

/** Plain triangles: polygon pads, stencil fans for zones and the board outline, cover quads. */
export const triVS = /* glsl */ `#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in float a_net;
${VIEW}
flat out float v_net;
void main() { v_net = a_net; gl_Position = toClip(a_pos); }`;

export const triFS = /* glsl */ `#version 300 es
precision highp float;
flat in float v_net;
${COLOR}
out vec4 o;
void main() { vec4 c = pick(v_net); o = vec4(c.rgb * c.a, c.a); }`;

/** Full-screen composite of an offscreen layer with an opacity. */
export const compositeVS = /* glsl */ `#version 300 es
layout(location = 0) in vec2 a_corner;
out vec2 v_uv;
void main() { v_uv = a_corner * 0.5 + 0.5; gl_Position = vec4(a_corner, 0.0, 1.0); }`;

export const compositeFS = /* glsl */ `#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_alpha;
out vec4 o;
void main() { o = texture(u_tex, v_uv) * u_alpha; }`;

/**
 * Additive glow effects. Instance data:
 *  i_ab = segment endpoints (a == b for dots and rings)
 *  i_p  = (radius mm, minimum radius px, glow sigma px, start time s)
 *  i_q  = (life s, kind, dash length px, unused)   kind: 0 glow capsule, 1 pulsing ring, 2 dashed line
 *  i_c  = (rgb, intensity)
 */
export const fxVS = /* glsl */ `#version 300 es
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 i_ab;
layout(location = 2) in vec4 i_p;
layout(location = 3) in vec4 i_q;
layout(location = 4) in vec4 i_c;
${VIEW}
uniform float u_time;
out vec2 v_p;
flat out vec4 v_ab;
flat out vec4 v_p2;
flat out vec4 v_q;
flat out vec4 v_c;
void main() {
  vec2 a = i_ab.xy, b = i_ab.zw;
  float r = max(i_p.x, i_p.y * u_px);
  float sigma = i_p.z * u_px;
  float m = (i_q.y == 1.0 ? r * 1.35 : r) + 3.0 * sigma + u_px;
  vec2 d = b - a;
  float len = length(d);
  vec2 dir = len > 1e-9 ? d / len : vec2(1.0, 0.0);
  vec2 nrm = vec2(-dir.y, dir.x);
  vec2 p = (a + b) * 0.5 + dir * a_corner.x * (len * 0.5 + m) + nrm * a_corner.y * m;
  v_p = p; v_ab = i_ab; v_p2 = vec4(r, sigma, i_p.w, 0.0); v_q = i_q; v_c = i_c;
  float age = u_time - i_p.w;
  gl_Position = (age < 0.0 || age > i_q.x) ? vec4(2.0, 2.0, 2.0, 1.0) : toClip(p);
}`;

export const fxFS = /* glsl */ `#version 300 es
precision highp float;
in vec2 v_p;
flat in vec4 v_ab;
flat in vec4 v_p2;
flat in vec4 v_q;
flat in vec4 v_c;
uniform float u_px;
uniform float u_time;
${SEG}
out vec4 o;
void main() {
  float age = u_time - v_p2.z;
  float life = v_q.x;
  float f = clamp(1.0 - age / life, 0.0, 1.0);
  f = f * f * (3.0 - 2.0 * f);
  float r = v_p2.x, sigma = max(v_p2.y, 1e-6);
  float k = v_q.y;
  float I;
  if (k == 1.0) {
    // Pulsing ring around a: radius breathes, brightness pulses.
    float pulse = 0.5 + 0.5 * sin(age * 7.0);
    float R = r * (1.0 + 0.22 * pulse);
    float d = abs(length(v_p - v_ab.xy) - R);
    I = exp(-(d * d) / (sigma * sigma)) * (0.65 + 0.35 * pulse) + 0.35 * exp(-(d * d) / (9.0 * sigma * sigma));
  } else {
    float d = max(sdSeg(v_p, v_ab.xy, v_ab.zw) - r, 0.0);
    I = exp(-(d * d) / (sigma * sigma));
    if (k == 2.0) {
      vec2 ba = v_ab.zw - v_ab.xy;
      float s = dot(v_p - v_ab.xy, ba) / max(length(ba), 1e-9) / u_px;
      float dash = step(0.45, fract(s / v_q.z - age * 1.5));
      I *= dash;
    }
  }
  vec3 rgb = v_c.rgb * I * v_c.a * f;
  if (max(rgb.r, max(rgb.g, rgb.b)) < 0.002) discard;
  o = vec4(rgb, 0.0);
}`;
