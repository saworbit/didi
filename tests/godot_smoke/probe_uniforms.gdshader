shader_type spatial;

// Deliberately plain types with no colour hint, so the declared Godot type is
// unambiguous. strength is overridden by the material and offset is not, which
// is what makes both sides of the overridden flag testable.
uniform float strength = 0.5;
uniform vec3 offset = vec3(1.0, 2.0, 3.0);

void fragment() {
	ALBEDO = offset * strength;
}
