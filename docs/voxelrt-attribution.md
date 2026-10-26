# VoxelRT attribution

The Tree64 shadow backend was designed with reference to the Tree64 acceleration
structure and fast traversal documented at pinned revision
[`e605812`](https://github.com/dubiousconst282/VoxelRT/tree/e605812840a9f6abbadf84a436f8602e2972ee43)
of dubiousconst282/VoxelRT. VoxelRT describes a sparse voxel tree with a
4-by-4-by-4 branching factor, 12-byte nodes, per-chunk rebuilds, bitmask-based
empty-region skipping, and a compact ancestor stack. The Engine implementation
adds its own toroidal camera directory, generation-safe GPU publication,
bounded scene builds, streaming invalidation, transformed-instance handling,
renderer bindings, and half-open boundary semantics. Its first optimized
walker adopts the current-child/empty-region marching shape while re-descending
the shallow tree instead of copying VoxelRT's traversal kernel. A later
experimental specialization independently implements negative-octant
mirroring, a compact workgroup ancestor stack, hierarchy ascent, and
2-by-2-by-2 mask skips. Local benchmarks retain only the mask skip in the
default path; the complete specialization remains an explicit diagnostic
variant.

VoxelRT is distributed under the MIT License:

> Copyright (c) 2024 dubiousconst282
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.
