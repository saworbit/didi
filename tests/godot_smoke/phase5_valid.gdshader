shader_type spatial;

uniform vec3 tint : source_color = vec3(0.25, 0.5, 0.75);

void fragment() {
	ALBEDO = tint;
}
