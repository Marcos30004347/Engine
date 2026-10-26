"""UV rasterization regressions; run with python3 -m unittest discover here."""
import unittest
import tempfile
from pathlib import Path
from unittest.mock import patch
from render_dwelling_eye_level import render_interior

import numpy as np

from render_city_model import raster_triangle
from dwelling_materials import Materials


class TextureSamplingTests(unittest.TestCase):
    def test_masonry_uses_one_authored_texture_with_metric_uvs(self):
        data={"textureManifest":"assets/procedural/textures/medieval-pixel/materials.json",
              "surfaceMaterials":[1,9,22],
              "triangleUVs":[[[-3.2,0],[0,3.2],[3.2,0]]]*3}
        materials=Materials(data)
        for triangle, key in enumerate(["1","9","22"]):
            _,uv,texture=materials.surface(data,triangle,[0,0,0])
            self.assertIs(texture,materials.textures[key])
            self.assertNotIn("blend",materials.records[key])
            np.testing.assert_allclose(uv,np.array(data["triangleUVs"][triangle]) /
                                       materials.records[key]["repeatMetres"])

    def test_masonry_does_not_change_when_quad_diagonal_changes(self):
        # Alternate triangulations must sample the same blocks, including negative UVs.
        data={"textureManifest":"assets/procedural/textures/medieval-pixel/materials.json"}
        texture=Materials(data).textures["1"]
        points=[(0,0,1),(31,0,1),(31,31,1),(0,31,1)]
        uv=[[-.8,-.4],[1.2,-.4],[1.2,1.6],[-.8,1.6]]
        images=[]
        for faces in [[(0,1,2),(0,2,3)],[(0,1,3),(1,2,3)]]:
            color=np.zeros((32,32,3),dtype=np.uint8);depth=np.full((32,32),-np.inf)
            for face in faces:
                raster_triangle(color,depth,*(points[i] for i in face),(0,0,0),
                                [uv[i] for i in face],texture=texture)
            images.append(color)
        np.testing.assert_array_equal(images[0][1:30,1:30],images[1][1:30,1:30])

    def test_ground_blend_is_normalized_and_linear_light(self):
        color=np.zeros((4,4,3),dtype=np.uint8);depth=np.full((4,4),-np.inf)
        textures=[np.full((1,1,3),v,dtype=np.uint8) for v in [0,255,0]]
        ground=(np.array([[.5,.5,0]]*3),textures,[[1,1]]*3)
        raster_triangle(color,depth,(0,0,1),(4,0,1),(0,4,1),(0,0,0),[[0,0],[1,0],[0,1]],ground=ground)
        # Equal black/white in linear light encodes near 188, not 128.
        self.assertTrue(186<=color[0,0,0]<=188)

    def test_perspective_renderer_preserves_ground_weights_through_near_clip(self):
        data={"vertices":[[-1,0,-1],[1,0,2],[0,1,2]],"triangles":[[0,1,2]],
              "materials":[50],"triangleUVs":[[[0,0],[2,0],[0,2]]]}
        weights=np.eye(3)
        textures=[np.full((1,1,3),v,dtype=np.uint8) for v in [0,128,255]]
        with tempfile.TemporaryDirectory() as directory, patch("render_dwelling_eye_level.Materials") as material, patch("render_dwelling_eye_level.raster_triangle") as raster:
            material.return_value.surface.return_value=([0,0,0],np.zeros((3,2)),None)
            material.return_value.ground.return_value=(weights,textures,[[2,2]]*3)
            render_interior(data,Path(directory)/"clipped.png",[0,0,0],[0,0,1],width=128,height=128)
            self.assertEqual(raster.call_count,2)
            for call in raster.call_args_list:
                uv=np.asarray(call.args[6]);ground=call.args[10]
                self.assertEqual(uv.shape,(3,2))
                np.testing.assert_allclose(ground[0].sum(axis=1),1)
                self.assertTrue(np.all(ground[0]>=0))
                self.assertEqual(call.args[8],1.)

    def test_fitted_material_clamps_without_repeating_hardware(self):
        library=Materials({})
        library.records={"8":{"wrap":"clamp"}}
        data={"surfaceMaterials":[8],"triangleUVs":[[[-.1,0],[1.1,0],[0,1]]]}
        _,uv,_=library.surface(data,0,[0,0,0])
        self.assertTrue(np.all(uv>=0) and np.all(uv<1))
        self.assertGreater(uv[1,0],.999)

    def draw(self, uv, perspective=False):
        color=np.zeros((4,4,3),dtype=np.uint8)
        depth=np.full((4,4),-np.inf)
        texture=np.array([[[255,0,0],[0,255,0]],[[0,0,255],[255,255,0]]],dtype=np.uint8)
        raster_triangle(color,depth,(0,0,1),(4,0,.5),(0,4,.25),(0,0,0),uv,texture,1,perspective)
        return color

    def test_nearest_and_v_orientation(self):
        image=self.draw([[0,0],[1,0],[0,1]])
        np.testing.assert_array_equal(image[0,0],[0,0,255])
        np.testing.assert_array_equal(image[0,2],[255,255,0])

    def test_repeat_supports_negative_and_large_uvs(self):
        uv=np.array([[0,0],[1,0],[0,1]])
        np.testing.assert_array_equal(self.draw(uv),self.draw(uv+[2,-3]))

    def test_perspective_correct_not_affine(self):
        uv=[[0,0],[1,0],[0,1]]
        np.testing.assert_array_equal(self.draw(uv,False)[2,0],[255,0,0])
        np.testing.assert_array_equal(self.draw(uv,True)[2,0],[0,0,255])


if __name__=="__main__":
    unittest.main()
