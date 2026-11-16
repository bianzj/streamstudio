import unittest
import numpy as np
from thermal_imaging import band,outgoing_band,observe_radiance


class CoupledRadiometryTests(unittest.TestCase):
    def record(self,t=310.,sky=280.,eps=.9):
        a=np.zeros((1,2,13),float);a[:,:,0]=t;a[:,:,8]=.25;a[:,:,9:11]=t
        a[:,:,3]=eps*5.670374419e-8*t**4+(1-eps)*5.670374419e-8*sky**4
        return a
    def test_black_surface_is_planck(self):
        a=self.record(eps=1);r=outgoing_band(a,np.ones(1),np.full(1,.2),280,0)
        np.testing.assert_allclose(r,band(310),rtol=1e-12)
    def test_reflected_local_longwave_is_recovered(self):
        a=self.record();r=outgoing_band(a,np.full(1,.9),np.full(1,.2),280,0)
        np.testing.assert_allclose(r,.9*band(310)+.1*band(280),rtol=1e-12)
    def test_sunlit_shaded_emission_is_mixed_in_radiance(self):
        a=self.record();a[:,:,9]=320;a[:,:,10]=290
        a[:,:,3]=.9*5.670374419e-8*(.25*320**4+.75*290**4)+.1*5.670374419e-8*280**4
        r=outgoing_band(a,np.full(1,.9),np.full(1,.2),280,0)
        np.testing.assert_allclose(r,.9*(.25*band(320)+.75*band(290))+.1*band(280),rtol=1e-12)
    def test_solar_flux_to_radiance(self):
        a=self.record();a[:,:,2]=100
        r=outgoing_band(a,np.full(1,.9),np.full(1,.2),280,.02)
        base=.9*band(310)+.1*band(280)
        np.testing.assert_allclose(r-base,100/.2*.1*.02/np.pi,rtol=1e-12)
    def test_uniform_signal_survives_optics_and_adc(self):
        image=np.full((16,16),2.5)
        result=observe_radiance(image,0,5,seed=2,shape=(4,4),sigma=1.2,noise=0,gain_fpn=0)
        self.assertTrue(np.all(result['codes']==2048))
        self.assertEqual(result['codes'].dtype,np.uint16)
    def test_observation_does_not_mutate_ideal_signal(self):
        image=np.arange(256).reshape(16,16)/256;before=image.copy()
        observe_radiance(image,0,1,seed=2,shape=(4,4))
        np.testing.assert_array_equal(before,image)


if __name__=='__main__':unittest.main()
