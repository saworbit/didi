shader_type canvas_item;

uniform float strength : hint_range(0.0, 1.0) = 0.5;
uniform vec4 tint : source_color = vec4(1.0, 1.0, 1.0, 1.0);
uniform int steps = 3;
uniform vec2 offset = vec2(0.0, 0.0);

void fragment() {
	vec4 base = texture(TEXTURE, UV + offset);
	COLOR = mix(base, tint, strength * float(steps) / 3.0);
}
