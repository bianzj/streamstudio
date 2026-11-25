// ==========================================================================
// 文件：utils.cpp
// 功能：Utils 命名空间的通用工具实现：文本数值表读取、GDAL 栅格读写、字符串/路径处理、
//       指数积分 E1、儒略历相关的 ΔT 与日序换算。声明见 utils.h。
// 主要模块：
//   1. 数值表解析：readNumericRows / readPositionFile / readascfileinout / infile2num
//   2. 路径与字符串：getDirectoryPath / getFileName / splitt / findnum / getMapIndex
//   3. GDAL 栅格读取：readImageinout1（单波段）/ readImageinout（多波段）/ readImage
//   4. GDAL 栅格写出：saveImage1（单波段）/ saveImage（多波段），均写 Float32 GTiff
//   5. 数学函数：expint（指数积分 E1，供 PROSPECT 叶片光学模型使用）
//   6. 时间函数：calculateDeltaT（ΔT 多项式）/ isLeapYear / calculateMonthAndDay
// 数据流：场景/气象/地形等文件 → 本文件读入为 float 数组 → 各模式引擎（上传 GPU 或 CPU 计算）；
//         结果影像经 saveImage* 写回 GTiff。
// 约定：栅格数据按行优先存放，下标 = 行 * width + 列；读取一律转换为 float。
// 注意：多处直接用 std::cout 输出错误并以返回 0 表示失败，调用方需检查返回值；
//       saveImage/saveImage1 的 proj、trans 参数当前未被使用（输出不含地理参考；这两个函数目前无调用点）；
//       写入的 RasterIO 失败时返回 0。
// ==========================================================================

#include "utils.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>



namespace {
// 模块：数值表文本解析的内部辅助函数（匿名命名空间）。
// tableContent：去掉 '#' 之后的内容与首尾空白；以 "//" 开头的整行视为注释，返回空串。
std::string tableContent(std::string line)
{
    const size_t comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    const char* blanks = " \t\r\n\v\f";
    const size_t first = line.find_first_not_of(blanks);
    if (first == std::string::npos) return {};
    const size_t last = line.find_last_not_of(blanks);
    line = line.substr(first, last - first + 1);
    if (line.compare(0, 2, "//") == 0) return {};
    return line;
}

// 作用：把一行按空格/制表符/逗号切分并逐字段用 strtof 解析为 float。
// 返回：全部字段均为有限数值且至少一个字段时为 true；任何字段无法完整解析（含 nan/inf）即 false。
// 副作用：先清空 values。
bool parseNumericFields(const std::string& line, std::vector<float>& values)
{
    values.clear();
    const char* separators = " \t,";
    size_t begin = line.find_first_not_of(separators);
    while (begin != std::string::npos) {
        const size_t end = line.find_first_of(separators, begin);
        const std::string token = line.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        char* parsedEnd = nullptr;
        const float value = std::strtof(token.c_str(), &parsedEnd);
        if (parsedEnd == token.c_str() || *parsedEnd != '\0' || !std::isfinite(value)) return false;
        values.push_back(value);
        if (end == std::string::npos) break;
        begin = line.find_first_not_of(separators, end);
    }
    return !values.empty();
}
}

