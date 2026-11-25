// ==========================================================================
// 文件：types.h
// 功能：STREAM 物理核心共用的简单数据结构（冠层、垄行、叶片/土壤光学参数、PROSPECT/BSM 系数表）。
// 主要模块：
//   1. Canopy：冠层结构参数（LAI、高度、叶片投影 G、热点、叶宽、IGBP 类型、树冠参数）
//   2. CropRows：垄行结构参数（目前未接入计算，见下方 FIX C9 说明）
//   3. LeafOptics：PROSPECT 叶片参数与热红外反射/透射
//   4. SoilOptics：BSM 土壤参数
//   5. OptCoeff：400–2400 nm、1 nm 网格上的 PROSPECT/BSM 系数（来自 defined/optipar_new.csv）
// 关系：scene/classes 解析地类表填充这些结构；optics / canopy / energy 等模块读取。
// 约定：长度 m，LAI m2 m-2，光谱网格 400–2400 nm、1 nm。
// ==========================================================================

#pragma once
#include <vector>

namespace stream {

// 结构体 Canopy：一个像元的冠层输入。lai（m2 m-2）、height（空气动力学冠层高度，m）、
// leafProjection（叶片投影系数 G，缺省 0.5）、hotspot（热点参数，缺省 0.2）、leafWidth（m）、
// type（IGBP 地类码）；treeStand / canopyHeight / crownShape 目前不参与计算（见下方说明）。
// FIX(2026-11-21 C9): every canopy is simulated as a homogeneous (turbid-medium) canopy. The crown fields below
// (treeStand, canopyHeight, crownShape) and CropRows are only consumed by physics/effective_lai.cpp, which is compiled
// but not called by the imaging, the energy balance, the inversion or the assimilation (porting pending). When they
// are given in classes.json the run logs a warning and records "homogeneous" in scene_summary.json canopyStructure.
// (Former comment, misleading: "Homogeneous canopy unless a crown (treeStand, canopyHeight, b) or row structure is supplied.")
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

// 结构体 CropRows：垄行结构（enabled 为 false 时不启用）；spacing / width / height 单位 m（待核对），
// azimuth 为垄向方位角（单位待核对）。当前引擎不使用该结构（均质冠层近似）。
struct CropRows {
    bool enabled = false;
    float spacing = 0.75f;
    float width = 0.35f;
    float height = 1.5f;
    float azimuth = 0.0f;
};

// 结构体 LeafOptics：PROSPECT 叶片参数（cab 叶绿素、cw 等效水厚、cdm 干物质、cs 棕色素、n 结构参数；
// 单位按 PROSPECT 习惯，待核对）与热红外叶片反射 reflTir / 透射 transTir（发射率 = 1 - 二者之和）。
struct LeafOptics {
    float cab = 40.0f, cw = 0.015f, cdm = 0.005f, cs = 0.0f, n = 1.5f;
    float reflTir = 0.025f, transTir = 0.0f;
};

// 结构体 SoilOptics：BSM 土壤参数（brightness 亮度；latitude / longitude 为 BSM 光谱形状参数，非地理坐标，待核对）。
struct SoilOptics {
    float brightness = 0.25f, latitude = 25.0f, longitude = 45.0f;
};

// PROSPECT and BSM coefficients on the 400-2400 nm, 1 nm grid (optipar_new.csv).
struct OptCoeff {
    std::vector<float> wl, nr, kab, kca, kdm, ks, kw, phiI, phiII, kcaV, kcaV2, kcaZ, kcant, gsv1, gsv2, gsv3, nw, phi;
};

}  // namespace stream
