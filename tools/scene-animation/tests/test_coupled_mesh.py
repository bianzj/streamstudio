"""Numerical mesh must conserve area and preserve the original display scene."""
import unittest
import numpy as np
from coupled_town_prepare import clip_triangle,subdivide_triangle


def areas(t):
    return np.linalg.norm(np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]),axis=1)/2


class CoupledMeshTests(unittest.TestCase):
    def test_refinement_conserves_area_and_plane(self):
        t=np.array([[0,0,2],[8,0,2],[0,6,2]],float)
        fine=subdivide_triangle(t,2)
        self.assertAlmostEqual(areas(fine).sum(),24,places=10)
        self.assertTrue(np.all(fine[:,:,2]==2))
        self.assertTrue(np.all(np.cross(fine[:,1]-fine[:,0],fine[:,2]-fine[:,0])[:,2]>0))
        edges=np.concatenate([fine[:,1]-fine[:,0],fine[:,2]-fine[:,1],fine[:,0]-fine[:,2]])
        self.assertLessEqual(np.linalg.norm(edges,axis=1).max(),2+1e-10)

    def test_large_ground_clips_to_exact_roi(self):
        quad=np.array([[[-100,-100,0],[100,-100,0],[100,100,0]],[[-100,-100,0],[100,100,0],[-100,100,0]]],float)
        parts=np.concatenate([clip_triangle(t,[-2,-3,2,3]) for t in quad])
        self.assertAlmostEqual(areas(parts).sum(),24,places=10)
        self.assertGreaterEqual(parts[:,:,0].min(),-2-1e-10)
        self.assertLessEqual(parts[:,:,1].max(),3+1e-10)

    def test_outside_triangle_is_empty(self):
        t=np.array([[10,10,0],[11,10,0],[10,11,0]],float)
        self.assertEqual(len(clip_triangle(t,[-2,-3,2,3])),0)


if __name__=='__main__':unittest.main()
