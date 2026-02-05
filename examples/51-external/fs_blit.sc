$input v_texcoord0

/*
 * Copyright 2026 Daniele Bartolini. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

#include "../common/common.sh"

SAMPLER2D(s_color, 0);

void main()
{
	vec2 texCoord = v_texcoord0;
	vec4 color = texture2D(s_color, texCoord);
	gl_FragColor = color;
}

