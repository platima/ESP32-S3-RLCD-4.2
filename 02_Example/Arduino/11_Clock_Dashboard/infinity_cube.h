#pragma once

// An infinity-mirror cube: a wireframe cube that turns, with smaller copies of itself falling away inside it
// for ever.  Each copy is the one outside it, scaled down about the centre; they all drift inwards, and as
// the outermost copy leaves the frame of the cube the next one peels off it, so the picture never ends and
// never jumps.  The spokes from the corners to the centre are the lines along which the corners recede.
//
// Pure geometry (no drawing, no Arduino): frame() gives the line segments of the picture at a time, for a
// screen of a given size.  ui.cpp draws them, and tools/tests and the UI preview run the same code on a PC.

#include <math.h>
#include <stdint.h>

namespace infcube {

struct Line {
  int16_t x0, y0, x1, y1;
};

const int kCopies = 10;                          // copies inside the cube; the smallest are a few pixels across
const int kMaxLines = 12 * (kCopies + 1) + 8;    // the cube, its copies, and the eight spokes
const float kShrink = 0.66f;                     // each copy against the one outside it
const uint32_t kStepMs = 1700;                   // the time a copy takes to shrink into the place of the next
const float kCamera = 6.0f;                      // the eye's distance from the centre, in half edges of the cube
const float kFill = 0.90f;                       // how much of the screen's half height the cube's corners may reach
const float kMinHalfPx = 3.0f;                   // a copy smaller than this (half an edge, in pixels) is left out

// How far the copies have drifted at `ms`: 0 = each sits where it started, 1 = each has reached the next one's place.
inline float drift(uint32_t ms) { return (float)(ms % kStepMs) / (float)kStepMs; }

// The scale of copy `j` (0 = the outermost) at that drift, against the cube itself.
inline float copyScale(int j, float d) { return powf(kShrink, (float)j + d); }

// The lines of the picture `ms` after it began, for a screen of w x h pixels, into `out` (room for `cap`).
// Returns how many: the cube's 12 edges first, then the 8 spokes, then 12 for every copy that is big enough.
inline int frame(uint32_t ms, int w, int h, Line *out, int cap) {
  // the cube turns steadily about the upright axis and nods a little
  const float t = (float)(ms % 3600000u);  // (an hour of milliseconds keeps its precision in a float)
  const float yaw = t * 0.00052f, pitch = 0.40f * sinf(t * 0.00037f);
  const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
  // the eight corners of a cube with half edge 1, turned, as seen from kCamera away.  A corner is sqrt(3) from the
  // centre, and wherever the cube has turned to it shows no further from the middle of the screen than
  // focal * sqrt(3) / sqrt(kCamera^2 - 3): the focal length makes that kFill of half the height.
  const float focal = kFill * 0.5f * (float)h * sqrtf(kCamera * kCamera - 3.0f) / 1.7320508f;
  float cx[8], cyy[8], cz[8];
  for (int i = 0; i < 8; i++) {
    const float x = (i & 1) ? 1.0f : -1.0f, y = (i & 2) ? 1.0f : -1.0f, z = (i & 4) ? 1.0f : -1.0f;
    const float x1 = x * cy + z * sy, z1 = -x * sy + z * cy;  // about the upright axis
    const float y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;  // the nod
    cx[i] = x1;
    cyy[i] = y2;
    cz[i] = z2;
  }
  const float midX = 0.5f * (float)(w - 1), midY = 0.5f * (float)(h - 1);
  auto put = [&](int corner, float scale, int16_t *px, int16_t *py) {
    const float k = focal / (kCamera + cz[corner] * scale);
    *px = (int16_t)lroundf(midX + cx[corner] * scale * k);
    *py = (int16_t)lroundf(midY - cyy[corner] * scale * k);
  };
  // the 12 edges join the corners that differ in one coordinate
  static const uint8_t kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  int n = 0;
  auto cube = [&](float scale) {
    int16_t px[8], py[8];
    for (int i = 0; i < 8; i++) put(i, scale, &px[i], &py[i]);
    for (int e = 0; e < 12 && n < cap; e++) out[n++] = {px[kEdge[e][0]], py[kEdge[e][0]], px[kEdge[e][1]], py[kEdge[e][1]]};
  };
  cube(1.0f);
  const float d = drift(ms);
  // the spokes: from each corner in to where the copies have become too small to draw
  float inner = 1.0f;
  for (int j = 0; j < kCopies; j++) {
    const float s = copyScale(j, d);
    if (s * focal / kCamera < kMinHalfPx) break;
    inner = s;
  }
  for (int i = 0; i < 8 && n < cap; i++) {
    Line l;
    put(i, 1.0f, &l.x0, &l.y0);
    put(i, inner, &l.x1, &l.y1);
    out[n++] = l;
  }
  for (int j = 0; j < kCopies; j++) {
    const float s = copyScale(j, d);
    if (s * focal / kCamera < kMinHalfPx) break;
    cube(s);
  }
  return n;
}

}  // namespace infcube