// 作用：读取数值表文本文件，每个有效行返回一个 float 向量。
// 输入：path 文件路径；description 用于报错信息；输出 skippedRows 为含非数值字段而被跳过的行数。
// 说明：空行与注释行（# 或行首 //）不计入 skippedRows；会去掉文件开头的 UTF-8 BOM；
//       无法打开文件时抛 std::runtime_error。
std::vector<std::vector<float>> Utils::readNumericRows(const std::string& path,
                                                       const std::string& description,
                                                       size_t& skippedRows)
{
    // DEFERRED[中] 安全: 窄字符路径打开 ifstream，Windows 上按 ANSI 代码页解析，UTF-8/代码页外的中文路径会打开失败（本文件其余 ifstream 同理）；建议：统一用 std::filesystem::u8path(path) 或宽字符路径构造流。
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open " + description + ": " + path);
    std::vector<std::vector<float>> rows;
    std::vector<float> values;
    skippedRows = 0;
    std::string line;
    bool firstLine = true;
    while (std::getline(input, line)) {
        // 步骤：仅首行检查并剔除 UTF-8 BOM，避免 BOM 使第一个数值解析失败。
        if (firstLine && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        firstLine = false;
        const std::string content = tableContent(line);
        if (content.empty()) continue;
        if (!parseNumericFields(content, values)) {
            ++skippedRows;
            continue;
        }
        rows.push_back(values);
    }
    return rows;
}

// 作用：读取对象实例位置文件，每行格式 "x y z [scale] [rotation]"。
// 说明：缺省 scale=1、rotation=0（PositionRecord 默认值）；少于 3 列，或给出 scale 但 scale<=0（含 NaN）
//       的行被丢弃并计入警告；没有任何有效实例时抛异常。rotation 单位（待核对，按调用方约定）。
std::vector<Utils::PositionRecord> Utils::readPositionFile(const std::string& path)
{
    size_t skipped = 0;
    const auto rows = readNumericRows(path, "object position file", skipped);
    std::vector<PositionRecord> records;
    records.reserve(rows.size());
    for (const auto& row : rows) {
        if (row.size() < 3 || (row.size() > 3 && !(row[3] > 0.0f))) {
            ++skipped;
            continue;
        }
        PositionRecord record;
        record.x = row[0];
        record.y = row[1];
        record.z = row[2];
        if (row.size() > 3) record.scale = row[3];
        if (row.size() > 4) record.rotation = row[4];
        records.push_back(record);
    }
    if (skipped > 0) {
        std::cerr << "Warning: skipped " << skipped
                  << " invalid row(s) in object position file " << path << std::endl;
    }
    if (records.empty())
        throw std::runtime_error("Object position file contains no valid instance: " + path);
    return records;
}


// 作用：读取以空格分隔的文本文件的第 col 列（0 起）到 data，先跳过前 skip 行。
// 返回：打开文件成功为 1，否则为 0（此时 num=0、data 为空）。
// 说明：num 是第一遍扫描中“至少有 1 个字段的行数”，而 data 仅收集字段数 > col 的行，
//       两者可能不相等；字段用 atof 转换，无法解析时得到 0。
int Utils::readascfileinout(std::string infileName, int skip, int col, std::vector<float> &data, int &num)
{
    std::string line;
    std::vector <std::string> fields;
    std::string deli(" ");
    data.clear();

    std::ifstream infile(infileName.c_str());
    if (infile.is_open())
    {
        num = 0;
        if (skip != 0)
        {
            for (int i = 0; i < skip; i++) std::getline(infile, line);
        }
        while (std::getline(infile, line))
        {
            fields = Utils::splitt(line, deli);
            if (int(fields.size()) >= 1)
            {
                num++;
            }
        }
    }
    else
    {
        std::cout << "Unable to open the file: " << infileName << std::endl;
        num = 0;
        data.clear();
        infile.close();
        return 0;
    }
    infile.close();

//    float* mydata = new float[num];

    data.clear();
    if (num >= 1)
    {
        int jj = 0;
        std::ifstream infilee(infileName.c_str());
//        mydata = new float[num];
        //getline(infilee,line);

        if (skip != 0)
        {
            for (int i = 0; i < skip; i++) std::getline(infilee, line);
        }
        while (std::getline(infilee, line))
        {
            fields = Utils::splitt(line, deli);
            if (int(fields.size()) > col)
            {
//                mydata[jj] = atof(fields[col].c_str());
                data.push_back(atof(fields[col].c_str()));
                jj = jj + 1;
            }
        }
        infilee.close();
    }

    return 1;
}



// 作用：返回最后一个 '/' 或 '\' 之前的目录部分（不含分隔符）；无分隔符时返回空串。
std::string Utils::getDirectoryPath(const std::string& fullPath)
{
    size_t lastSlash = fullPath.find_last_of("/\\");
    if(lastSlash!=std::string::npos)
    {
        return fullPath.substr(0,lastSlash);
    }else{
        return "";
    }
}

// 作用：返回最后一个 '/' 或 '\' 之后的文件名；无分隔符时返回原字符串。
std::string Utils::getFileName(const std::string& fullPath)
{
    size_t lastSlash = fullPath.find_last_of("/\\");
    if(lastSlash!=std::string::npos)
    {
        return fullPath.substr(lastSlash+1);
    }else{
        return fullPath;
    }
}

// 作用：用 GDAL 读取栅格影像的第 1 个波段，转换为 float 并按行优先存入 collected。
// 输出：width、height、nband 为影像尺寸与波段总数（其余波段不读取）。
// 返回：成功 1；文件无法打开、尺寸/波段数无效、内存或读取失败返回 0。
// 说明：GDAL_FILENAME_IS_UTF8=NO 表示文件名按本地编码而非 UTF-8 传给 GDAL；
//       collected 被整体覆盖（assign）。
int Utils::readImageinout1(std::string infilename,std::vector<float> &collected,
                           int &width, int &height, int &nband) {


    GDALAllRegister();
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");

    GDALDataset *poDataset;
    GDALDriver *poDriver;


    poDataset = (GDALDataset *) GDALOpen(infilename.c_str(), GA_ReadOnly);//
    if (poDataset == NULL) {
        std::cout << "指定的文件不能打开!" << std::endl;
        return 0;
    }
    width = poDataset->GetRasterXSize();          //获取影像信息
    height = poDataset->GetRasterYSize();
    nband = poDataset->GetRasterCount();

    if (nband < 1 || width <= 0 || height <= 0) {
        std::cout << "影像波段数或尺寸无效!" << std::endl;
        GDALClose(poDataset);
        return 0;
    }
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    GDALRasterBand * poBand = poDataset->GetRasterBand(1);
    if (poBand == nullptr) {
        GDALClose(poDataset);
        return 0;
    }
    GDALDataType gBand = poBand->GetRasterDataType();
    int nBits = GDALGetDataTypeSize(gBand);


    //std::vector<std::vector<float>> collected(nband);

    float *bandData = (float *)CPLMalloc(sizeof(float)*pixelCount);
    if (bandData == nullptr) {
        GDALClose(poDataset);
        return 0;
    }
    CPLErr result = poBand->RasterIO(GF_Read,0,0,width,height,bandData,width,height,GDT_Float32,0,0);
    if (result != CE_None) {
        CPLFree(bandData);
        GDALClose(poDataset);
        return 0;
    }

    collected.assign(bandData,bandData+pixelCount);
    CPLFree(bandData);


    // std::cout<<"123"<<std::endl;
//    double geoTransform[6];                       //获取坐标信息
//    poDataset->GetGeoTransform(geoTransform);
//    const char *spatialRef = poDataset->GetProjectionRef();  //获取投影信息

    GDALClose(poDataset);

    return 1;
}

// 作用：用 GDAL 读取影像全部波段为 float，每个波段一个向量（行优先，大小 width*height）。
// 返回：成功 1，失败 0（打开失败、尺寸无效、像素数溢出、读取失败）。
// 说明：与 readImageinout1 一致，开始时先清空 collected（此前是直接 push_back 追加，会混入旧数据），
//       读取失败时 collected 可能保留已读出的部分波段。
int Utils::readImageinout(std::string infilename,std::vector<std::vector<float>> &collected,
                          int &width, int &height, int &nband) {

    collected.clear();


    GDALAllRegister();
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");

    GDALDataset *poDataset;
    GDALDriver *poDriver;


    poDataset = (GDALDataset *) GDALOpen(infilename.c_str(), GA_ReadOnly);//
    if (poDataset == NULL) {
        std::cout << "指定的文件不能打开!" << std::endl;
        return 0;
    }
    width = poDataset->GetRasterXSize();          //获取影像信息
    height = poDataset->GetRasterYSize();
    nband = poDataset->GetRasterCount();

    if (nband < 1 || width <= 0 || height <= 0) {
        std::cout << "影像波段数或尺寸无效!" << std::endl;
        GDALClose(poDataset);
        return 0;
    }
    GDALRasterBand * poBand = poDataset->GetRasterBand(1);
    if (poBand == nullptr) {
        GDALClose(poDataset);
        return 0;
    }
    GDALDataType gBand = poBand->GetRasterDataType();
    int nBits = GDALGetDataTypeSize(gBand);
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixelCount / static_cast<size_t>(width) != static_cast<size_t>(height)) {
        std::cout << "影像像素数溢出!" << std::endl;
        GDALClose(poDataset);
        return 0;
    }

    //std::vector<std::vector<float>> collected(nband);
    for(int kband=1;kband < nband+1;kband++)
    {
//        GDALRasterBand * band = poDataset->GetRasterBand(kband);
//        float *bandData = new float(width*height);
//        band->RasterIO(GF_Read,0,0,width,height,bandData,width,height,GDT_Float32,0,0);
//        collected[kband-1].assign(bandData,bandData+width*height);
//        delete [] bandData;

        poBand = poDataset->GetRasterBand(kband);
        if (poBand == nullptr) {
            GDALClose(poDataset);
            return 0;
        }
        float *bandData = (float *)CPLMalloc(sizeof(float)*pixelCount);
        if (bandData == nullptr) {
            GDALClose(poDataset);
            return 0;
        }
        CPLErr result = poBand->RasterIO(GF_Read,0,0,width,height,bandData,width,height,GDT_Float32,0,0);
        if (result != CE_None) {
            CPLFree(bandData);
            GDALClose(poDataset);
            return 0;
        }
        std::vector<float> tempcollected;
        tempcollected.assign(bandData,bandData+pixelCount);
        collected.push_back(tempcollected);
        CPLFree(bandData);

    }

    double geoTransform[6];                       //获取坐标信息
    poDataset->GetGeoTransform(geoTransform);
    const char *spatialRef = poDataset->GetProjectionRef();  //获取投影信息

    GDALClose(poDataset);

    return 1;
}

