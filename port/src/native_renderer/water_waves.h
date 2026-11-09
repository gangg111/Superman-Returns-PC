// Ocean waves: the CPU copy of the wave model the water shaders evaluate
// (shader_common.h, srWaterGerstner / srWaterNormal, patch 0032). The tables
// must stay identical to the HLSL ones: physics and rendering have to agree
// on where the surface is. A sum of Gerstner waves over the world xz plane,
// time in seconds (the game's gTime), heights in world units around the
// game's water level.
#pragma once

#include <cmath>

namespace superman_returns::water {

constexpr int kWaves = 12;
// dir.x, dir.z, k = 2 pi / wavelength, amplitude
constexpr float kWaveA[kWaves][4] = {
    {0.825336f, 0.564642f, 0.130900f, 0.576000f},
    {0.453596f, 0.891207f, 0.181805f, 0.351902f},
    {0.980067f, 0.198669f, 0.252507f, 0.214991f},
    {0.070737f, 0.997495f, 0.350704f, 0.131347f},
    {0.980067f, -0.198669f, 0.487089f, 0.080245f},
    {0.659983f, 0.751280f, 0.676513f, 0.049025f},
    {0.877583f, -0.479426f, 0.939601f, 0.029951f},
    {-0.323290f, 0.946300f, 1.305002f, 0.018298f},
    {0.900447f, 0.434966f, 1.812503f, 0.011179f},
    {0.267499f, 0.963558f, 2.517365f, 0.006830f},
    {1.000000f, 0.000000f, 3.496340f, 0.004173f},
    {-0.029200f, 0.999574f, 4.856028f, 0.002549f}};
// omega = sqrt(9.81 k), phase
constexpr float kWaveB[kWaves][2] = {
    {1.133193f, 0.0f},  {1.335481f, 1.7f},  {1.573879f, 3.4f},  {1.854834f, 5.1f},
    {2.185943f, 6.8f},  {2.576158f, 8.5f},  {3.036032f, 10.2f}, {3.577998f, 11.9f},
    {4.216711f, 13.6f}, {4.969442f, 15.3f}, {5.856543f, 17.0f}, {6.902002f, 18.7f}};
constexpr float kSteepness = 0.9f;

// Displacement (dx, dy, dz) of the surface point resting at (x, z); no distance
// fade (the shaders fade waves the mesh or the pixels cannot carry).
inline void Gerstner(float x, float z, float t, float scale, float out[3]) {
  const float q = kSteepness / std::fmax(1.0f, scale * 0.440f);
  out[0] = out[1] = out[2] = 0.0f;
  for (int i = 0; i < kWaves; ++i) {
    const float* w = kWaveA[i];
    const float a = w[3] * scale;
    const float ph = w[2] * (w[0] * x + w[1] * z) - kWaveB[i][0] * t + kWaveB[i][1];
    const float s = std::sin(ph), c = std::cos(ph);
    out[0] += q * a * w[0] * c;
    out[2] += q * a * w[1] * c;
    out[1] += a * s;
  }
}

// Height offset of the surface above the world point (x, z): the Gerstner
// surface moves points sideways, so find the resting point whose displaced
// position lands on (x, z) (fixed-point iteration; the slopes stay well under
// one, it converges in a few steps).
inline float HeightAt(float x, float z, float t, float scale) {
  float rx = x, rz = z, d[3] = {};
  for (int it = 0; it < 4; ++it) {
    Gerstner(rx, rz, t, scale, d);
    rx = x - d[0];
    rz = z - d[2];
  }
  Gerstner(rx, rz, t, scale, d);
  return d[1];
}

}  // namespace superman_returns::water
