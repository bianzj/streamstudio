#ifndef MWIR_SOLAR_GLSL
#define MWIR_SOLAR_GLSL

// Reflected sunlight in thermal (> 2.5 um) bands.
//
// Shortwave bands are normalized-illumination bands; thermal bands are
// absolute Planck radiance (W m-2 sr-1 um-1). Between 2.5 um and the 6.3 um
// water-vapour band the surface solar irradiance is comparable to the
// emitted radiance (about 10 W m-2 um-1 at the top of the atmosphere near
// 3.8 um), so thermal bands there also receive the solar beam and the
// scattered sky, in absolute units.
//
// MWIR_SOLAR_SHAPE is the bundled defined/Esun_.dat (the same file VoxelEB
// and FacetEB use for the shortwave spectral shape) on its 2500-6000 nm,
// 100 nm grid, divided by its 400-2400 nm integral in um. Multiplied by a
// shortwave total (W m-2, distributed over 400-2400 nm as in the energy
// balance) it is the spectral irradiance in W m-2 um-1. The file already
// carries the atmospheric absorption bands (2.7, 4.3 um) and falls to zero
// in the 6.3 um water band; beyond 6 um (8-14 um window) the solar term is
// below 0.1 % of the emission and is not modelled, so long-wave bands are
// unchanged.
const int MWIR_SOLAR_COUNT = 36;
const float MWIR_SOLAR_SHAPE[MWIR_SOLAR_COUNT] = float[MWIR_SOLAR_COUNT](
    1.7800e-02, 1.3376e-03, 6.2215e-09, 3.8114e-04, 2.7788e-03, 4.6614e-03,
    6.4018e-03, 6.2056e-03, 7.0701e-03, 1.1103e-02, 1.3684e-02, 1.3498e-02,
    1.2362e-02, 1.1301e-02, 1.0473e-02, 9.0935e-03, 6.4206e-03, 3.2183e-03,
    1.2786e-04, 1.4780e-04, 1.0580e-03, 2.4357e-03, 3.0036e-03, 2.3447e-03,
    1.7338e-03, 1.3681e-03, 7.3824e-04, 2.0866e-04, 6.5639e-05, 1.2040e-05,
    1.1073e-06, 3.5406e-08, 9.0807e-14, 0.0, 0.0, 0.0);

// Scattered (sky) sunlight relative to the beam beyond 2.5 um, per unit of
// the respective shortwave share: the ratio of the normalized Esky_/Esun_
// shapes over 2300-2400 nm. Sky scattering keeps falling with wavelength,
// so holding it constant is an upper bound; the Esky_ values beyond 2.5 um
// are atmospheric emission, already modelled by the sky temperature.
const float MWIR_SKY_SOLAR_RATIO = 0.0847;

// Spectral shape (1/um) at a band centre given in um or nm; 0 for
// wavelength <= 2.5 um (optical bands) and >= 6 um.
float mwirSolarShape(float wavelength)
{
    float um = wavelength > 50.0 ? wavelength * 0.001 : wavelength;
    if(!(um > 2.5) || !(um < 6.0)) return 0.0;
    float x = (um - 2.5) * 10.0;
    int index = clamp(int(floor(x)), 0, MWIR_SOLAR_COUNT - 2);
    float t = clamp(x - float(index), 0.0, 1.0);
    return max(mix(MWIR_SOLAR_SHAPE[index], MWIR_SOLAR_SHAPE[index + 1], t), 0.0);
}

// Clear-sky global horizontal shortwave irradiance (Haurwitz 1945), W m-2:
// the absolute scale for RayTracing/VoxelRT, which have no meteorology
// (about 890 W m-2 at 30 deg zenith).
float clearSkyGlobalShortwave(float cosSolarZenith)
{
    return cosSolarZenith > 0.0
        ? 1098.0 * cosSolarZenith * exp(-0.057 / cosSolarZenith) : 0.0;
}

// VoxelRT thermal-band solar sources, divided by pi because its thermal
// fields are radiance L while the optical fields are pi*L. The normalized
// illumination (direct beam-normal, diffuse = horizontal sky irradiance,
// reference direct*cos + diffuse with the beam only above y = 0.01, see
// referenceIrradianceRT) is scaled to the clear-sky global shortwave.
// x: beam (used like lightSet.direct), y: scattered sky (added to the
// Planck sky like lightSet.diffuse). Both are exactly 0 outside 2.5-6 um.
vec2 voxelRtThermalSolar(float wavelength, float direct, float diffuse,
                         vec3 sunDirection)
{
    float shape = mwirSolarShape(wavelength);
    if(!(shape > 0.0)) return vec2(0.0);
    float cosSolarZenith = normalize(sunDirection).y;
    float beam = cosSolarZenith >= 0.01 ? direct : 0.0;
    float reference = beam * cosSolarZenith + diffuse;
    if(!(reference > 1.0e-6)) return vec2(0.0);
    float scale = clearSkyGlobalShortwave(cosSolarZenith) / reference
        * shape * 0.318309886;
    return vec2(scale * beam, scale * diffuse * MWIR_SKY_SOLAR_RATIO);
}

#endif
