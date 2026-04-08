
VirtualShadowMapPass:

VirtualShadowMapBookkeepingPass:

Sliding Window (2D Wraparound Addressing)

As the cascaded view frustum moves with the main camera, new pages that were originally located outside the edge of the view frustum may need to be drawn. To preserve cached pages, we use a sliding window mechanism ([Asirvatham and Hoppe 05] called 2D Wraparound Addressing ).

See Figure 1.12. As the view frustum moves, a new page enters the window, and an old page exits. By sliding the window, the newly entering page is mapped to the position where the old page exited in the VPT. For this, we need to store the offset of the light source matrix position relative to the origin in each cascade. Then, using the transformation function in Listing 12.1, we project the virtual page coordinates to "surround coordinates"—ultimately using the surround coordinates to find the VPT entry.

The Light Cascades:

The cascades are located in the origin (camera position) at (0,0,0) (relative to the camera). Lower cascades partially overlap higher cascades. Each cascade have page size two times bigger than the previous cascade.

Tables:

Each cascade have its own Virtual Page Table (VPT). Each entry is a 32bit value where the layout is:
1 bit for dirty flag.
1 bit for visible flag.
1 bit for allocated flag.
8 bits for pageX offset in the physical page table.
8 bits for pageY offset in the physical page table.

We have a 16k by 16k where we allocate pages of 128 by 128 pixels, to manage this atlas we have a Physical Page Table (PPT). Each entry in the PPT represents a page in the atlas, each entry in the PPT holds:
8 bits for VPT X position.
8 bits for VPT Y position,
1 bit for visible flag.
1 bit for allocated flag.

There is a bidirectional mapping between VPT and PPT. entry (x,y) in PPT represents page starting at (128*x, 128*y) going to (128*x + 128, 128*y + 128).

The format of both tables are U32.

CPU feeback:

for each cascade of every light, the cpu will send a bitset, 32 uints, making a 32 x 32 bit matrix. The 1 bits represents pages that should be invalidated.

for each cascade, we send the number of pages the light source matrix has moved relative to the previous frame.

We then release these invalid pages by resetting the allocation status of the corresponding entries in the Virtual Page Table (VPT) to 0. The code for releasing invalid pages can be found in Listing 12.2.


// Listing 12.1.Function used to convert from coordinates obtained 
//  by projecting by cascades light matrix into wrapped coordinates.
ivec3 virtual_page_coords_to_wrapped_coords(ivec3 page_coords, ivec2 cascade_offset)
{
  // Make sure that the virtual page coordinates are in page table bounds to prevent erroneous wrapping
  if (any(lessThan(page_coords.xy, ivec2(0))) ||
      any(greaterThan(page_coords.xy, ivec2(VSM_PAGE_TABLE_RESOLUTION - 1)))) {
    return ivec3(-1, -1, page_coords.z);
  }
  
  const ivec2 offset_page_coords = page_coords.xy + cascade_offset.xy;
  const ivec2 wrapped_page_coords = ivec2(mod(offset_page_coords.xy, vec2(VSM_PAGE_TABLE_RESOLUTION)));
  
  // Third coordinate specifies the VSM cascade level
  return ivec3(wrapped_page_coords, page_coords.z);
}


//Listing 12.2. Function used to free invalidated pages. 
// First, we determine if the virtual page needs to be freed from 
// the data sent from the CPU. If the page does need freeing,
// we translate into wrapped corrdinates and reset its state

// Cascade shift gives the offset in pages relative to the cascade
// position previous frame.
// Cascade offset is the offset used for the sliding window.
void free_wrapped_pages(ivec2 cascade_shift, ivec2 cascade_offset)
{
    // Get local invocation's page coordinates in virtual page space
    const ivec3 page_coords = ivec3(gl_LocalInvocationID.xyz);

    // Determine if this page lies in the wrapped (reused) region due to cascade shift
    const bool should_clear_wrap =
        (clear_offset.x > 0 && page_coords.x < cascade_shift.x) ||
        (clear_offset.x < 0 && page_coords.x > VSM_PAGE_TABLE_RESOLUTION + (cascade_shift.x - 1)) ||
        (clear_offset.y > 0 && page_coords.y < cascade_shift.y) ||
        (clear_offset.y < 0 && page_coords.y > VSM_PAGE_TABLE_RESOLUTION + (cascade_shift.y - 1));

    // Check if this page is marked for dynamic clearance (e.g., per-frame dirty flag)
    const bool should_clear_dynamic = extract_page_bit_from_mask(page_coords, bitmask);

    // Convert virtual page coordinates to wrapped (physical) coordinates
    const ivec3 wrapped_page_coords = virtual_page_coords_to_wrapped_coords(page_coords, cascade_offset);

    // If the page is in the wrapped region or marked dynamic, and is allocated — deallocate it
    if (should_clear_wrap || should_clear_dynamic)
    {
        const uint page_entry = imageLoad(virtual_page_table, wrapped_page_coords).r;
        if (get_is_allocated(page_entry))
        {
            imageStore(virtual_page_table, wrapped_page_coords, uvec4(0));  // Clear allocation flag
        }
    }
}


