shader_type canvas_item;

uniform float strength : hint_range(0.0, 1.0) = 0.5;
uniform vec4 tint_color : source_color = vec4(1.0, 0.5, 0.25, 1.0);

void fragment() {
	COLOR = mix(COLOR, tint_color, strength);
}