// 作用：与 readImageinout 类似，返回全部波段（每波段一个 float 向量）；失败时返回空结果。
std::vector<std::vector<float>> Utils::readImage(std::string infilename) {


    GDALAllRegister();
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");

    GDALDataset *poDataset;
    GDALDriver *poDriver;


    poDataset = (GDALDataset *) GDALOpen(infilename.c_str(), GA_ReadOnly);//
    if (poDataset == NULL) {
        std::cout << "指定的文件不能打开!" << std::endl;
        return std::vector<std::vector<float>>();
    }
    int width = poDataset->GetRasterXSize();          //获取影像信息
    int height = poDataset->GetRasterYSize();
    int nBands = poDataset->GetRasterCount();

    if (nBands < 1 || width <= 0 || height <= 0) {
        std::cout << "影像波段数或尺寸无效!" << std::endl;
        GDALClose(poDataset);
        return std::vector<std::vector<float>>();
    }
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixelCount / static_cast<size_t>(width) != static_cast<size_t>(height)) {
        std::cout << "影像像素数溢出!" << std::endl;
        GDALClose(poDataset);
        return std::vector<std::vector<float>>();
    }

    GDALRasterBand * poBand = poDataset->GetRasterBand(1);
    if (poBand == nullptr) {
        GDALClose(poDataset);
        return std::vector<std::vector<float>>();
    }
    GDALDataType gBand = poBand->GetRasterDataType();
    int nBits = GDALGetDataTypeSize(gBand);


    std::vector<std::vector<float>> collected(nBands);
    for(int kband=1;kband <= nBands;kband++)
    {
//        GDALRasterBand * band = poDataset->GetRasterBand(kband);
//        float *bandData = new float(width*height);
//        band->RasterIO(GF_Read,0,0,width,height,bandData,width,height,GDT_Float32,0,0);
//        collected[kband-1].assign(bandData,bandData+width*height);
//        delete [] bandData;

        poBand = poDataset->GetRasterBand(kband);
        if (poBand == nullptr) {
            GDALClose(poDataset);
            return std::vector<std::vector<float>>();
        }
        float *bandData = (float *)CPLMalloc(sizeof(float)*pixelCount);
        if (bandData == nullptr) {
            GDALClose(poDataset);
            return std::vector<std::vector<float>>();
        }
        CPLErr result = poBand->RasterIO(GF_Read,0,0,width,height,bandData,width,height,GDT_Float32,0,0);
        if (result != CE_None) {
            CPLFree(bandData);
            GDALClose(poDataset);
            return std::vector<std::vector<float>>();
        }
        collected[kband-1].assign(bandData,bandData+pixelCount);
        CPLFree(bandData);
    }

    double geoTransform[6];                       //获取坐标信息
    poDataset->GetGeoTransform(geoTransform);
    const char *spatialRef = poDataset->GetProjectionRef();  //获取投影信息

    GDALClose(poDataset);

    return collected;
}