Marking visible pages:

assigns a shadow cascading index to each screen pixel based on a heuristic. This heuristic is used because simply selecting the finest cascading index doesn't provide sufficient control over shadow detail and memory consumption.

For Location-Given Rotationally Invariant: The method is simpler. While it doesn't achieve the strict screen pixel-to-shadow texel mapping as the first, it also avoids the problem of texel mapping outside the cascaded view frustum. The formula is as follows:

level = max(floor(log2(d/s_c0)), 0)

Where d is the world space distance from the texel to the camera. s_c0 is the side length of the cascaded view frustum of level 0.


For each depth vertex:

1. Cascade Selection : Select the corresponding cascade using any of the aforementioned heuristic methods;
2. Reprojection and VPT lookup:
    2.1 Using the selected cascaded matrix, reproject the fragment onto the cascaded UV coordinate system.
    2.2 Read the corresponding entry from the Virtual Page Table (VPT).
3. State-driven operation: Based on the state of the page in the page table, perform one of the following three operations (Figure 12.5):
    3.1 If the page has not yet been allocated : add its coordinates to the allocation request buffer for this frame;
    3.2 If a page has been allocated but not yet marked as visible : mark it as visible within this frame;
    3.3 If the page has been allocated and marked as visible : No action is taken.

Allocation request buffer is an atomic queue. 


Filling Allocator Buffers: Before allocating, we allocate physical pages to two buffers:

1. Buffer 1 Unallocated Physical Page Buffer : Stores the coordinates of all physical pages that have not yet been allocated to VSM virtual pages;
2. Buffer 2 Reclaimable Physical Page Buffer : Stores the coordinates of all allocated but corresponding virtual pages that are not marked as visible in this frame.

Key decision principle: If a physical page has been allocated and its corresponding mapped virtual page is visible in this frame, then no buffer will be inserted—these pages will be directly retained without needing to be released or reallocated.

Allocating pages:

The Allocating Pages phase is responsible for mapping virtual pages in the "allocation request buffer" to physical pages in the two buffers prepared in the previous step. The allocator's strategy is as follows:

1. The allocator will prioritize using the first buffer (the one with unallocated physical pages).
2. Pages from the second buffer will only be used when there are not enough pages available in buffer 1

When using a second buffer, the physical pages must be freed and then reallocated.

1. Coordination of release and reallocation: Since the physical page entry stores the coordinates of its current corresponding virtual page, the operations of releasing a physical page and allocating it to a new virtual page can be completed in the same stage.
2. Remove old associations: Before allocation, we will look up the virtual page entry that currently corresponds to the physical page and reset its status to "unallocated".
3. Establish a new association: After allocation, both the physical page and the new virtual page are marked as "allocated" and record each other's coordinates for subsequent queries.

We need two allocation request buffers, one that will have a limit, so we can controll the maximum ammount of pages we are drawing this frame. Another one will not have a limit, it is for future use, but it should be able to hold a lot of pages and all of them needs to be allocated in this frame, essentially we want a capped queue and a huge queue. all allocation requests for now goes into the first queue, if the queue is full the state in the vpt just carries to the next frame with the dirty bit as 1.

Before drawing the VSM, it's necessary to clear the physical memory of dirty pages (i.e., pages that need updating). Since cached pages need to be retained, simply clearing all physical pages is not feasible. Therefore, we use an allocation request buffer to identify pages that have just been allocated and need to be cleared. Subsequently, an Indirect Dispatch is used to clear the corresponding physical pages. The indirect dispatch will use the size of the allocation request buffer and dispatch 128 x 128 x len(allocation request buffer) threads, each thread just writes the clear value, the clear value will be send from the cpu.

