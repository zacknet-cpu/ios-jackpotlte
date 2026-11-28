# tools/mesa-air: where it comes from

`air/` (except `air_jackpotlte.c` / `air_jackpotlte.h`, which are ours) and
`0101-air-frontend.patch` are copied unchanged from Jesvi-iOS
(github.com/jesvi-eulah/Jesvi-iOS, commit 1b184e5bf3d60505d1907e6a11f292b23b1b433c,
`tools/mesa/air/` and `tools/mesa/air-frontend.patch`).
`0102-air-panfrost-jackpotlte.patch` is adapted from its `tools/mesa/air-panfrost.patch`
(the header of that patch says what changed). Their license:

    MIT License

    Copyright (c) 2025-2026 the HakiOS authors

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

The source files in `air/` carry their own "Copyright (c) 2026 Darwin on redwood
contributors" + SPDX MIT headers, kept as they are.

What this is for: P13 route A (research/p13-metal-plan.md in the port's work
folder), phase 1 = build only. Nothing here is installed on the phone by CI.
