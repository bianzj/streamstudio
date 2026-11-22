#ifndef HISTREAM_IMAGE_ENVIRONMENT_GLSL
#define HISTREAM_IMAGE_ENVIRONMENT_GLSL

// Multiple-scattering environment of the VoxelEB optical image.
//
// The shortwave scattering pass reuses the N x 64 directional field for every
// order and spectral group, and the TIR diffuse pass overwrites it afterwards,
// so the image cannot read it. Stage 3 of every order therefore also stores,
// per voxel and sensor band, the collided (scattered at least once) incident
// shortwave of that band's spectral group - two-face total for foliage and
// media, cosine-weighted irradiance for solid facets, i.e. the skyDiffuse
// convention - summed over the explicit orders plus the spectral-invariant
// tail of the energy balance. It is normalized by the group illumination and,
// for foliage, by w/(1 - p w) of the group-mean leaf albedo, so the image
// rescales it to each band: E_b = value * I_b [* w_b/(1 - p w_b)].
//
// Radiance buffer layout (buffer.cpp): N*64 VoxelRad directional field, N*64
// sky gap transmittances (two per VoxelRad entry), then N*n_wave of these
// coefficients (two per entry). setting.imageEnvironment is 0 when they are
// not allocated (index range); the image then keeps single scattering only.

// Spectral invariants: the collided field of a leaf medium scales with
// w/(1 - p w) (first collision w, each further one p w). Same cap as the
// energy-balance tail (voxelrad_scattering_VNIR.comp).
float leafSpectralInvariant(float albedo, float recollision)
{
    float w = clamp(albedo, 0.0, 1.0);
    return w / (1.0 - min(recollision * w, 0.9));
}

// Shortwave band (0..2000 = 400..2400 nm) whose spectral group carries the
// environment of a sensor band, or -1. Bands between 2.5 and 6 um that get a
// solar term (mwir_solar.glsl) use the last group (up to 2400 nm). The optical
// test and band index follow isOpticalBand/atmosphereBand of the image.
int imageEnvironmentBand(float wavelength)
{
    float nm = wavelength <= 2.5 ? wavelength * 1000.0 : wavelength;
    if(nm <= 2500.0) return clamp(int(round(nm - 400.0)), 0, 2000);
    float um = wavelength > 50.0 ? wavelength * 0.001 : wavelength;
    return (um > 2.5 && um < 6.0) ? 2000 : -1;
}

uint imageEnvironmentEntry(uint voxel, uint band)
{
    uint k = voxel * uint(setting.n_wave) + band;
    return uint(setting.voxelCount) * uint(DIFFUSE_ANGLE_NUM * 3 / 2) + (k >> 1);
}

float loadImageEnvironment(uint voxel, uint band)
{
    uint entry = imageEnvironmentEntry(voxel, band);
    return ((voxel * uint(setting.n_wave) + band) & 1u) == 0u
        ? voxelRads[entry].cumulated : voxelRads[entry].induced;
}

void storeImageEnvironment(uint voxel, uint band, float value)
{
    uint entry = imageEnvironmentEntry(voxel, band);
    if(((voxel * uint(setting.n_wave) + band) & 1u) == 0u) voxelRads[entry].cumulated = value;
    else voxelRads[entry].induced = value;
}

#endif
