Improve probe light initialization, we can average current neighbour probes already on screen to get initial light data.
Try to decrease number of needed rays per probe for initialization.
Improve Ray Guiding
Improve Ray sharing between probes
When we have multiple lights enabled, like sky light and point light, the light is not stable over multiple frames, we see some spots dancing arround with bright and dark colors.



Add simple fog
Dont fire rays from probes that are within distance fog
Add volumetric light, we can have a simple cascaded volume for this.

Terrain generation
Tree models