// 作用：把单个 float 数组写成单波段 Float32 GTiff。
// 输入：c 为 width*height 的行优先数据（长度不足则失败）；band、proj、trans 当前未使用，
//       因此输出文件不含地理参考信息（本函数目前没有调用点；输出为无地理参考的普通 GTiff，
//       如需地理参考应在此用 SetGeoTransform/SetProjection 写入 trans、proj）。
// 返回：成功 1；GTiff 驱动缺失、尺寸不符、创建失败或 RasterIO 写入失败返回 0。
int Utils::saveImage1(std::string outfilepath, std::vector<float> &c,
                     int width, int height,int band, std::string proj, double trans[6]) {

    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");	// 支持中文路径
    GDALAllRegister();  //注册所有的驱动

//    GDALDataset *poDataset;   //GDAL数据集
//    GDALRasterBand* poBand = poDataset->GetRasterBand(band);
//    GDALDataType type = poBand->GetRasterDataType();

    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (driver == nullptr) {
        std::cout << "GDAL GTiff driver unavailable: " << outfilepath << std::endl;
        return 0;
    }
    if (width <= 0 || height <= 0 ||
        c.size() < static_cast<size_t>(width) * static_cast<size_t>(height)) {
        std::cout << "saveImage1: raster size does not match data: " << outfilepath << std::endl;
        return 0;
    }
    GDALDataset *ods = driver->Create(outfilepath.c_str(),width,height,1,GDT_Float32,NULL);
    if (ods == nullptr) {
        std::cout << "GDAL Create failed: " << outfilepath << std::endl;
        return 0;
    }
    GDALRasterBand *oBand = ods->GetRasterBand(1);
    //float *bandData = c.data();
    CPLErr result =oBand->RasterIO(GF_Write,0, 0,width,height,c.data(),width,height,GDT_Float32,0,0);
    if (result != CE_None) {
        std::cout << "saveImage1: RasterIO write failed: " << outfilepath << std::endl;
        GDALClose(ods);
        return 0;
    }

    GDALClose(ods);
    return 1;
}

