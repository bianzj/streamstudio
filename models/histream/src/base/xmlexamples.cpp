//
// Created by admin on 2024/1/26.
//
#include "xmlexamples.h"

std::filesystem::path XmlExamples::rootDirectory()
{
    // DEFERRED[低] Bug: 头文件注释说还会查找“可执行文件所在目录”，实现只查 current_path 与 <cwd>/models/histream；从其他工作目录启动时找不到 defined/meteo.txt，静默回落到 cwd，示例的 meteo/Esky/Esun 路径全部失效；建议：增加 GetModuleFileNameW 得到的 exe 目录作为候选，全部找不到时报错。
    const std::filesystem::path current = std::filesystem::current_path();
    for (const std::filesystem::path& candidate : {current / "models" / "histream", current}) {
        if (std::filesystem::exists(candidate / "defined" / "meteo.txt")) return candidate;
    }
    return current;
}

std::string XmlExamples::exampleDirectory(const char* name)
{
    // DEFERRED[中] 安全: path::string() 在 Windows 上按 ANSI 代码页转换，工作目录含代码页外字符时抛 std::system_error；随后 create_directories(std::string) 又按窄字符解释，并且会在 cwd 下静默创建 examples 目录（副作用），失败时抛 filesystem_error 无上下文；建议：以 std::filesystem::path 保存 projectDir，create_directories 使用 error_code 重载并给出明确报错。
    return (rootDirectory() / "examples" / name).string();
}

std::shared_ptr<RaytracingXml> XmlExamples::raytracing()
{
    if (!m_pRaytracingXml) createMinimalRaytracingxml();
    std::filesystem::create_directories(m_pRaytracingXml->projectDir);
    return m_pRaytracingXml;
}

std::shared_ptr<VoxelRTXml> XmlExamples::voxelrt()
{
    if (!m_pVoxelrtXml) createMinimalVoxelrtxml();
    std::filesystem::create_directories(m_pVoxelrtXml->projectDir);
    return m_pVoxelrtXml;
}

std::shared_ptr<VoxelEBXml> XmlExamples::voxeleb()
{
    if (!m_pVoxelebXml) createMinimalVoxelebxml();
    std::filesystem::create_directories(m_pVoxelebXml->projectDir);
    return m_pVoxelebXml;
}

std::shared_ptr<VoxelEBXml> XmlExamples::watereb()
{
    if (!m_pWaterEBXml) createMinimalWaterEBxml();
    std::filesystem::create_directories(m_pWaterEBXml->projectDir);
    return m_pWaterEBXml;
}

void XmlExamples::createVoxelebxml(){
    // DEFERRED[低] 可维护性: createVoxelebxml/createRaytracingxml/createVoxelrtxml 在 src 中无任何调用，且硬编码开发机绝对路径（C:\cases、F:\work）；若被误调用将直接因文件不存在失败，其中还含下面标出的参数错误；建议：删除或改为基于 rootDirectory() 的可运行示例。
    m_pVoxelebXml=std::make_shared<VoxelEBXml>();

    m_pVoxelebXml->projectDir = R"(C:\cases\histream\)";            // 均质场景与SCOPE对比的时间序列观测：LAI=2
    m_pVoxelebXml->definedDir = R"(C:\work\streamstudio\models\histream)";

    m_pVoxelebXml->settingxml.n_sample=32;
    m_pVoxelebXml->settingxml.maxDepth = 32;
    m_pVoxelebXml->settingxml.theGPU = 0;


    m_pVoxelebXml->lightxml.name = "Solar";
    m_pVoxelebXml->lightxml.solarAngle = {40, 30};
    m_pVoxelebXml->lightxml.direct = 0.9;
    m_pVoxelebXml->lightxml.diffuse = 0.1;
    m_pVoxelebXml->lightxml.skyTemperature = 250;
    m_pVoxelebXml->lightxml.solarTemperature = 6000;

    m_pVoxelebXml->sensorxml.name = "UAV";
    m_pVoxelebXml->sensorxml.resolution = {50, 50};
    m_pVoxelebXml->sensorxml.isImage = true;
    m_pVoxelebXml->sensorxml.isAlbedo = false;
    m_pVoxelebXml->sensorxml.isDisplay = false;
    m_pVoxelebXml->sensorxml.isTemperature = true;
    m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
    m_pVoxelebXml->sensorxml.waves = {10500};
    m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;

    m_pVoxelebXml->scenexml.background.sceneSize={100, 100, 50}; // length, width, height
    m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
/*    m_pVoxelebXml->scenexml.background.sMin={0,0,0};
    m_pVoxelebXml->scenexml.background.sMax={600,600,10};*/
    m_pVoxelebXml->scenexml.background.stepsize_surface=0.3;
    m_pVoxelebXml->scenexml.background.stepsize_height=0.3;
//    m_pVoxelebXml->scenexml.stepsize_atmosphere = 1000;


    m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
    m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
    m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
    m_pVoxelebXml->scenexml.background.isDEM = {false};
    m_pVoxelebXml->scenexml.background.DEMFile = "";
    m_pVoxelebXml->scenexml.background.lat = 40.3574;
    m_pVoxelebXml->scenexml.background.lon = 115.7923;


    PrimEntity primEntity1;
    primEntity1= PrimEntity{"building",
                            {"wall","roof"},
                            {"wall","roof"},
                            {"K300","K300"},
                            {"crown","crown"},
                            {"soilset","soilset"},
                            Type::BUILDING,
                            {ShapeType::ELLIPSOID,5,5,5,glm::vec3(0,0,0)},
                            false," ",
                            true,m_pVoxelebXml->projectDir + "\\height_0_5.tif",
                            false, " "};

    PrimEntity primEntity2;
    primEntity2 = PrimEntity{"tree",
                             {"crown"},
                             {"leaf"},
                             {"K300"},
                           {"crown"},
                           {"leafbio"},
                           Type::VEGETATION,
                           {ShapeType::CUBE,3,50,50,glm::vec3(0,0,0)},
                           false,"",
                           false,"",
                           true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};


    m_pVoxelebXml->scenexml.objEntities = {};
    m_pVoxelebXml->scenexml.primEntities = {primEntity2};



    SpectralXml spectralXml1;
    spectralXml1.spectralName="soil";
    spectralXml1.type = spectralType::OTHER;
    spectralXml1.reflectances = {0.04};
    spectralXml1.transmittance ={0};
    spectralXml1.path = R"(C:\cases\histream\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
    spectralXml1.tau_tir = 0;
    spectralXml1.refl_tir = 0.04;

    SpectralXml spectralXml3;
    spectralXml3.spectralName="wall";
    spectralXml3.type = spectralType::OTHER;
    spectralXml3.reflectances = {0.05};
    spectralXml3.transmittance ={0};
    spectralXml3.path =  m_pVoxelebXml->definedDir + "\\defined\\VNIR_construction_tar_asphalt.txt";
//    spectralXml3.path = "F:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_concrete_cement_solid.txt";
    spectralXml3.tau_tir = 0;
    spectralXml3.refl_tir = 0.05;

    SpectralXml spectralXml4;
    spectralXml4.spectralName="roof";
    spectralXml4.type = spectralType::OTHER;
    spectralXml4.reflectances = {0.05};
    spectralXml4.transmittance ={0};
    spectralXml4.path =  m_pVoxelebXml->definedDir + "\\defined\\VNIR_construction_tar_asphalt.txt";
//    spectralXml4.path = "D:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_concrete_cement_solid.txt";
    spectralXml4.tau_tir = 0;
    spectralXml4.refl_tir = 0.05;

    SpectralXml spectralXml2{};
    spectralXml2.spectralName="leaf";
    spectralXml2.type = spectralType::PROSPECT;
    spectralXml2.reflectances = {0.02};
    spectralXml2.transmittance ={0};
    spectralXml2.fp = {80,0.009,0.012,0,1.46};
    spectralXml2.tau_tir = 0;
    spectralXml2.refl_tir = 0.02;

    m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};


    m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
                                  {"K300", 305, 295}};


    PropertyXml propertyXml1,propertyXml2,propertyXml3;
    propertyXml1 = {"soilset",Type::SOIL};
    // DEFERRED[中] Bug: 与 xmlexamples.h 中最小示例相同，按位置初始化后 Tsoil=0.25、smc=25（字段顺序 …lambdas, Tsoil, smc, SatWater），土壤温度与含水量互换；建议：用指定成员赋值。
    propertyXml1.soilset = SoilSet{1,2000,1180,1800,1.55,0.25,25,0.45};

    propertyXml2 = {"leafbio",Type::VEGETATION};
    propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
        {0.2,0.3,288,313,328},25,0.507,0,1,1,0};

    propertyXml3 = {"buildup",Type::BUILDING};
    propertyXml3.buildup = BuildUp{1,500,1180,1800,1.55,25};




    m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};

    m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 0.666, 3, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};

    // DEFERRED[中] 数值: AeroCond 只给出前 5 个字段（type=1, L=0, ustar=10, hc_veg=3, hc_build=12），lai/leafwidth/cover 被值初始化为 0；leafwidth=0 时叶片边界层阻抗 ∝ sqrt(w/u) 为 0（热交换无穷大），L=0 使稳定度函数 z/L 除零；ustar=10 m/s 也远超常见值（0.1–1）；建议：补全全部字段并给出物理合理值。
    m_pVoxelebXml->aerocondxml = {AeroType::ONE, {1, 0, 10, 3, 12.}, "", 1000};

    m_pVoxelebXml->meteoxml.meteofile =  R"(C:\cases\histream\15m_meteo.txt)";
    m_pVoxelebXml->atomcondxml.rlifile =  m_pVoxelebXml->definedDir + "\\defined\\Esky_.dat";
    m_pVoxelebXml->atomcondxml.rinfile =  m_pVoxelebXml->definedDir + "\\defined\\Esun_.dat";
    //   m_pVoxelebXml->meteoxml.aerocond = {10,10,3,0.36};
}


