# Third-party notices

## VitaDeploy package-head template

`src/promote_head_template.c` contains the package-head template distributed by
[SKGleba/VitaDeploy](https://github.com/SKGleba/VitaDeploy).

The MIT License (MIT)

Copyright (c) 2021 skgleba

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## SHA-1 implementation

`src/sha1.c` and `src/sha1.h` are based on Brad Conte's SHA-1 implementation
from [B-Con/crypto-algorithms](https://github.com/B-Con/crypto-algorithms).
The upstream author released that implementation into the public domain,
without restrictions or warranty.

## zlib

The user module links [zlib](https://zlib.net/) from VitaSDK to compress
screenshots as PNG. zlib is distributed under the zlib licence:

> (C) 1995-2024 Jean-loup Gailly and Mark Adler
>
> This software is provided 'as-is', without any express or implied
> warranty. In no event will the authors be held liable for any damages
> arising from the use of this software.
>
> Permission is granted to anyone to use this software for any purpose,
> including commercial applications, and to alter it and redistribute it
> freely, subject to the following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not
>    claim that you wrote the original software. If you use this software
>    in a product, an acknowledgment in the product documentation would be
>    appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be
>    misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

## VitaStream and vita-udcd-uvc

The displayed-frame capture in `kernel/frame_capture.c` (frame lookup,
pixel-format mapping, IFTU plane setup and buffer allocation) is adapted from
[VitaStream](https://github.com/devnoname120/vitastream) by devnoname120 and
[vita-udcd-uvc](https://github.com/xerpi/vita_udcd_uvc) by xerpi. Neither
repository contains a licence file; the code is reused on the understanding
that it is available under the MIT licence, which is still to be confirmed
with the authors.