// 作用：把 band 个 float 数组写成多波段 Float32 GTiff（c[k] 对应第 k+1 个波段）。
// 返回：成功 1；驱动缺失、波段数或每个波段大小与 width*height 不符、创建失败、RasterIO 写入失败返回 0。
// 说明：proj、trans 当前未使用，输出不含地理参考（本函数目前没有调用点，处理方式同 saveImage1）。
int Utils::saveImage(std::string outfilepath, std::vector<std::vector<float>> &c,
                      int width, int height,int band, std::string proj, double trans[6]) {

    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");	// 支持中文路径
    GDALAllRegister();  //注册所有的驱动

//    GDALDataset *poDataset;   //GDAL数据集
//    GDALRasterBand* poBand = poDataset->GetRasterBand(band);
//    GDALDataType type = poBand->GetRasterDataType();

    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (driver == nullptr) {
        std::cout << "GDAL GTiff driver unavailable: " << outfilepath << std::endl;
        return 0;
    }
    if (width <= 0 || height <= 0 || c.size() < static_cast<size_t>(band)) {
        std::cout << "saveImage: invalid raster size or band count: " << outfilepath << std::endl;
        return 0;
    }
    for (int kb = 0; kb < band; kb++) {
        if (c[kb].size() < static_cast<size_t>(width) * static_cast<size_t>(height)) {
            std::cout << "saveImage: band data does not match raster size: " << outfilepath << std::endl;
            return 0;
        }
    }
    GDALDataset *ods = driver->Create(outfilepath.c_str(),width,height,band,GDT_Float32,NULL);
    if (ods == nullptr) {
        std::cout << "GDAL Create failed: " << outfilepath << std::endl;
        return 0;
    }
    for(int kband=0;kband<band;kband++) {
        GDALRasterBand *oBand = ods->GetRasterBand(kband+1);
        //float *bandData = c[kband].data();
        CPLErr result = oBand->RasterIO(GF_Write, 0, 0, width, height, c[kband].data(), width, height, GDT_Float32, 0, 0);
        if (result != CE_None) {
            std::cout << "saveImage: RasterIO write failed (band " << (kband + 1) << "): " << outfilepath << std::endl;
            GDALClose(ods);
            return 0;
        }
    }
    GDALClose(ods);
    return 1;
}

