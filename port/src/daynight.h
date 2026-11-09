// Day and night: what the renderer needs to relight the game's materials for
// the current hour (port/src/daynight.cpp computes it; the cascades' sun is
// moved there too). Everything is relative to the game's own sun, so at the
// anchor hour (where our sun path meets the game's sun) the factors are 1 and
// the rotation is identity: the picture is the original.
#pragma once

namespace superman_returns::daynight {

struct LightingFrame {
  bool active = false;
  // gLightPos (a far point): keep its distance, add `azimuth_delta` (radians)
  // to its azimuth and multiply its elevation by `elevation_ratio` (the true
  // sun elevation over the game's shadow-sun elevation; negative at night).
  double azimuth_delta = 0.0;
  double elevation_ratio = 1.0;
  float key[3] = {1, 1, 1};      // key light colour factor (sun / moon)
  float ambient[3] = {1, 1, 1};  // fill, mid and shadow colour factor (sky light)
  float fog[3] = {1, 1, 1};      // fog and sky colour factor
  float sun_visibility = 1.0f;   // the sun's flare: 0 below the horizon
  float exposure = 1.0f;         // the scene's darkening (holds the automatic exposure back)
  float night = 0.0f;            // 0 by day, 1 at night (dusk: sun from 6 degrees above to 6 below)
  float windows = 0.0f;          // the city's lit windows: 0 by day, sr_night_windows at night
};

// The current frame's lighting (false: the game's own sun, nothing to change).
bool CurrentLighting(LightingFrame& out);

// The game's light position moved to the hour of `frame` (xyz of a float4).
void MoveSunPosition(const LightingFrame& frame, float xyz[3]);

}  // namespace superman_returns::daynight