void XmlExamples::createRaytracingxml(){
    m_pRaytracingXml=std::make_shared<RaytracingXml>();

    m_pRaytracingXml->projectDir = "F:\\work\\field_aoyunlst\\field_data\\aoyun\\";
    m_pRaytracingXml->definedDir = "F:\\work\\field_aoyunlst\\field\\";

    m_pRaytracingXml->settingxml.n_sample=32;
    m_pRaytracingXml->settingxml.maxDepth = 32;
    m_pRaytracingXml->settingxml.theGPU = 0;

    m_pRaytracingXml->sensorxml.name = "UAV";
    m_pRaytracingXml->sensorxml.resolution = {1024,1024};
    m_pRaytracingXml->sensorxml.isImage = true;
    m_pRaytracingXml->sensorxml.isAlbedo = false;
    m_pRaytracingXml->sensorxml.isDisplay = false;
    m_pRaytracingXml->sensorxml.isTemperature = true;
    m_pRaytracingXml->sensorxml.viewAngles = {{10,10},{45,45}};
    m_pRaytracingXml->sensorxml.waves = {650,850,10500};
    m_pRaytracingXml->sensorxml.projection = Projection::PARALLAL;


    m_pRaytracingXml->lightxml.name = "Solar";
    m_pRaytracingXml->lightxml.solarAngle = {25,25};
    m_pRaytracingXml->lightxml.direct = 0.9;
    m_pRaytracingXml->lightxml.diffuse = 0.1;
    m_pRaytracingXml->lightxml.skyTemperature = 250;
    m_pRaytracingXml->lightxml.solarTemperature = 6000;


    m_pRaytracingXml->scenexml.background.sceneSize={5715,5081,10};
    m_pRaytracingXml->scenexml.background.sceneOrigin={0,0,0};
//    m_pRaytracingXml->scenexml.sMin={0,0,0};
//    m_pRaytracingXml->scenexml.sMax={50,50,10};
    m_pRaytracingXml->scenexml.background.bgSpectralName ="soil";
    m_pRaytracingXml->scenexml.background.bgThermalName ="K310";
    m_pRaytracingXml->scenexml.background.bgPropName ="soilset";
    m_pRaytracingXml->scenexml.background.isDEM = {false};
    m_pRaytracingXml->scenexml.background.DEMFile = "";
    m_pRaytracingXml->scenexml.background.stepsize_surface = 1;

    ObjEntity objEntity1 = ObjEntity{"aoyun","F:\\work\\field_aoyun\\field_data\\aoyun\\studyarea.obj",
                                     {"wall","roof"},{Type::BUILDING,Type::BUILDING},{"soil","soil"},{"K310","K310"},
                                     {{2857.5,2540.7,0}},{1},{0},false};

    ObjEntity objEntity2 = ObjEntity{"aoyun","F:\\work\\field_aoyun\\field_data\\aoyun\\single_tree_a2_b3_q0.3_lai2.7.obj",
                                     {"Crown"},{Type::VEGETATION},{"leaf"},{"K300"},
                                     {},{},{},true,"F:\\work\\field_aoyun\\field_data\\aoyun\\entity_1_position.txt"};


    PrimEntity primEntity;
//    primEntity= PrimEntity{"primTree",{"crown"},{"leaf"},{"K300"},{"crown"},
//                                {ShapeType::ELLIPSOID},{{5,5,5,glm::vec3(0,0,0)}},
//                                {{10,40,0},{40,10,0}},{1,1,1},{0,0,0}};
    // DEFERRED[低] 未初始化: primEntity 为默认初始化的局部变量（type/shape 为垃圾值），且名称与绑定为空，却被放入 primEntities；任何遍历 primEntities 的代码都会读到未初始化的类型/形状；建议：不放入空实体，或写成 PrimEntity primEntity{}。
    m_pRaytracingXml->scenexml.objEntities = {objEntity1,objEntity2};
    m_pRaytracingXml->scenexml.primEntities = {primEntity};


    SpectralXml spectralXml1;
    spectralXml1.spectralName="soil";
    spectralXml1.type = spectralType::CUSTOM;
    // DEFERRED[中] 物理/未初始化: 土壤背景透射率设为 0.23/0.25（不透明地表应为 0，光线会穿透地面）；spectralXml1 默认初始化且未设置 refl_tir/tau_tir/fp，10500 nm 波段读到未初始化值（resolveSpectrals 中有限垃圾值会被直接当作 TIR 反射率，leaf 同理）；建议：透射率改为 0，写成 SpectralXml spectralXml1{} 并显式给出 refl_tir/tau_tir。
    spectralXml1.reflectances = {0.23,0.25,0.05};
    spectralXml1.transmittance ={0.23,0.25,0};
    SpectralXml spectralXml2{};
    spectralXml2.spectralName="leaf";
    spectralXml2.type = spectralType::CUSTOM;
    spectralXml2.reflectances = {0.13,0.15,0.025};
    spectralXml2.transmittance ={0.13,0.15,0};
    m_pRaytracingXml->spectralxmls = {spectralXml1,spectralXml2};


    m_pRaytracingXml->thermalxmls = {{"K310",320,300},{"K300",305,295}};
}

