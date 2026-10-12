shader_type canvas_item;

// Its one uniform is turned off by the preprocessor, so the engine lists
// none, and the code compiles (#1345).
#ifdef DIDI_GLOW
uniform float glow = 1.0;
#endif

void fragment() {
	COLOR = vec4(1.0);
}
