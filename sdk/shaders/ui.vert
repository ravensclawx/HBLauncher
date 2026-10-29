#version 450
// HBLauncher Native UI vertex shader.
// Positions arrive in 1920x1080 pixel space with the origin at the top-left,
// +y down.  Vulkan NDC is also +y down, so the mapping is a plain scale.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in uint in_tex;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_tex;

layout(push_constant) uniform Push {
    vec2 screen;
} push;

void main() {
    vec2 ndc = in_pos / push.screen * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color;
    v_tex = in_tex;
}
