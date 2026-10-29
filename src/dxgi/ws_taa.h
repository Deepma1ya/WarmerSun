// WarmerSun - temporal anti-aliasing and camera motion blur.
//
// The game already runs a TAA pass ("def.taa.compute"). Its pixel shader
// receives the scene colour, the previous frame's colour, the previous frame's
// depth and a packed depth/jitter texture, and it projects the current pixel
// with the *previous* frame's view-projection matrix stored in its own
// constant buffer.
//
// WarmerSun replaces that pixel shader with its own implementation. The
// bindings are untouched, so the engine keeps feeding the same data, and the
// reprojection maths is carried over verbatim - but the history blend, the
// clarity pass and the motion blur are ours and are exposed in the panel.
//
// If anything looks wrong the replacement fails safe: when the depth
// validation cannot confirm the reprojection the history weight drops to zero
// and the frame is simply the current colour.
#pragma once

#include <d3d11.h>
#include <cstdint>

namespace ws {
namespace taa {

// Compiles the replacement shader and creates the tuning buffer.
// Safe to call more than once; returns false if the compiler is unavailable.
bool install(ID3D11Device* device, const wchar_t* log_dir);

bool available();

// Called from the CreatePixelShader hook for every shader the game builds.
void note_shader(ID3D11PixelShader* ps);

// True when `ps` belongs to the engine's TAA family.
bool is_taa(ID3D11PixelShader* ps);

ID3D11PixelShader* replacement();
ID3D11Buffer*      tune_buffer();

void set_enabled(bool on);
bool enabled();

// Uploads the current tuning values. Called on the render thread.
void write_tune(ID3D11DeviceContext* ctx);

// strength     0..1   history weight (how much temporal accumulation)
// clarity      0..1   extra local contrast on the resolved image
// motion_blur  0..4   camera motion blur amount
// mb_samples   2..16  taps used by the motion blur
// motion_range px     velocity at which the history is fully dropped
void set_params(float strength, float clarity, float motion_blur,
                int mb_samples, float motion_range);

void shutdown();

} // namespace taa
} // namespace ws
