shader_type spatial;

// Deliberately plain types with no colour hint, so the declared Godot type is
// unambiguous. strength is overridden by the material and offset is not, which
// is what makes both sides of the overridden flag testable.
uniform float strength = 0.5;
uniform vec3 offset = vec3(1.0, 2.0, 3.0);

// A declared range, so the reported hint and the refusal that honours it have
// something to be about.
uniform float bounded : hint_range(0.0, 1.0) = 0.5;
// A colour, because a Color is four float32 channels and its alpha is optional
// in the JSON spelling this server documents. Both are what made a write that
// landed report applied: false.
uniform vec4 tint : source_color = vec4(1.0, 1.0, 1.0, 1.0);

void fragment() {
	ALBEDO = offset * strength * bounded * tint.rgb;
}