// 作用：按 deli 中的任一字符切分 s，连续分隔符与首尾空字段被跳过。
// 例：以 " " 切分 "a  b" 得 {a, b}。
std::vector<std::string> Utils::splitt(std::string& s, std::string& deli)
{
    // Delimiters are a set of characters; retain the existing empty-field
    // skipping contract without truncating a single-character final token.
    std::vector<std::string> fields;
    size_t first = s.find_first_not_of(deli);
    while (first != std::string::npos)
    {
        const size_t end = s.find_first_of(deli, first);
        fields.push_back(s.substr(first, end == std::string::npos ? end : end - first));
        if (end == std::string::npos) break;
        first = s.find_first_not_of(deli, end);
    }
    return fields;
}

// 作用：指数积分 E1(x) = ∫_1^∞ exp(-x t)/t dt，内部用 double 计算。
// 说明：x <= 0（或 NaN）返回 +inf；x <= 1 用幂级数，x > 1 用 Lentz 连分式（均最多 200 项），
//       收敛判据见各循环内的 eps。算法选择的原因见函数内的原注释。
float Utils::expint(float x)
{
	// Exponential integral E1(x) = int_1^inf exp(-x t)/t dt (MATLAB expint),
	// used by PROSPECT. The former left-rectangle quadrature on t in [1,100)
	// cost ~1e5 exp() per call (2e8 per PROSPECT material, ~10 s of VoxelEB
	// start-up) and dropped the E1(100x) tail (5 % error at x = 0.01).
	// Standard evaluation: power series for x <= 1, Lentz continued fraction
	// for x > 1 (Numerical Recipes expint, n = 1), relative error ~1e-12.
	const double v = static_cast<double>(x);
	if (!(v > 0.0)) return std::numeric_limits<float>::infinity();
	constexpr double euler = 0.57721566490153286061;
	constexpr double eps = 1.0e-15;
	// 步骤 1：x <= 1 用幂级数 E1 = -γ - ln x - Σ (-x)^k/(k·k!)，γ 为欧拉常数。
	if (v <= 1.0) {
		double sum = 0.0;
		double term = 1.0;
		for (int k = 1; k < 200; ++k) {
			term *= -v / k;               // (-x)^k / k!
			const double delta = term / k;
			sum += delta;
			if (std::abs(delta) < eps * std::max(1.0, std::abs(sum))) break;
		}
		return static_cast<float>(-euler - std::log(v) - sum);
	}
	// 步骤 2：x > 1 用修正 Lentz 连分式求 E1 = exp(-x)·h；c 的初值取极大数以避免除零。
	double b = v + 1.0;
	double c = 1.0 / 1.0e-300;
	double d = 1.0 / b;
	double h = d;
	for (int i = 1; i < 200; ++i) {
		const double a = -static_cast<double>(i) * i;
		b += 2.0;
		d = 1.0 / (a * d + b);
		c = b + a / c;
		const double delta = c * d;
		h *= delta;
		if (std::abs(delta - 1.0) < eps) break;
	}
	return static_cast<float>(h * std::exp(-v));
}