Debug passes needed:

1. Pass that draws the current clip map, we can use the clip map index as a hash for a random color, inside the clipmap region I want to see page boundaries, we can use the vpt entry to create a random number to serve as an internsity parameter, we need to clamp this value, I want page boundaries to be light colors. We can also add other informations, like pages that got drawn that frame (if the page was marked as dirty). We can call this a VirtualShadowMapPagesDebugPass

2. We draw the VPT of each cascade on top of each other. The size of each cascade doubles, so we can draw the last cascade i, then i - 1, down to 0. Each cascade receives a random color. We can use RenderToQuadPass for this in the VirtualGeometryRederer. In the cascade, bright pixels are visible pages, mid darker pixels are cached pages, super darker pixels are unallocated entries in vpt.

For now we can add another pass that will take all dirty pages and mark them as valid. This is just to simulate future drawings to the page. 

We should create the shaders under assets/shaders/virtualshadowmap/wgsl/

We should have a vsm-common.wgsl that will contain common code for the vsm system.

For specific kernel implementations we can create a vsm-bookkeeping.wgsl to create the current pass.

We should have a VirtualShadowMapManager class that will compute the bitsets and compute and upload the camera offsets in the correct buffers. This class can also own those buffers. It should have methods to create lights, we will start only with directional lights for now, it will also create the matrices for each cascade. The class should also have a method called invalidateRegion that will receive an AABB and will, for every light and cascade, set the values in the bitset for the pages that needs to be invalidated in the current frame, those are going to be reseted every frame so we also need a function called resetInvalidations that can be called after the frame has finished.

VirtualShadowMapDrawPass:

Hierarchical Page Bounds generation:

we need to create a HPB for each cascade. We are going to create an image with same resolution as the VPT, the iamge could be a R32 image. If the page is dirty we mark the pixel as 1, if not we set it to black. We create mip levels the same way we do for depth pyramid. We reduce the image by checking if any pixel is dirty, if there is one pixel active in the generation then we mark the current pixel as red too. 

Cluster Culling:

We adapt the culling_multipass.wgsl to create vsm-culling-multipass.wgsl. We are going to remove frustum culling, occlusion culling will be adapter to use the HPB, we are going to project the node/cluster in the HPB and check if it touches a active pixel in the correct HPB, if it does we continue pushing the node to the queue or mark the cluster to be drawn. For the queue, we need a new queue node type, each node should hold the (cluster_id/node, light_id, cascade_id), during the prepare phase we expand the work by pushing one node per instance hierarchy, light and cascade. During cluster processing we need an efficient way of getting all pages that a cluster projecst itself, we need to output one indirect draw command per cluster per overlapped page. The prepare to the drawing phase should be same as culling_multipass.

Page Clearing:
We can have a pass that will prepare dirty pages to be cleared, in previous passes when we mark a page as dirty, we can atomically push it into a queue and record the count. We can then perform a cmdDrawIndirect to clear all pages with a single draw command, we draw a quad in the dirty page clip space in the atlas, writing the depth o color value to the correct clear value.

Page Drawing:
We perform a cmdDrawIndirect like we do in VirtualGeometryHardwareDrawPass, we draw the cluster depth from the light perspective in the page. The queue was written by the vsm culling pass.

Page Finish:
To avoid atomic contention, we can have a separate pass that will dispatch one thread per dirty page cleared/drawn in that frame were we remove the dirty status.


Fallback rendering:

We may define a budget for pages to be drawn/marked dirty in that frame. Pages outside of the budget should be rendered next frame when budge is available. To avoid regions without shadow data, we can use fallback pages drom higher clip maps, for example, supose an fallbackOffset of 2, if we have a page of clip map 0 visible, it may not be drawn to in the frame due to budget, we can, when marking the page as visible and dirty, mark the page in clip map currentClipMapIndex + fallbackOffset as dirty too (if its not currently valid), the fallback pages should always be drawn (they dont count as budget). When sampling, we check if the current page is valid, if not we can sample from the fallback page that is garanteed to have been drawn or was valid in that frame.

Since clip maps overlay a page in clip map i + 1 contains pages of clip map i. 