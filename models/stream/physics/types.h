#pragma once
#include <vector>

namespace stream {

// Homogeneous canopy unless a crown (treeStand, canopyHeight, b) or row structure is supplied.
struct Canopy {
    float lai = 0.0f;
    float height = 1.0f;          // m, aerodynamic canopy height
    float leafProjection = 0.5f;  // G
    float hotspot = 0.2f;
    float leafWidth = 0.05f;      // m
    int type = 0;                 // IGBP code
    float treeStand = 0.0f;       // trees m-2
    float canopyHeight = 0.0f;    // crown height, m
    float crownShape = 0.0f;      // crown height / crown diameter
};

struct CropRows {
    bool enabled = false;
    float spacing = 0.75f;
    float width = 0.35f;
    float height = 1.5f;
    float azimuth = 0.0f;
};

struct LeafOptics {
    float cab = 40.0f, cw = 0.015f, cdm = 0.005f, cs = 0.0f, n = 1.5f;
    float reflTir = 0.025f, transTir = 0.0f;
};

struct SoilOptics {
    float brightness = 0.25f, latitude = 25.0f, longitude = 45.0f;
};

// PROSPECT and BSM coefficients on the 400-2400 nm, 1 nm grid (optipar_new.csv).
struct OptCoeff {
    std::vector<float> wl, nr, kab, kca, kdm, ks, kw, phiI, phiII, kcaV, kcaV2, kcaZ, kcant, gsv1, gsv2, gsv3, nw, phi;
};

}  // namespace stream