// 作用：与 readascfileinout 类似，读取空格分隔文本第 col 列（0 起）为 new[] 分配的 float 数组。
// 输入：skip 为跳过的行数；输出 num 为“至少含 1 个字段的行数”（即数组长度）。
// 返回：文件无法打开或 num <= 0 时返回 nullptr；否则调用方负责 delete[]。
// 说明：字段数不足 col+1 的行不写入，数组尾部保持 0（new float[num]() 零初始化）。
float* Utils::infile2num(std::string infileName, int skip, int col, int& num)
{
	std::string line;
	std::vector <std::string> fields;
	std::string deli(" ");



	std::ifstream infile(infileName.c_str());
	if (infile.is_open())
	{
		num = 0;
		if (skip != 0)
		{
			for (int i = 0; i < skip; i++) std::getline(infile, line);
		}
		while (std::getline(infile, line))
		{
			fields = Utils::splitt(line, deli);
			if (int(fields.size()) >= 1)
			{
				num++;
			}
		}
	}
	else
	{
		std::cout << "Unable to open the file: " << infileName << std::endl;
		num = 0;
		infile.close();
		return nullptr;
	}
	infile.close();

	// Rows shorter than col are counted but not written; keep them at zero.
	if (num <= 0) return nullptr;
	float* mydata = new float[num]();
	if (num >= 1)
	{

		int jj = 0;
		std::ifstream infilee(infileName.c_str());
		//getline(infilee,line);

		if (skip != 0)
		{
			for (int i = 0; i < skip; i++) std::getline(infilee, line);
		}
		while (std::getline(infilee, line) && jj < num)
		{
			fields = Utils::splitt(line, deli);
			if (int(fields.size()) > col)
			{
				mydata[jj] = atof(fields[col].c_str());
				jj = jj + 1;
			}
		}
		infilee.close();
	}

	return mydata;
}

// 作用：按名称在 maps 中查找索引值；找不到时返回 0（调用方无法区分“找不到”与“索引 0”）。
int Utils::getMapIndex(std::map<std::string, int> maps, std::string name)
{
	// DEFERRED[中] 错误处理: 名称不存在时静默返回 0，拼写错误的材质/属性名会被映射到第一个条目，产生错误的光学/热属性而不报错；且 map 按值传入，每次调用整表复制；建议：未找到时抛异常或返回 -1 由调用方处理，参数改为 const 引用。
	int loc = 0;
	std::map<std::string, int>::iterator it;
	it = maps.find(name);
	if (it != maps.end())
		loc = it->second;
	return loc;
	return 0;
}

// 内部辅助（本文件内部链接，不对外导出）：判断字符是否为十进制数字。
static bool isnum(char n)
{
	return (n >= '0' && n <= '9');
}
// 作用：提取字符串中所有连续数字串并依次转为 int（如 "a12b3" → {12, 3}）。
// 说明：不处理负号与小数点；数字串过长超过 int 范围时按 INT_MAX 饱和（不再发生溢出）。
std::vector <int> Utils::findnum(std::string& ch)
{
	std::vector <int> numVec;
	int k = ch.size();
	int result;
	int n = 0;
	// 追加一位十进制数字，超出 int 上限时饱和为 INT_MAX。
	const auto appendDigit = [](int value, char digit) {
		const int d = digit - '0';
		if (value > (std::numeric_limits<int>::max() - d) / 10)
			return std::numeric_limits<int>::max();
		return value * 10 + d;
	};
	while (n < k) {
		result = 0;
		if (isnum(ch[n]))
		{
			result = ch[n] - '0';
			while (n < k && isnum(ch[++n]))
				result = appendDigit(result, ch[n]);
			numVec.push_back(result);
		}
		++n;
	}
	return numVec;
}



