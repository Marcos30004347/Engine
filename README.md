TODO:
Add virtual textures


Add Materials
Add SSAO
microui

Repository refactor:
- Move VirtualShadowMap stuff outside of virtualgeometry folder into its own folder, the structure should still contain a rendering folder for passes
- Create a three new folders inside src called editor, runtime, and cli.
- Move VirtualGeometryFile creation to editor/virtualgeometry
- Add a CMakeLists file to each folder, top cmake file should just load each folder separately, pay attention to dependencies, METIS and meshoptmizer should only be included in editor for example.
- editor project should have runtime project as a dependency
- cli project should have editor as a dependency
- inside cli create cli tools that will be used to create virtual geometry files from glft and obj files. Can be under cli/virtualgeometry/importer
- inside cli create cli tools to create virtual textures from jpeg, pngs and so on.


- document each function inside all wgsl shaders, documenting arguments and behaviour.

- create a folder called docs/
- inside docs create a md file and document in detail the virtual geometry passes, including file creation, file encoding, passes, and shaders.
- inside docs create a md file and document in detail the virtual shadow system, including passes and shaders.
- inside docs create a md file and document virtual texture system
- document the current material pass

- Add contact shadows
- optmize current material pass, we should use a or atomic to set visible materials, each material have an active bool/bit. We then prepare draw indirect, we check on each quad the materials that are visible there, from that we prepare indirect draw commands, for each visible material and quad pair, we shade the quad using the material in a latter pass. Each material can have its own Pass. we can create a MaterialPass class that extends Pass, this class will receive the custom shader, and prepare the culling pass (compute the quads that have this material), prepare indirect draw commands and dispatch the actual shading using the custom shader. The MaterialPass should have a metod to get the default binding group (group 0) with the common parameters for the material (like gbuffer textures)
- we should detatch images from materials, lets create a .virtualimage file that will store the images loaded by the material.
- pixel interpolation should still be a material parameter.
- material should accept a custom shader, for now we can create a default one that just outputs the base color and shadow information.
- we should have a material.wgsl file that will contain utility functions, like sampliling if a pixel is in light or in shadow by a given light.
- when we include this file we will already add the bindings needed, including gbuffer textures.
- the material shaders should also receive camera settings and so on. 
- maybe we could find a way to pass custom bindings, we set them after including the material.wgsl, we could use a different group for custom bindings, and find a way to pass them to the material using the render graph.



- we should add an extra pass to vsm, this will iterate over all the lights and prepare a gbuffer texture for shadow information. this texture should encode shadow ambient color, and light color for bright pixels and shadow pixels, it should also blend different lights bright and shadow pixels properly. This texture will also be feed to the material pass to shadow pixels, we should use this in output of key 1. we can display it on key 2 and remove the current debug pass.
- lets add contaxt shadow pass to vsm, we can use the current frame depth buffer and use efficient methods. it should add contact shadows to the output of vsm.


- Lets improve virtual texture system, we should every frame compute priority for pages based on camera distance and visibility.

