COPYRIGHT = """\
/*
 * Copyright 2021 Collabora Ltd.
 *
 * Derived from tu_extensions.py which is:
 * Copyright 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
"""

import argparse
import os.path
import re
import sys

VULKAN_UTIL = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../vulkan/util'))
sys.path.append(VULKAN_UTIL)

from vk_extensions import *
from vk_extensions_gen import *

MAX_API_VERSION = '1.0.0'

# On Android, we disable all surface and swapchain extensions. Android's Vulkan
# loader implements VK_KHR_surface and VK_KHR_swapchain, and applications
# cannot access the driver's implementation. Moreoever, if the driver exposes
# the those extension strings, then tests dEQP-VK.api.info.instance.extensions
# and dEQP-VK.api.info.device fail due to the duplicated strings.
EXTENSIONS = [
    Extension('VK_KHR_surface',                          25, 'PANVK_HAS_SURFACE'),
    Extension('VK_KHR_swapchain',                        68, 'PANVK_HAS_SURFACE'),
    Extension('VK_KHR_wayland_surface',                   6, 'VK_USE_PLATFORM_WAYLAND_KHR'),
    Extension('VK_EXT_custom_border_color',              12, True),
]

MAX_API_VERSION = VkVersion(MAX_API_VERSION)
API_VERSIONS = [ ApiVersion(MAX_API_VERSION,  True) ]

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-c', help='Output C file.', required=True)
    parser.add_argument('--out-h', help='Output H file.', required=True)
    parser.add_argument('--xml',
                        help='Vulkan API XML file.',
                        required=True,
                        action='append',
                        dest='xml_files')
    args = parser.parse_args()

    gen_extensions('panvk', args.xml_files, API_VERSIONS, MAX_API_VERSION, EXTENSIONS, args.out_c, args.out_h)