// 作用：计算 ΔT = TT - UT（单位 s），按年份分段采用 Espenak & Meeus 多项式。
// 输入：year（年）、month（1..12）；内部用 y = year + (month - 0.5)/12 作为十进制年。
// 说明：分段边界按整数 year 判断，各段多项式的参考年（如 1820、1900、2000）见各分支。
// Espenak & Meeus polynomial expressions (NASA "Polynomial Expressions for Delta T").
double Utils::calculateDeltaT(int year, int month) {
    double deltaT = 0.0;
    const double y = year + (month - 0.5) / 12.0;

    if (year < -500) {
        deltaT = -20.0 + 32.0 * pow((y - 1820) / 100.0, 2);
    } else if (year < 500) {
        deltaT = 10583.6 - 1014.41 * (y / 100.0) + 33.78311 * pow(y / 100.0, 2) - 5.952053 * pow(y / 100.0, 3) - 0.1798452 * pow(y / 100.0, 4) + 0.022174192 * pow(y / 100.0, 5) + 0.0090316521 * pow(y / 100.0, 6);
    } else if (year < 1600) {
        deltaT = 1574.2 - 556.01 * ((y - 1000) / 100.0) + 71.23472 * pow((y - 1000) / 100.0, 2) + 0.319781 * pow((y - 1000) / 100.0, 3) - 0.8503463 * pow((y - 1000) / 100.0, 4) - 0.005050998 * pow((y - 1000) / 100.0, 5) + 0.0083572073 * pow((y - 1000) / 100.0, 6);
    } else if (year < 1700) {
        deltaT = 120.0 - 0.9808 * (y - 1600) - 0.01532 * pow(y - 1600, 2) + pow(y - 1600, 3) / 7129.0;
    } else if (year < 1800) {
        deltaT = 8.83 + 0.1603 * (y - 1700) - 0.0059285 * pow(y - 1700, 2) + 0.00013336 * pow(y - 1700, 3) - pow(y - 1700, 4) / 1174000.0;
    } else if (year < 1860) {
        deltaT = 13.72 - 0.332447 * (y - 1800) + 0.0068612 * pow(y - 1800, 2) + 0.0041116 * pow(y - 1800, 3) - 0.00037436 * pow(y - 1800, 4) + 0.0000121272 * pow(y - 1800, 5) - 0.0000001699 * pow(y - 1800, 6) + 0.000000000875 * pow(y - 1800, 7);
    } else if (year < 1900) {
        deltaT = 7.62 + 0.5737 * (y - 1860) - 0.251754 * pow(y - 1860, 2) + 0.01680668 * pow(y - 1860, 3) - 0.0004473624 * pow(y - 1860, 4) + pow(y - 1860, 5) / 233174.0;
    } else if (year < 1920) {
        deltaT = -2.79 + 1.494119 * (y - 1900) - 0.0598939 * pow(y - 1900, 2) + 0.0061966 * pow(y - 1900, 3) - 0.000197 * pow(y - 1900, 4);
    } else if (year < 1941) {
        deltaT = 21.2 + 0.84493 * (y - 1920) - 0.0761 * pow(y - 1920, 2) + 0.0020936 * pow(y - 1920, 3);
    } else if (year < 1961) {
        deltaT = 29.07 + 0.407 * (y - 1950) - pow(y - 1950, 2) / 233.0 + pow(y - 1950, 3) / 2547.0;
    } else if (year < 1986) {
        deltaT = 45.45 + 1.067 * (y - 1975) - pow(y - 1975, 2) / 260.0 - pow(y - 1975, 3) / 718.0;
    } else if (year < 2005) {
        deltaT = 63.86 + 0.3345 * (y - 2000) - 0.060374 * pow(y - 2000, 2) + 0.0017275 * pow(y - 2000, 3) + 0.000651814 * pow(y - 2000, 4) + 0.00002373599 * pow(y - 2000, 5);
    } else if (year < 2050) {
        deltaT = 62.92 + 0.32217 * (y - 2000) + 0.005589 * pow(y - 2000, 2);
    } else if (year < 2150) {
        deltaT = -20.0 + 32.0 * pow((y - 1820) / 100.0, 2) - 0.5628 * (2150 - y);
    } else {
        deltaT = -20.0 + 32.0 * pow((y - 1820) / 100.0, 2);
    }

    return deltaT;
}

// 作用：格里高利历闰年判断（四年一闰，百年不闰，四百年再闰）。
bool Utils::isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// 作用：把年内日序 kdoy（1 起）换算为月（1..12）和日；闰年 2 月按 29 天。
// 输入越界（平年 >365、闰年 >366 或 <1）时抛 std::runtime_error。
void Utils::calculateMonthAndDay(int kyear, int kdoy, int *kmonth, int *kday) {
    // 各个月的天数（平年）
    int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    // 如果是闰年，2月有29天
    if (isLeapYear(kyear)) {
        daysInMonth[1] = 29;
    }
    if (kdoy < 1 || kdoy > (isLeapYear(kyear) ? 366 : 365)) {
        throw std::runtime_error("Day of year out of range: " + std::to_string(kdoy));
    }
    int dayOfYear = kdoy;
    int month = 0;
    // 找出对应的月
    while (dayOfYear > daysInMonth[month]) {
        dayOfYear -= daysInMonth[month];
        month++;
    }
    // 返回结果
    *kmonth = month + 1;
    *kday = dayOfYear;
}
