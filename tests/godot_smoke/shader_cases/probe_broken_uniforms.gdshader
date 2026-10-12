shader_type canvas_item;

// Does not compile: the name in fragment() is never declared. A node using
// it is refused shader_not_compiled by shader_list_uniforms (#1343).
uniform float glow = 1.0;

void fragment() {
	COLOR = vec4(didi_undeclared_glow);
}
