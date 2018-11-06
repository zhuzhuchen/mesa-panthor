/* Input source color in gl_Color; backbuffer color in gl_SecondaryColor; constant in uniform */

uniform vec4 constant;

void main() {
	vec4 multiply = gl_Color * gl_SecondaryColor;
	vec4 screen = 1.0 - (1.0 - gl_Color) * (1.0 - gl_SecondaryColor);
	gl_FragColor = screen;

	gl_FragColor = constant.a > 0.5 ? screen : multiply;
}
