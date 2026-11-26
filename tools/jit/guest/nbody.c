// SPDX-License-Identifier: GPL-2.0-or-later
// N-body simulation in double precision: fadd/fmul/fdiv/fsqrt and fused multiply-adds.
#define REPS 2
#include "rt.h"
#define NB 5
typedef struct { double x, y, z, vx, vy, vz, m; } body;
static body b[NB];
static void init(void) {
  static const double v[NB][7] = {
    { 0, 0, 0, 0, 0, 0, 39.47841760435743 },
    { 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 6.06326392995832e-01, 2.81198684491626e+00, -2.52183616598876e-02, 3.7693674870389486e-02 },
    { 8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -1.01077434617365e+00, 1.82566237123041e+00, 8.41576137658415e-03, 1.1286326131968767e-02 },
    { 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 1.08279100644154e+00, 8.68713018169608e-01, -1.01086306153286e-02, 1.7237240570597112e-03 },
    { 1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 9.79090732243898e-01, 5.94698998647676e-01, -3.47559555040781e-02, 2.0336868699246304e-03 } };
  for (int i = 0; i < NB; i++) { b[i].x = v[i][0]; b[i].y = v[i][1]; b[i].z = v[i][2]; b[i].vx = v[i][3] * 365.24; b[i].vy = v[i][4] * 365.24; b[i].vz = v[i][5] * 365.24; b[i].m = v[i][6]; }
}
static double energy(void) {
  double e = 0;
  for (int i = 0; i < NB; i++) {
    e += 0.5 * b[i].m * (b[i].vx * b[i].vx + b[i].vy * b[i].vy + b[i].vz * b[i].vz);
    for (int j = i + 1; j < NB; j++) {
      double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
      e -= b[i].m * b[j].m / __builtin_sqrt(dx * dx + dy * dy + dz * dz);
    }
  }
  return e;
}
static void step(double dt) {
  for (int i = 0; i < NB; i++)
    for (int j = i + 1; j < NB; j++) {
      double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
      double d2 = dx * dx + dy * dy + dz * dz, mag = dt / (d2 * __builtin_sqrt(d2));
      b[i].vx -= dx * b[j].m * mag; b[i].vy -= dy * b[j].m * mag; b[i].vz -= dz * b[j].m * mag;
      b[j].vx += dx * b[i].m * mag; b[j].vy += dy * b[i].m * mag; b[j].vz += dz * b[i].m * mag;
    }
  for (int i = 0; i < NB; i++) { b[i].x += dt * b[i].vx; b[i].y += dt * b[i].vy; b[i].z += dt * b[i].vz; }
}
uint64_t work(void) {
  init();
  double e0 = energy();
  for (int i = 0; i < 300000; i++) step(0.01);
  double e1 = energy();
  uint64_t u0, u1;
  __builtin_memcpy(&u0, &e0, 8); __builtin_memcpy(&u1, &e1, 8);
  return u0 ^ (u1 << 1);
}
