TODO:

Add SSAO (GTAO)
Add GI

Refactor Material to output albedo, roughness, metalic and so on instead of pixel color. The material can be associated with a shading type (PBR, Toon and so on that will take the material and output pixel color), each material may have up to 7 output targets. Each shading type will read each output in its own way.

microui

Improve mesh streaming, use last used strategy to evict clusters and full meshes when buffers are full.
Add render graph settings in imgui to control maximum number of instances, pages uploads per frame, and decompressed clusters buffers

Add more scratch pages for atomic drawing, we need to load balance the draw commands. Check if this can increase performance.
Check for ways to decrease number of draw jobs and triangles for shadow jobs.

I'm thinking about refactoring the RenderGraph system, we could create the resources database externaly, with the RHI, and pass it to the renderGraph, then we can have multiple graphs using the same resource database. We need ways of syncronizing different graphs

Improve and debug memory usage, currently it is using too much memory.

Add Tracy, Profiler

Cleanup VirtualGeometryRenderer.

Improve VirtualGeometryFile, see if there is ways to keep just the pages for current cut. Add a clusters cache. 