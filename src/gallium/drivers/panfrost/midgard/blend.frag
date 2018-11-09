/* Input source color in gl_Color; backbuffer color in gl_SecondaryColor; constant in uniform */

uniform vec4 constant;

void main() {
	gl_FragColor = gl_Color;
}
