#version 450
/* Raw-Vulkan SDK example: one equilateral triangle, no vertex buffer. */
layout(location = 0) out vec3 v_color;
layout(push_constant) uniform Push { vec2 screen; float angle; } push;
void main() {
    float a = push.angle + float(gl_VertexIndex) * 2.0943951;
    vec2 p = vec2(cos(a), sin(a)) * 260.0 + push.screen * 0.5;
    gl_Position = vec4(p / push.screen * 2.0 - 1.0, 0.0, 1.0);
    v_color = vec3(gl_VertexIndex == 0u ? 1.0 : 0.25,
                   gl_VertexIndex == 1u ? 1.0 : 0.30,
                   gl_VertexIndex == 2u ? 1.0 : 0.55);
}
