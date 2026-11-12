#version 450
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col0;
layout(location = 2) in vec3 a_tex0;
layout(push_constant) uniform Push {
    mat4 projection;
    uvec4 tev[4];
} pc;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
void main() {
    gl_Position = pc.projection * vec4(a_pos, 1.0);
    v_color = a_col0;
    v_uv = a_tex0.xy;
}