void XmlExamples::createVoxelrtxml(){
    m_pVoxelrtXml=std::make_shared<VoxelRTXml>();

    m_pVoxelrtXml->projectDir = "F:\\work\\field_aoyunlst\\field_data\\aoyun\\";
    m_pVoxelrtXml->definedDir = "F:\\work\\field_aoyunlst\\field\\";

    m_pVoxelrtXml->settingxml.n_sample=32;
    m_pVoxelrtXml->settingxml.maxDepth = 32;
    m_pVoxelrtXml->settingxml.theGPU = 0;
    m_pVoxelrtXml->settingxml.isUAVtrave = false;


    m_pVoxelrtXml->lightxml.name = "Solar";
    m_pVoxelrtXml->lightxml.solarAngle = {40, 30};
    m_pVoxelrtXml->lightxml.direct = 0.9;
    m_pVoxelrtXml->lightxml.diffuse = 0.1;
    m_pVoxelrtXml->lightxml.skyTemperature = 250;
    m_pVoxelrtXml->lightxml.solarTemperature = 6000;

    m_pVoxelrtXml->sensorxml.name = "UAV";
    m_pVoxelrtXml->sensorxml.resolution = {1024, 1024};
    m_pVoxelrtXml->sensorxml.isImage = true;
    m_pVoxelrtXml->sensorxml.isAlbedo = false;
    m_pVoxelrtXml->sensorxml.isDisplay = false;
    m_pVoxelrtXml->sensorxml.isTemperature = true;
    m_pVoxelrtXml->sensorxml.viewAngles = {{0.5, 0}};
    m_pVoxelrtXml->sensorxml.waves = {10500};
    m_pVoxelrtXml->sensorxml.projection = Projection::PARALLAL;
    m_pVoxelrtXml->sensorxml.uavPoses = {glm::vec3(0,1000,0),glm::vec3(100,1000,0),glm::vec3(200,1000,0)};

    m_pVoxelrtXml->scenexml.background.sceneSize={2888, 2890, 10}; // length, width, height
    m_pVoxelrtXml->scenexml.background.sceneOrigin={0, 0, 0};
/*    m_pVoxelrtXml->scenexml.background.sMin={0,0,0};
    m_pVoxelrtXml->scenexml.background.sMax={600,600,10};*/
    m_pVoxelrtXml->scenexml.background.stepsize_surface=1;
    m_pVoxelrtXml->scenexml.background.stepsize_height=1;
//    m_pVoxelrtXml->scenexml.stepsize_atmosphere = 1000;


    m_pVoxelrtXml->scenexml.background.bgSpectralName = "soil";
    m_pVoxelrtXml->scenexml.background.bgThermalName = "K310";
    m_pVoxelrtXml->scenexml.background.bgPropName = "soilset";
    m_pVoxelrtXml->scenexml.background.isDEM = {false};
    m_pVoxelrtXml->scenexml.background.DEMFile = "";
    m_pVoxelrtXml->scenexml.background.lat = 40;
    m_pVoxelrtXml->scenexml.background.lon = 120;


    PrimEntity primEntity1;
    primEntity1= PrimEntity{"building",{"wall","roof"},{"wall","roof"},{"K300","K300"},
                            {"crown","crown"},{"soilset","soilset"},Type::BUILDING,
                            {ShapeType::ELLIPSOID,5,5,5,glm::vec3(0,0,0)},
        false," ",
        true,"F:\\work\\field_aoyunlst\\field_data\\aoyun\\beijing_zhong_height_normal.tif"};

    PrimEntity primEntity2;
    primEntity2 = PrimEntity{"tree",{"crown"},{"leaf"},{"K300"},
                             {"crown"},{"leafbio"},Type::VEGETATION,
                             {ShapeType::ELLIPSOID,10,10,10,glm::vec3(0,0,0)},false,
                             " ", false,
                             "",true,"F:\\work\\field_aoyunlst\\field_data\\aoyun\\entity_0_position.txt"};


    m_pVoxelrtXml->scenexml.objEntities = {};
    m_pVoxelrtXml->scenexml.primEntities = {primEntity1};



    SpectralXml spectralXml1;
    spectralXml1.spectralName="soil";
    spectralXml1.type = spectralType::CUSTOM;
    spectralXml1.reflectances = {0.05};
    spectralXml1.transmittance ={0};
    spectralXml1.path = "F:\\work\\field_aoyunlst\\field\\defined\\soilnew.txt";
    spectralXml1.tau_tir = 0;
    spectralXml1.refl_tir = 0.05;

    SpectralXml spectralXml3;
    spectralXml3.spectralName="wall";
    spectralXml3.type = spectralType::CUSTOM;
    spectralXml3.reflectances = {0.05};
    spectralXml3.transmittance ={0};
    spectralXml3.path = "F:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_tar_asphalt.txt";
//    spectralXml3.path = "F:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_concrete_cement_solid.txt";
    spectralXml3.tau_tir = 0;
    spectralXml3.refl_tir = 0.05;

    SpectralXml spectralXml4;
    spectralXml4.spectralName="roof";
    spectralXml4.type = spectralType::CUSTOM;
    spectralXml4.reflectances = {0.05};
    spectralXml4.transmittance ={0};
    spectralXml4.path = "F:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_tar_asphalt.txt";
//    spectralXml4.path = "D:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_concrete_cement_solid.txt";
    spectralXml4.tau_tir = 0;
    spectralXml4.refl_tir = 0.05;

    SpectralXml spectralXml2{};
    spectralXml2.spectralName="leaf";
    spectralXml2.type = spectralType::CUSTOM;
    spectralXml2.reflectances = {0.025};
    spectralXml2.transmittance ={0};
    spectralXml2.fp = {80,0.009,0.012,0,1.4};
    spectralXml2.tau_tir = 0;
    spectralXml2.refl_tir = 0.05;

    m_pVoxelrtXml->spectralxmls = {spectralXml1, spectralXml2, spectralXml3, spectralXml4};


    m_pVoxelrtXml->thermalxmls = {{"K310", 320, 300}, {"K300", 305, 295}};


//    PropertyXml propertyXml1,propertyXml2,propertyXml3;
//    propertyXml1 = {"soilset",Type::SOIL};
//    propertyXml1.soilset = SoilSet{1,500,1180,1800,1.55,0.25,25,0.45};
//
//    propertyXml2 = {"leafbio",Type::VEGETATION};
//    propertyXml2.leafbio = LeafBio{25,8,0.01,4,0.6396,0.025,{0.2,0.3,288,313,328},25,0.4,0,1,1,0};
//
//    propertyXml3 = {"buildup",Type::BUILDING};
//    propertyXml3.buildup = BuildUp{1,500,1180,1800,1.55,25};
//
//
//    m_pVoxelrtXml->propxmls ={propertyXml1, propertyXml2};

    m_pVoxelrtXml->canopyxmls ={{"crown", {1, 1, 1, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};

    //m_pVoxelrtXml->canopyxmls ={{"crown", {1, 1, 1, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
}



// void XmlExamples::createVoxelebxml(){
//     m_pVoxelebXml=std::make_shared<VoxelEBXml>();
//
//     // 时间序列案例
//     std::string caseName = "timeSeries";    /////// 多时相垂直观测模拟
//     // caseName = "multiangle";                /////// 多角度观测模拟
//     // caseName = "forest";                    /////// 相同LAI情况下，不同植被覆盖度的模拟
//     // caseName = "satellite";              /////// 均质场景下的卫星尺度模拟
//     // caseName = "forest_satellite";       /////// 离散森林场景下的静止卫星多角度模拟
//     // caseName = "forest_TRGMEB";          /////// TRGMEB比较模拟
//     // caseName = "diff_season";
//     // caseName = "diff_height";
//     // caseName = "diff_LAI";
//     // caseName = "diff_SMC";
//     // caseName = "diff_wind";
//
//     if (caseName == "timeSeries")
//     {
//         int LAI = 1;                ////// 1,2,3
//         m_pVoxelebXml->projectDir = "D:/data/field_data/Sim_homo_LAI_" + std::to_string(LAI) + ".0_timeSeries//";
//
//         if (LAI == 1)
//         {
//             m_pVoxelebXml->canopyxmls ={{"crown", {1.0, 0.333, 3, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//             m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 1.0, 0.2}, "", 1000};
//         }
//         if (LAI == 2)
//         {
//             m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 0.667, 3, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//             m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 2.0, 0.2}, "", 1000};
//         }
//         if(LAI == 3)
//         {
//             m_pVoxelebXml->canopyxmls ={{"crown", {3.0, 1, 3, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//             m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 3.0, 0.2}, "", 1000};
//         }
//
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->settingxml.n_sample=32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         m_pVoxelebXml->meteoxml.startTimeNode = 0;
//         m_pVoxelebXml->meteoxml.endTimeNode = 144;
//         m_pVoxelebXml->meteoxml.meta.dTime = 1800;
//
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={15, 15, 30}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//     /*    m_pVoxelebXml->scenexml.background.sMin={0,0,0};
//         m_pVoxelebXml->scenexml.background.sMax={600,600,10};*/
//         m_pVoxelebXml->scenexml.background.stepsize_surface=0.3;
//         m_pVoxelebXml->scenexml.background.stepsize_height=0.3;
//         // m_pVoxelebXml->scenexml.background.stepsize_surface=1;
//         // m_pVoxelebXml->scenexml.background.stepsize_height=1;
//     //    m_pVoxelebXml->scenexml.stepsize_atmosphere = 1000;
//
//
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::CUBE,3,15,15,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//
//
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//
//         PropertyXml propertyXml1,propertyXml2,propertyXml3;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,0.25,25,0.45};
//
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//
//         propertyXml3 = {"buildup",Type::BUILDING};
//         propertyXml3.buildup = BuildUp{1,500,1180,1800,1.55,25};
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Meteo\15m_meteo_corrected_sameWind.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  "D:\\code\\field\\defined\\Esky_scope.dat";
//         m_pVoxelebXml->atomcondxml.rinfile =  "D:\\code\\field\\defined\\Esun_scope.dat";
//     }
//
//     if (caseName == "multiangle")
//    {
//        int LAI = 3;
//        if (LAI == 1)
//        {
//            m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_LAI_1.0_multiangle\)";
//        }
//        if (LAI == 2)
//        {
//            m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_LAI_2.0_multiangle\)";
//        }
//        if (LAI == 3)
//        {
//            m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_LAI_3.0_multiangle\)";
//        }
//
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->settingxml.n_sample= 32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         // m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//        std::string vzaFileName = "D:\\data\\field_data\\Sim_homo_LAI_1.0\\results_multiangle\\angles_Hemi.txt";
//        float  *vza, *vaa;
//        int num = 1;
//        // wave_ = Utils::infile2num(predifineDir+'Esk', 0, 0, num);
//        vza = Utils::readascfile(vzaFileName, 0, 0, num);
//        vaa = Utils::readascfile(vzaFileName, 0, 1, num);
//        for(int i = 0; i < num; i = i + 1){
//            m_pVoxelebXml->sensorxml.viewAngles.emplace_back(vza[i], vaa[i]);
//        }
//
//
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={50, 50, 50}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//     /*    m_pVoxelebXml->scenexml.background.sMin={0,0,0};
//         m_pVoxelebXml->scenexml.background.sMax={600,600,10};*/
//         m_pVoxelebXml->scenexml.background.stepsize_surface=0.5;
//         m_pVoxelebXml->scenexml.background.stepsize_height=0.75;
//         // m_pVoxelebXml->scenexml.background.stepsize_surface=1;
//         // m_pVoxelebXml->scenexml.background.stepsize_height=1;
//     //    m_pVoxelebXml->scenexml.stepsize_atmosphere = 1000;
//
//
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//
//         PrimEntity primEntity1;
//         primEntity1= PrimEntity{"building",
//                                 {"wall","roof"},
//                                 {"wall","roof"},
//                                 {"K300","K300"},
//                                 {"crown","crown"},
//                                 {"soilset","soilset"},
//                                 Type::BUILDING,
//                                 {ShapeType::ELLIPSOID,5,5,5,glm::vec3(0,0,0)},
//                                 false," ",
//                                 true,m_pVoxelebXml->projectDir + "\\height_0_5.tif",
//                                 false, " "};
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::CUBE,3,20,20,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//
//
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//
//         SpectralXml spectralXml3;
//         spectralXml3.spectralName="wall";
//         spectralXml3.type = spectralType::OTHER;
//         spectralXml3.reflectances = {0.05};
//         spectralXml3.transmittance ={0};
//         spectralXml3.path =  m_pVoxelebXml->definedDir + "\\defined\\VNIR_construction_tar_asphalt.txt";
//     //    spectralXml3.path = "F:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_concrete_cement_solid.txt";
//         spectralXml3.tau_tir = 0;
//         spectralXml3.refl_tir = 0.05;
//
//         SpectralXml spectralXml4;
//         spectralXml4.spectralName="roof";
//         spectralXml4.type = spectralType::OTHER;
//         spectralXml4.reflectances = {0.05};
//         spectralXml4.transmittance ={0};
//         spectralXml4.path =  m_pVoxelebXml->definedDir + "\\defined\\VNIR_construction_tar_asphalt.txt";
//     //    spectralXml4.path = "D:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_concrete_cement_solid.txt";
//         spectralXml4.tau_tir = 0;
//         spectralXml4.refl_tir = 0.05;
//
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//
//         PropertyXml propertyXml1,propertyXml2,propertyXml3;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{1,2000,1180,1800,1.55,0.25,25,0.45};
//
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//
//         propertyXml3 = {"buildup",Type::BUILDING};
//         propertyXml3.buildup = BuildUp{1,500,1180,1800,1.55,25};
//
//
//
//
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         // m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 0.666, 3, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//         // m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 2.0, 0.2}, "", 1000};
//
//        if (LAI == 1)
//        {
//            m_pVoxelebXml->canopyxmls ={{"crown", {1.0, 0.333, 3, 1, 0.5, -0.35, -0.15, 0.03, 0.2}}};
//            m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 1.0, 0.2}, "", 1000};
//        }
//        if (LAI == 2)
//        {
//            m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 0.666, 3, 1, 0.5, -0.35, -0.15, 0.11, 0.2}}};
//            m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 2.0, 0.2}, "", 1000};
//        }
//        if(LAI == 3)
//        {
//            m_pVoxelebXml->canopyxmls ={{"crown", {3.0, 1, 3, 1, 0.5, -0.35, -0.15, 0.03, 0.2}}};
//            m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 3.0, 0.2}, "", 1000};
//        }
//
//         // m_pVoxelebXml->canopyxmls ={{"crown", {1.0, 0.333, 3, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//         // m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 1.0, 0.2}, "", 1000};
//         // m_pVoxelebXml->canopyxmls ={{"crown", {3.0, 1, 3, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//         // m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 3.0, 0.2}, "", 1000};
//
//        // m_pVoxelebXml->canopyxmls ={{"crown", {5.0, 1, 5, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//        // m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 5, 12, 5.0, 0.2}, "", 1000};
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Meteo\15m_meteo_corrected_sameWind.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  "D:\\code\\field\\defined\\Esky_scope.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esky_scope.dat";
//         m_pVoxelebXml->atomcondxml.rinfile =  "D:\\code\\field\\defined\\Esun_scope.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esun_scope.dat";
//         //   m_pVoxelebXml->meteoxml.aerocond = {10,10,3,0.36};
//    }
//
//     if (caseName == "forest")
//     {
//         int lai = 3; ////// 1,2,3
//         std::string sceneDes =  "sparse";
//         sceneDes = "normal";
//         sceneDes = "dense";
//
//         m_pVoxelebXml->projectDir = "D:/data/field_data/Sim_homo_LAI_" + std::to_string(lai) + ".0_forest_" + sceneDes + "/";
//         if (lai == 1)
//         {
//             if (sceneDes == "sparse"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {3.1, 1, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             if (sceneDes == "normal"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {1.55, 0.5, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             if (sceneDes == "dense"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {0.78, 0.25, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 6, 12, 1.0, 0.2}, "", 1000};
//         }
//         if (lai == 2)
//         {
//             if (sceneDes == "sparse"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {6.2, 2, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             if (sceneDes == "normal"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {3.1, 1, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             if (sceneDes == "dense"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {1.55, 0.5, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 6, 12, 2.0, 0.2}, "", 1000};
//         }
//         if (lai == 3)
//         {
//             if (sceneDes == "sparse"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {12.4, 4, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             if (sceneDes == "normal"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {6.2, 2, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             if (sceneDes == "dense"){
//                 m_pVoxelebXml->canopyxmls ={{"crown", {3.1, 1, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//             }
//             m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 6, 12, 3.0, 0.2}, "", 1000};
//         }
//
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->settingxml.n_sample= 32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         // m_pVoxelebXml->settingxml.startTimeNode = 0;
//         // m_pVoxelebXml->settingxml.endTimeNode = 144;
//         // m_pVoxelebXml->settingxml.dTime = 600;
//         m_pVoxelebXml->meteoxml.startTimeNode = 0;
//         m_pVoxelebXml->meteoxml.endTimeNode = 144;
//         m_pVoxelebXml->meteoxml.meta.dTime = 600;
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//
//
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={50, 50, 50}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//     /*    m_pVoxelebXml->scenexml.background.sMin={0,0,0};
//         m_pVoxelebXml->scenexml.background.sMax={600,600,10};*/
//         m_pVoxelebXml->scenexml.background.stepsize_surface=0.5;
//         m_pVoxelebXml->scenexml.background.stepsize_height=0.75;
//
//
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::ELLIPSOID,6,3,3,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//
//         PropertyXml propertyXml1,propertyXml2,propertyXml3;
//         propertyXml1 = {"soilset",Type::SOIL};
//         // propertyXml1.soilset = SoilSet{1,2000,1180,1800,1.55,0.25,25,0.45};
//         propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25, 0.25,0.45};
//
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Meteo\15m_meteo_corrected_sameWind.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  "D:\\code\\field\\defined\\Esky_scope.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esky_scope.dat";
//         m_pVoxelebXml->atomcondxml.rinfile =  "D:\\code\\field\\defined\\Esun_scope.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esun_scope.dat";
//         //   m_pVoxelebXml->meteoxml.aerocond = {10,10,3,0.36};
//     }
//
//     if (caseName == "forest_TRGMEB")
//     {
//         int LAI = 2; //4
//         std::string sceneDes =  "TRGMEB";
//         m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_LAI_2.0_forest_normal_TRGMEB\)";
//
//
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->settingxml.n_sample= 32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         m_pVoxelebXml->meteoxml.startTimeNode = 0;
//         m_pVoxelebXml->meteoxml.endTimeNode = 240;
//         m_pVoxelebXml->meteoxml.meta.dTime = 1800;
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//
//
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={20, 20, 20}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//     /*    m_pVoxelebXml->scenexml.background.sMin={0,0,0};
//         m_pVoxelebXml->scenexml.background.sMax={600,600,10};*/
//         m_pVoxelebXml->scenexml.background.stepsize_surface=0.5;
//         m_pVoxelebXml->scenexml.background.stepsize_height=0.5;
//         // m_pVoxelebXml->scenexml.background.stepsize_surface=1;
//         // m_pVoxelebXml->scenexml.background.stepsize_height=1;
//     //    m_pVoxelebXml->scenexml.stepsize_atmosphere = 1000;
//
//
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::ELLIPSOID,6,3,3,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         // spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_medium.txt)";
//         spectralXml1.refl_tir = 0.04;
//
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//
//         PropertyXml propertyXml1,propertyXml2,propertyXml3;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         m_pVoxelebXml->canopyxmls ={{"crown", {6.2, 2, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.5}}};
//
//         m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 6, 12, 2.1, 0.5}, "", 1000};
//
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_LAI_2.0_forest_normal_TRGMEB\Meteo\meteo_TRGMEB.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  "D:\\code\\field\\defined\\Esky_.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esky_scope.dat";
//         m_pVoxelebXml->atomcondxml.rinfile =  "D:\\code\\field\\defined\\Esun_.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esun_scope.dat";
//         //   m_pVoxelebXml->meteoxml.aerocond = {10,10,3,0.36};
//     }
//
//     if (caseName == "satellite")
//     {
//         int LAI = 2;
//         m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_LAI_2.0_timeSeries_satellite\)";            // 模拟静止卫星观测
//
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->settingxml.n_sample=32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}, {20, 0},{40, 0}, {60,0},
//                                                     {0, 45}, {20, 45}, {40, 45}, {60,45},
//                                                     {0, 90}, {20, 90}, {40, 90}, {60,90},
//                                                     {0, 135}, {20, 135}, {40, 135}, {60,135},
//                                                     {0, 180}, {20, 180}, {40, 180}, {60,180},};
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={100, 100, 50}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//     /*    m_pVoxelebXml->scenexml.background.sMin={0,0,0};
//         m_pVoxelebXml->scenexml.background.sMax={600,600,10};*/
//         m_pVoxelebXml->scenexml.background.stepsize_surface=0.75;
//         m_pVoxelebXml->scenexml.background.stepsize_height=0.75;
//         // m_pVoxelebXml->scenexml.background.stepsize_surface=1;
//         // m_pVoxelebXml->scenexml.background.stepsize_height=1;
//     //    m_pVoxelebXml->scenexml.stepsize_atmosphere = 1000;
//
//
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//
//         PrimEntity primEntity1;
//         primEntity1= PrimEntity{"building",
//                                 {"wall","roof"},
//                                 {"wall","roof"},
//                                 {"K300","K300"},
//                                 {"crown","crown"},
//                                 {"soilset","soilset"},
//                                 Type::BUILDING,
//                                 {ShapeType::ELLIPSOID,5,5,5,glm::vec3(0,0,0)},
//                                 false," ",
//                                 true,m_pVoxelebXml->projectDir + "\\height_0_5.tif",
//                                 false, " "};
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::CUBE,3,50,50,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//
//
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//
//         SpectralXml spectralXml3;
//         spectralXml3.spectralName="wall";
//         spectralXml3.type = spectralType::OTHER;
//         spectralXml3.reflectances = {0.05};
//         spectralXml3.transmittance ={0};
//         spectralXml3.path =  m_pVoxelebXml->definedDir + "\\defined\\VNIR_construction_tar_asphalt.txt";
//     //    spectralXml3.path = "F:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_concrete_cement_solid.txt";
//         spectralXml3.tau_tir = 0;
//         spectralXml3.refl_tir = 0.05;
//
//         SpectralXml spectralXml4;
//         spectralXml4.spectralName="roof";
//         spectralXml4.type = spectralType::OTHER;
//         spectralXml4.reflectances = {0.05};
//         spectralXml4.transmittance ={0};
//         spectralXml4.path =  m_pVoxelebXml->definedDir + "\\defined\\VNIR_construction_tar_asphalt.txt";
//     //    spectralXml4.path = "D:\\work\\field_aoyunlst\\field\\defined\\VNIR_construction_concrete_cement_solid.txt";
//         spectralXml4.tau_tir = 0;
//         spectralXml4.refl_tir = 0.05;
//
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//
//         PropertyXml propertyXml1,propertyXml2,propertyXml3;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{1,2000,1180,1800,1.55,0.25,25,0.45};
//
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//
//         propertyXml3 = {"buildup",Type::BUILDING};
//         propertyXml3.buildup = BuildUp{1,500,1180,1800,1.55,25};
//
//
//
//
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//
//         m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 0.666, 3, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//         m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 3, 12, 2.0, 0.2}, "", 1000};
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Meteo\15m_meteo_corrected_sameWind.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  "D:\\code\\field\\defined\\Esky_scope.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esky_scope.dat";
//         m_pVoxelebXml->atomcondxml.rinfile =  "D:\\code\\field\\defined\\Esun_scope.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esun_scope.dat";
//         //   m_pVoxelebXml->meteoxml.aerocond = {10,10,3,0.36};
//     }
//
//     if (caseName == "forest_satellite")
//     {
//         std::string sceneDes =  "sparse";
//         sceneDes = "normal";
//         // sceneDes = "dense";
//
//        if (sceneDes ==  "sparse")
//        {
//            m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_LAI_2.0_forest_sparse_satellite\)";
//        }
//        if (sceneDes == "normal")
//        {
//            m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_LAI_2.0_forest_normal_satellite\)";
//        }
//        if (sceneDes == "dense")
//        {
//            m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_LAI_2.0_forest_dense_satellite\\)";
//        }
//
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->settingxml.n_sample= 32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}, {20, 0},{40, 0}, {60,0},
//                                                     {0, 45}, {20, 45}, {40, 45}, {60,45},
//                                                     {0, 90}, {20, 90}, {40, 90}, {60,90},
//                                                     {0, 135}, {20, 135}, {40, 135}, {60,135},
//                                                     {0, 180}, {20, 180}, {40, 180}, {60,180},};
//         // m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//        // std::string vzaFileName = "D:\\data\\field_data\\Sim_homo_LAI_1.0\\results_multiangle\\angles_Hemi.txt";
//        // float  *vza, *vaa;
//        // int num = 1;
//        // // wave_ = Utils::infile2num(predifineDir+'Esk', 0, 0, num);
//        // vza = Utils::readascfile(vzaFileName, 0, 0, num);
//        // vaa = Utils::readascfile(vzaFileName, 0, 1, num);
//        // for(int i = 0; i < num; i = i + 1){
//        //     m_pVoxelebXml->sensorxml.viewAngles.emplace_back(vza[i], vaa[i]);
//        // }
//
//
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={50, 50, 50}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//     /*    m_pVoxelebXml->scenexml.background.sMin={0,0,0};
//         m_pVoxelebXml->scenexml.background.sMax={600,600,10};*/
//         m_pVoxelebXml->scenexml.background.stepsize_surface=0.5;
//         m_pVoxelebXml->scenexml.background.stepsize_height=0.75;
//         // m_pVoxelebXml->scenexml.background.stepsize_surface=1;
//         // m_pVoxelebXml->scenexml.background.stepsize_height=1;
//     //    m_pVoxelebXml->scenexml.stepsize_atmosphere = 1000;
//
//
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::ELLIPSOID,6,3,3,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//
//         PropertyXml propertyXml1,propertyXml2,propertyXml3;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{1,2000,1180,1800,1.55,0.25,25,0.45};
//
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         if (sceneDes == "sparse"){
//             m_pVoxelebXml->canopyxmls ={{"crown", {8.0, 2, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//         }
//         if (sceneDes == "normal"){
//             m_pVoxelebXml->canopyxmls ={{"crown", {4.0, 1, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//         }
//         if (sceneDes == "dense"){
//             m_pVoxelebXml->canopyxmls ={{"crown", {2, 0.5, 6, 1, 0.5, -0.35, -0.15, 0.1, 0.2}}};
//         }
//
//         m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 6, 12, 2.0, 0.2}, "", 1000};
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Meteo\15m_meteo_corrected_sameWind.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  "D:\\code\\field\\defined\\Esky_scope.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esky_scope.dat";
//         m_pVoxelebXml->atomcondxml.rinfile =  "D:\\code\\field\\defined\\Esun_scope.dat";//m_pVoxelebXml->definedDir + "\\defined\\Esun_scope.dat";
//     }
//
//     if (caseName == "diff_season")
//     {
//
//         m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_season\Height_2_LAI_2_C3_80_diff_season\)";
//         // m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_season\Height_2_LAI_2_C3_80_diff_season_shaded_soil\)";
//         // m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_season\Height_2_LAI_2_C3_80_diff_season_shaded_veg\)";
//         // m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_season\Height_2_LAI_2_C3_80_diff_season_voxelSize_0.25\)";
//         // m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_season\Height_2_LAI_2_C3_80_diff_season_voxelSize_1\)";
//
//
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->meteoxml.startTimeNode = 0;
//         m_pVoxelebXml->meteoxml.endTimeNode = 576;
//         m_pVoxelebXml->meteoxml.meta.dTime = 1800;
//
//         m_pVoxelebXml->settingxml.n_sample= 32;//32
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={20, 20, 10}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//         m_pVoxelebXml->scenexml.background.stepsize_surface= 0.5;//0.5
//         m_pVoxelebXml->scenexml.background.stepsize_height= 0.5;
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         //15
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::CUBE,2,15,15,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//         PropertyXml propertyXml1,propertyXml2;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 1, 2, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//         m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 2, 12, 2.0, 0.2}, "", 1000};
//
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_season\meteo_diff_season_HL.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  R"(D:\code\field\defined\Esky_scope.dat)";
//         m_pVoxelebXml->atomcondxml.rinfile =  R"(D:\code\field\defined\Esun_scope.dat)";
//     }
//
//     if (caseName == "diff_height")
//     {
//         // int height = 2;//1,2,4,6
//         // m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_canopy_height\LAI_2_C3_80_summer_Height_)" + std::to_string(height) + "//";
//         // m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//         //
//         // m_pVoxelebXml->settingxml.startTimeNode = 0;
//         // m_pVoxelebXml->settingxml.endTimeNode = 576;
//         // m_pVoxelebXml->settingxml.dTime = 1800;
//         // m_pVoxelebXml->settingxml.n_sample= 32;
//         // m_pVoxelebXml->settingxml.maxDepth = 32;
//         // m_pVoxelebXml->settingxml.theGPU = 0;
//         //
//         // m_pVoxelebXml->lightxml.name = "Solar";
//         // m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         // m_pVoxelebXml->lightxml.direct = 0.9;
//         // m_pVoxelebXml->lightxml.diffuse = 0.1;
//         // m_pVoxelebXml->lightxml.skyTemperature = 250;
//         // m_pVoxelebXml->lightxml.solarTemperature = 6000;
//         //
//         // m_pVoxelebXml->sensorxml.name = "UAV";
//         // m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         // m_pVoxelebXml->sensorxml.isImage = true;
//         // m_pVoxelebXml->sensorxml.isAlbedo = false;
//         // m_pVoxelebXml->sensorxml.isDisplay = false;
//         // m_pVoxelebXml->sensorxml.isTemperature = true;
//         // m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         // m_pVoxelebXml->sensorxml.waves = {10500};
//         // m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//         //
//         // m_pVoxelebXml->scenexml.background.sceneSize={20, 20, 10}; // length, width, height
//         // m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//         // m_pVoxelebXml->scenexml.background.stepsize_surface= 0.5;
//         // m_pVoxelebXml->scenexml.background.stepsize_height= 0.5;
//         // m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         // m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         // m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         // m_pVoxelebXml->scenexml.background.isDEM = {false};
//         // m_pVoxelebXml->scenexml.background.DEMFile = "";
//         // m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         // m_pVoxelebXml->scenexml.background.lon = 115.7923;
//         //
//         // PrimEntity primEntity2;
//         // primEntity2 = PrimEntity{"tree",
//         //                          {"crown"},
//         //                          {"leaf"},
//         //                          {"K300"},
//         //                        {"crown"},
//         //                        {"leafbio"},
//         //                        Type::VEGETATION,
//         //                        {ShapeType::CUBE,2,15,15,glm::vec3(0,0,0)},
//         //                        false,"",
//         //                        false,"",
//         //                        true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         // m_pVoxelebXml->scenexml.objEntities = {};
//         // m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//         //
//         // SpectralXml spectralXml1;
//         // spectralXml1.spectralName="soil";
//         // spectralXml1.type = spectralType::OTHER;
//         // spectralXml1.reflectances = {0.04};
//         // spectralXml1.transmittance ={0};
//         // spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         // spectralXml1.tau_tir = 0;
//         // spectralXml1.refl_tir = 0.04;
//         // SpectralXml spectralXml2{};
//         // spectralXml2.spectralName="leaf";
//         // spectralXml2.type = spectralType::PROSPECT;
//         // spectralXml2.reflectances = {0.02};
//         // spectralXml2.transmittance ={0};
//         // spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         // spectralXml2.tau_tir = 0;
//         // spectralXml2.refl_tir = 0.02;
//         // m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//         //
//         // m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//         //                               {"K300", 305, 295}};
//         //
//         // PropertyXml propertyXml1,propertyXml2;
//         // propertyXml1 = {"soilset",Type::SOIL};
//         // propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//         // propertyXml2 = {"leafbio",Type::VEGETATION};
//         // propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//         //     {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//         // m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//         //
//         // m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 1, 2, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//         //
//         // m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 2, 12, 2.0, 0.2}, "", 1000};
//         //
//         //
//         // m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_canopy_height\meteo_diff_season_HL.txt)";
//         // m_pVoxelebXml->atomcondxml.rlifile =  R"(D:\code\field\defined\Esky_scope.dat)";
//         // m_pVoxelebXml->atomcondxml.rinfile =  R"(D:\code\field\defined\Esun_scope.dat)";
//
//         // int height = 4;//1,2,4,6
//         // m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_canopy_height\LAI_2_C3_80_summer_Height_)" + std::to_string(height) + "//";
//         // m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//         //
//         // m_pVoxelebXml->settingxml.startTimeNode = 0;
//         // m_pVoxelebXml->settingxml.endTimeNode = 576;
//         // m_pVoxelebXml->settingxml.dTime = 1800;
//         // m_pVoxelebXml->settingxml.n_sample= 32;
//         // m_pVoxelebXml->settingxml.maxDepth = 32;
//         // m_pVoxelebXml->settingxml.theGPU = 0;
//         //
//         // m_pVoxelebXml->lightxml.name = "Solar";
//         // m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         // m_pVoxelebXml->lightxml.direct = 0.9;
//         // m_pVoxelebXml->lightxml.diffuse = 0.1;
//         // m_pVoxelebXml->lightxml.skyTemperature = 250;
//         // m_pVoxelebXml->lightxml.solarTemperature = 6000;
//         //
//         // m_pVoxelebXml->sensorxml.name = "UAV";
//         // m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         // m_pVoxelebXml->sensorxml.isImage = true;
//         // m_pVoxelebXml->sensorxml.isAlbedo = false;
//         // m_pVoxelebXml->sensorxml.isDisplay = false;
//         // m_pVoxelebXml->sensorxml.isTemperature = true;
//         // m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         // m_pVoxelebXml->sensorxml.waves = {10500};
//         // m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//         //
//         // m_pVoxelebXml->scenexml.background.sceneSize={40, 40, 10}; // length, width, height
//         // m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//         // m_pVoxelebXml->scenexml.background.stepsize_surface= 1;
//         // m_pVoxelebXml->scenexml.background.stepsize_height= 1;
//         // m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         // m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         // m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         // m_pVoxelebXml->scenexml.background.isDEM = {false};
//         // m_pVoxelebXml->scenexml.background.DEMFile = "";
//         // m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         // m_pVoxelebXml->scenexml.background.lon = 115.7923;
//         //
//         // PrimEntity primEntity2;
//         // primEntity2 = PrimEntity{"tree",
//         //                          {"crown"},
//         //                          {"leaf"},
//         //                          {"K300"},
//         //                        {"crown"},
//         //                        {"leafbio"},
//         //                        Type::VEGETATION,
//         //                        {ShapeType::CUBE,4,30,30,glm::vec3(0,0,0)},
//         //                        false,"",
//         //                        false,"",
//         //                        true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         // m_pVoxelebXml->scenexml.objEntities = {};
//         // m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//         //
//         // SpectralXml spectralXml1;
//         // spectralXml1.spectralName="soil";
//         // spectralXml1.type = spectralType::OTHER;
//         // spectralXml1.reflectances = {0.04};
//         // spectralXml1.transmittance ={0};
//         // spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         // spectralXml1.tau_tir = 0;
//         // spectralXml1.refl_tir = 0.04;
//         // SpectralXml spectralXml2{};
//         // spectralXml2.spectralName="leaf";
//         // spectralXml2.type = spectralType::PROSPECT;
//         // spectralXml2.reflectances = {0.02};
//         // spectralXml2.transmittance ={0};
//         // spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         // spectralXml2.tau_tir = 0;
//         // spectralXml2.refl_tir = 0.02;
//         // m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//         //
//         // m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//         //                               {"K300", 305, 295}};
//         //
//         // PropertyXml propertyXml1,propertyXml2;
//         // propertyXml1 = {"soilset",Type::SOIL};
//         // propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//         // propertyXml2 = {"leafbio",Type::VEGETATION};
//         // propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//         //     {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//         // m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//         //
//         // m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 0.5, 4, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//         //
//         // m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 4, 12, 2.0, 0.2}, "", 1000};
//         //
//         //
//         // m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_canopy_height\meteo_diff_season_HL.txt)";
//         // m_pVoxelebXml->atomcondxml.rlifile =  R"(D:\code\field\defined\Esky_scope.dat)";
//         // m_pVoxelebXml->atomcondxml.rinfile =  R"(D:\code\field\defined\Esun_scope.dat)";
//
//
//         // int height = 6;//1,2,4,6
//         // m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_canopy_height\LAI_2_C3_80_summer_Height_)" + std::to_string(height) + "//";
//         // m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//         //
//         // m_pVoxelebXml->settingxml.startTimeNode = 0;
//         // m_pVoxelebXml->settingxml.endTimeNode = 576;
//         // m_pVoxelebXml->settingxml.dTime = 1800;
//         // m_pVoxelebXml->settingxml.n_sample= 32;
//         // m_pVoxelebXml->settingxml.maxDepth = 32;
//         // m_pVoxelebXml->settingxml.theGPU = 0;
//         //
//         // m_pVoxelebXml->lightxml.name = "Solar";
//         // m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         // m_pVoxelebXml->lightxml.direct = 0.9;
//         // m_pVoxelebXml->lightxml.diffuse = 0.1;
//         // m_pVoxelebXml->lightxml.skyTemperature = 250;
//         // m_pVoxelebXml->lightxml.solarTemperature = 6000;
//         //
//         // m_pVoxelebXml->sensorxml.name = "UAV";
//         // m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         // m_pVoxelebXml->sensorxml.isImage = true;
//         // m_pVoxelebXml->sensorxml.isAlbedo = false;
//         // m_pVoxelebXml->sensorxml.isDisplay = false;
//         // m_pVoxelebXml->sensorxml.isTemperature = true;
//         // m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         // m_pVoxelebXml->sensorxml.waves = {10500};
//         // m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//         //
//         // m_pVoxelebXml->scenexml.background.sceneSize={60, 60, 10}; // length, width, height
//         // m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//         // m_pVoxelebXml->scenexml.background.stepsize_surface= 1.5;
//         // m_pVoxelebXml->scenexml.background.stepsize_height= 1.5;
//         // m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         // m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         // m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         // m_pVoxelebXml->scenexml.background.isDEM = {false};
//         // m_pVoxelebXml->scenexml.background.DEMFile = "";
//         // m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         // m_pVoxelebXml->scenexml.background.lon = 115.7923;
//         //
//         // PrimEntity primEntity2;
//         // primEntity2 = PrimEntity{"tree",
//         //                          {"crown"},
//         //                          {"leaf"},
//         //                          {"K300"},
//         //                        {"crown"},
//         //                        {"leafbio"},
//         //                        Type::VEGETATION,
//         //                        {ShapeType::CUBE,6,45,45,glm::vec3(0,0,0)},
//         //                        false,"",
//         //                        false,"",
//         //                        true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         // m_pVoxelebXml->scenexml.objEntities = {};
//         // m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//         //
//         // SpectralXml spectralXml1;
//         // spectralXml1.spectralName="soil";
//         // spectralXml1.type = spectralType::OTHER;
//         // spectralXml1.reflectances = {0.04};
//         // spectralXml1.transmittance ={0};
//         // spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         // spectralXml1.tau_tir = 0;
//         // spectralXml1.refl_tir = 0.04;
//         // SpectralXml spectralXml2{};
//         // spectralXml2.spectralName="leaf";
//         // spectralXml2.type = spectralType::PROSPECT;
//         // spectralXml2.reflectances = {0.02};
//         // spectralXml2.transmittance ={0};
//         // spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         // spectralXml2.tau_tir = 0;
//         // spectralXml2.refl_tir = 0.02;
//         // m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//         //
//         // m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//         //                               {"K300", 305, 295}};
//         //
//         // PropertyXml propertyXml1,propertyXml2;
//         // propertyXml1 = {"soilset",Type::SOIL};
//         // propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//         // propertyXml2 = {"leafbio",Type::VEGETATION};
//         // propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//         //     {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//         // m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//         //
//         // m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 0.333, 6, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//         //
//         // m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 6, 12, 2.0, 0.2}, "", 1000};
//         //
//         //
//         // m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_canopy_height\meteo_diff_season_HL.txt)";
//         // m_pVoxelebXml->atomcondxml.rlifile =  R"(D:\code\field\defined\Esky_scope.dat)";
//         // m_pVoxelebXml->atomcondxml.rinfile =  R"(D:\code\field\defined\Esun_scope.dat)";
//
//
//         int height = 1;//1,2,4,6
//         m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_canopy_height\LAI_2_C3_80_summer_Height_)" + std::to_string(height) + "//";
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->meteoxml.startTimeNode = 0;
//         m_pVoxelebXml->meteoxml.endTimeNode = 576;
//         m_pVoxelebXml->meteoxml.meta.dTime = 1800;
//
//         m_pVoxelebXml->settingxml.n_sample= 32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={10, 10, 10}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//         m_pVoxelebXml->scenexml.background.stepsize_surface= 0.25;
//         m_pVoxelebXml->scenexml.background.stepsize_height= 0.25;
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::CUBE,1,7.5,7.5,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//         PropertyXml propertyXml1,propertyXml2;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 2, 1, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//
//         m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 1, 12, 2.0, 0.2}, "", 1000};
//
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_canopy_height\meteo_diff_season_HL.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  R"(D:\code\field\defined\Esky_scope.dat)";
//         m_pVoxelebXml->atomcondxml.rinfile =  R"(D:\code\field\defined\Esun_scope.dat)";
//     }
//
//     if (caseName == "diff_LAI")
//     {
//         int LAI = 6;//1,2,4,6
//         m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_LAI\C3_80_summer_Height_2_LAI_)" + std::to_string(LAI) + "//";
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->meteoxml.startTimeNode = 0;
//         m_pVoxelebXml->meteoxml.endTimeNode = 576;
//         m_pVoxelebXml->meteoxml.meta.dTime = 1800;
//
//         m_pVoxelebXml->settingxml.n_sample= 32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={10, 10, 10}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//         m_pVoxelebXml->scenexml.background.stepsize_surface= 0.5;
//         m_pVoxelebXml->scenexml.background.stepsize_height= 0.5;
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::CUBE,2,10,10,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//         PropertyXml propertyXml1,propertyXml2;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 1, 2, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//         m_pVoxelebXml->canopyxmls[0].canopy.density = LAI / 2.0;
//         m_pVoxelebXml->canopyxmls[0].canopy.lai = LAI;
//
//         m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 2, 12, 2.0, 0.2}, "", 1000};
//         m_pVoxelebXml->aerocondxml.aerocond.lai = LAI;
//
//
//         // m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_LAI\meteo_summer_HL.txt)";
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_LAI\meteo_diff_season_HL.txt)";
//
//         m_pVoxelebXml->atomcondxml.rlifile =  R"(D:\code\field\defined\Esky_scope.dat)";
//         m_pVoxelebXml->atomcondxml.rinfile =  R"(D:\code\field\defined\Esun_scope.dat)";
//     }
//
//     if (caseName == "diff_SMC")
//     {
//          int SMC = 35;//15,25,35
//         m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_SMC\LAI_2_Height_2_C3_80_summer_SMC_)" + std::to_string(SMC) + "//";
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->meteoxml.startTimeNode = 0;
//         m_pVoxelebXml->meteoxml.endTimeNode = 576;
//         m_pVoxelebXml->meteoxml.meta.dTime = 1800;
//
//         m_pVoxelebXml->settingxml.n_sample= 32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={10, 10, 10}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//         m_pVoxelebXml->scenexml.background.stepsize_surface= 0.5;
//         m_pVoxelebXml->scenexml.background.stepsize_height= 0.5;
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::CUBE,2,10,10,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//         if (SMC == 15)
//         {
//             spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//             spectralXml1.tau_tir = 0;
//             spectralXml1.refl_tir = 0.04;
//         }
//         if (SMC==25)
//         {
//             spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_medium.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//             spectralXml1.tau_tir = 0;
//             spectralXml1.refl_tir = 0.03;
//         }
//         if (SMC == 35)
//         {
//             spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_low.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//             spectralXml1.tau_tir = 0;
//             spectralXml1.refl_tir = 0.02;
//         }
//
//
//
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//         PropertyXml propertyXml1,propertyXml2;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//         if (SMC == 15)
//         {
//             propertyXml1.soilset.rss = 3283;
//         }
//         if (SMC == 25)
//         {
//             propertyXml1.soilset.rss = 919;
//         }
//         if (SMC == 35)
//         {
//             propertyXml1.soilset.rss = 104;
//         }
//
//
//
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 1, 2, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//
//         m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 2, 12, 2.0, 0.2}, "", 1000};
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_SMC\meteo_diff_season_HL.txt)";
//         m_pVoxelebXml->atomcondxml.rlifile =  R"(D:\code\field\defined\Esky_scope.dat)";
//         m_pVoxelebXml->atomcondxml.rinfile =  R"(D:\code\field\defined\Esun_scope.dat)";
//     }
//
//     if (caseName == "diff_wind")
//     {
//          int windSpeed = 6;//2,4,6
//         m_pVoxelebXml->projectDir = R"(D:\data\field_data\Sim_homo_diff_wind_speed\Height_2_LAI_2_C3_80_diff_wind_speed_)" + std::to_string(windSpeed) + "//";
//         m_pVoxelebXml->definedDir = R"(D:\code\filed_newest)";
//
//         m_pVoxelebXml->meteoxml.startTimeNode = 0;
//         m_pVoxelebXml->meteoxml.endTimeNode = 576;
//         m_pVoxelebXml->meteoxml.meta.dTime = 1800;
//
//         m_pVoxelebXml->settingxml.n_sample= 32;
//         m_pVoxelebXml->settingxml.maxDepth = 32;
//         m_pVoxelebXml->settingxml.theGPU = 0;
//
//         m_pVoxelebXml->lightxml.name = "Solar";
//         m_pVoxelebXml->lightxml.solarAngle = {40, 30};
//         m_pVoxelebXml->lightxml.direct = 0.9;
//         m_pVoxelebXml->lightxml.diffuse = 0.1;
//         m_pVoxelebXml->lightxml.skyTemperature = 250;
//         m_pVoxelebXml->lightxml.solarTemperature = 6000;
//
//         m_pVoxelebXml->sensorxml.name = "UAV";
//         m_pVoxelebXml->sensorxml.resolution = {500, 500};
//         m_pVoxelebXml->sensorxml.isImage = true;
//         m_pVoxelebXml->sensorxml.isAlbedo = false;
//         m_pVoxelebXml->sensorxml.isDisplay = false;
//         m_pVoxelebXml->sensorxml.isTemperature = true;
//         m_pVoxelebXml->sensorxml.viewAngles = {{0, 0}};
//         m_pVoxelebXml->sensorxml.waves = {10500};
//         m_pVoxelebXml->sensorxml.projection = Projection::PARALLAL;
//
//         m_pVoxelebXml->scenexml.background.sceneSize={10, 10, 10}; // length, width, height
//         m_pVoxelebXml->scenexml.background.sceneOrigin={0, 0, 0};
//         m_pVoxelebXml->scenexml.background.stepsize_surface= 0.5;
//         m_pVoxelebXml->scenexml.background.stepsize_height= 0.5;
//         m_pVoxelebXml->scenexml.background.bgSpectralName = "soil";
//         m_pVoxelebXml->scenexml.background.bgThermalName = "K310";
//         m_pVoxelebXml->scenexml.background.bgPropName = "soilset";
//         m_pVoxelebXml->scenexml.background.isDEM = {false};
//         m_pVoxelebXml->scenexml.background.DEMFile = "";
//         m_pVoxelebXml->scenexml.background.lat = 40.3574;
//         m_pVoxelebXml->scenexml.background.lon = 115.7923;
//
//         PrimEntity primEntity2;
//         primEntity2 = PrimEntity{"tree",
//                                  {"crown"},
//                                  {"leaf"},
//                                  {"K300"},
//                                {"crown"},
//                                {"leafbio"},
//                                Type::VEGETATION,
//                                {ShapeType::CUBE,2,10,10,glm::vec3(0,0,0)},
//                                false,"",
//                                false,"",
//                                true,m_pVoxelebXml->projectDir + "\\entity_0_position.txt"};
//         m_pVoxelebXml->scenexml.objEntities = {};
//         m_pVoxelebXml->scenexml.primEntities = {primEntity2};
//
//         SpectralXml spectralXml1;
//         spectralXml1.spectralName="soil";
//         spectralXml1.type = spectralType::OTHER;
//         spectralXml1.reflectances = {0.04};
//         spectralXml1.transmittance ={0};
//
//         spectralXml1.path = R"(D:\data\field_data\Sim_homo_LAI_0.0_timeSeries\Spectral\\soilnew_high.txt)";// m_pVoxelebXml->definedDir + "\\defined\\soilnew_high.txt";
//         spectralXml1.tau_tir = 0;
//         spectralXml1.refl_tir = 0.04;
//
//
//         SpectralXml spectralXml2{};
//         spectralXml2.spectralName="leaf";
//         spectralXml2.type = spectralType::PROSPECT;
//         spectralXml2.reflectances = {0.02};
//         spectralXml2.transmittance ={0};
//         spectralXml2.fp = {80,0.009,0.012,0,1.46};
//         spectralXml2.tau_tir = 0;
//         spectralXml2.refl_tir = 0.02;
//         m_pVoxelebXml->spectralxmls = {spectralXml1, spectralXml2};
//
//         m_pVoxelebXml->thermalxmls = {{"K310", 320, 300},
//                                       {"K300", 305, 295}};
//
//         PropertyXml propertyXml1,propertyXml2;
//         propertyXml1 = {"soilset",Type::SOIL};
//         propertyXml1.soilset = SoilSet{0,2000,1180,1800,1.55,25,0.25,0.45};
//
//         propertyXml2 = {"leafbio",Type::VEGETATION};
//         propertyXml2.leafbio = LeafBio{80,9,0.01,3,0.6396,0.015,
//             {0.2,0.3,288,313,328},25,0.507,0,1,1,0};
//         m_pVoxelebXml->propxmls ={propertyXml1, propertyXml2};
//
//         m_pVoxelebXml->canopyxmls ={{"crown", {2.0, 1, 2, 1, 0.5, -0.35, -0.15, 0.2, 0.2}}};
//
//         m_pVoxelebXml->aerocondxml = {AeroType::ONE, {0, 0, 10, 2, 12, 2.0, 0.2}, "", 1000};
//
//         m_pVoxelebXml->meteoxml.meteofile =  R"(D:\data\field_data\Sim_homo_diff_wind_speed\meteo_diff_season_HL_wind_speed_)"  + std::to_string(windSpeed) + ".txt";
//         m_pVoxelebXml->atomcondxml.rlifile =  R"(D:\code\field\defined\Esky_scope.dat)";
//         m_pVoxelebXml->atomcondxml.rinfile =  R"(D:\code\field\defined\Esun_scope.dat)";
//     }
//
//
// }

