#version 450
// HBLauncher Native UI fragment shader.
// One pipeline draws solids, glyphs and app icons.  The vertex `tex` word
// selects the source: 0 solid vertex colour, 1 glyph (R8 alpha), 2 icon
// (RGBA8 array layer).  Blending is straight alpha.
//
// Fragment components are logical R/G/B/A for every attachment format; the
// driver maps them to the format's storage (BGRA8 included), so no swizzle is
// needed here.
layout(set = 0, binding = 0) uniform sampler2D atlas_glyphs;
layout(set = 0, binding = 1) uniform sampler2DArray atlas_icons;
layout(set = 0, binding = 2) uniform sampler2D app_image;   /* hbx_ui image */

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_tex;

layout(location = 0) out vec4 outColor;

void main() {
    uint mode = v_tex & 3u;
    vec3 rgb = v_color.rgb;
    float alpha = v_color.a;
    if (mode == 1u) {
        alpha *= texture(atlas_glyphs, v_uv).r;
    } else if (mode == 2u) {
        float layer = float((v_tex >> 8) & 0xffu);
        vec4 icon = texture(atlas_icons, vec3(v_uv, layer));
        rgb = mix(rgb, icon.rgb, icon.a);
        alpha *= icon.a;
    } else if (mode == 3u) {
        vec4 texel = texture(app_image, v_uv);
        rgb = mix(rgb, texel.rgb, texel.a);
        alpha *= texel.a;
    }
    outColor = vec4(rgb, alpha);
}
