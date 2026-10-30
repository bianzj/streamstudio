#include "utils.h"
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>



namespace {
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


int Utils::readascfileinout(std::string infileName, int skip, int col, std::vector<float> &data, int &num)
{
    std::string line;
    std::vector <std::string> fields;
    std::string deli(" ");
    data.clear();

    // FIXED[低] 错误处理: 文件打不开时 num 保持调用方传入的旧值且函数仍返回 1，调用方无法区分"空文件"与"打开失败"；atof 对非数字字段静默返回 0；已修复：打不开时把 num 置 0、data 保持已清空并返回 0，调用方可区分失败（atof 的静默解析仍为 DEFERRED）。
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



std::string Utils::getDirectoryPath(const std::string& fullPath)
{
    // FIXED[中] Bug: 只识别 '/'，Windows 原生路径（C:\a\b.obj）找不到分隔符，返回空目录/整串文件名，相对资源路径解析错误（getFileName 同理）；已修复：两个函数的分隔符集合改为 "/\\"，Windows 原生路径也能正确切分。
    size_t lastSlash = fullPath.find_last_of("/\\");
    if(lastSlash!=std::string::npos)
    {
        return fullPath.substr(0,lastSlash);
    }else{
        return "";
    }
}

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

    // FIXED[中] 内存: nband==0 时 GetRasterBand(1) 返回 nullptr 被解引用；sizeof(float)*width*height 中 width*height 先以 int 相乘，>2^31 像素溢出；RasterIO 返回值未检查，读取失败仍返回 1（readImageinout/readImage 同理）；已修复：nband<1 或尺寸非正时关闭数据集并返回 0（readImageinout 同步加上同样的波段/尺寸判空），像素数与字节数改用 size_t 计算，poBand/CPLMalloc 判空后再使用；RasterIO 的 CPLErr 当时仍未检查（DEFERRED），本轮已在下方 RasterIO 处补齐检查。
    if (nband < 1 || width <= 0 || height <= 0) {
        std::cout << "影像波段数或尺寸无效!" << std::endl;
        GDALClose(poDataset);
        return 0;
    }
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    // FIXED[中] 错误处理: GetRasterBand(1) 在判空之前就被解引用以取 GetRasterDataType（前轮补漏发现，原文件未标注）；已修复：先取出 poBand 并判空，再取数据类型，空指针时 GDALClose 数据集并返回 0（readImageinout/readImage 同改）。
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
    // FIXED[中] 错误处理: RasterIO 的 CPLErr 未检查，读取失败时仍把未初始化的 bandData 当作影像返回 1（前轮补漏发现，原文件未标注）；已修复：检查 != CE_None 时 CPLFree(bandData)、GDALClose 数据集并返回 0。
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

int Utils::readImageinout(std::string infilename,std::vector<std::vector<float>> &collected,
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
    // FIXED[中] 错误处理: GetRasterBand(1) 在判空之前就被解引用以取 GetRasterDataType（前轮补漏发现，原文件未标注）；已修复：先取出 poBand 并判空，再取数据类型，空指针时 GDALClose 数据集并返回 0（readImageinout1/readImage 同改）。
    GDALRasterBand * poBand = poDataset->GetRasterBand(1);
    if (poBand == nullptr) {
        GDALClose(poDataset);
        return 0;
    }
    GDALDataType gBand = poBand->GetRasterDataType();
    int nBits = GDALGetDataTypeSize(gBand);
    // FIXED[中] 内存: sizeof(float)*width*height 与 bandData+width*height 的像素数仍按 int 相乘（>2^31 像素时溢出，分配/拷贝越界）；已修复：像素数改用 size_t 逐项相乘，分配前回除校验，分配字节数与指针偏移均用 pixelCount（readImage 同改）。
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixelCount / static_cast<size_t>(width) != static_cast<size_t>(height)) {
        std::cout << "影像像素数溢出!" << std::endl;
        GDALClose(poDataset);
        return 0;
    }

    // FIXED[中] 错误处理: GetRasterBand(kband)/CPLMalloc 返回值未判空、RasterIO 的 CPLErr 未检查，波段为空或分配/读取失败时解引用空指针或把未初始化数据当作影像返回（前轮补漏发现，原文件未标注）；已修复：三者逐一判空并检查 CE_None，失败时释放已分配的 bandData、GDALClose 数据集并返回 0。
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

    // FIXED[中] 内存: 与 readImageinout 同源的 int 像素数乘法，bandData+width*height 在大图上溢出越界；且本函数缺少波段/尺寸判空（第 207 行注释声称已同步，实际未加）；已修复：补上 nBands<1 或尺寸非正的判空，像素数用 size_t 计算并回除校验，分配与偏移改用 pixelCount。
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

    // FIXED[中] 错误处理: GetRasterBand(1) 在判空之前就被解引用以取 GetRasterDataType（前轮补漏发现，原文件未标注）；已修复：先取出 poBand 并判空，再取数据类型，空指针时 GDALClose 数据集并返回空 vector（readImageinout1/readImageinout 同改）。
    GDALRasterBand * poBand = poDataset->GetRasterBand(1);
    if (poBand == nullptr) {
        GDALClose(poDataset);
        return std::vector<std::vector<float>>();
    }
    GDALDataType gBand = poBand->GetRasterDataType();
    int nBits = GDALGetDataTypeSize(gBand);


    std::vector<std::vector<float>> collected(nBands);
    // FIXED[高] Bug: 循环条件 kband < nBands 少读最后一个波段（GDAL 波段号 1..nBands），collected[nBands-1] 恒为空，单波段影像返回空数据，调用方按 width*height 索引即越界；已修复：循环上界改为 kband <= nBands，最后一个波段也被读取。
    // FIXED[中] 错误处理: GetRasterBand(kband)/CPLMalloc 返回值未判空、RasterIO 的 CPLErr 未检查，波段为空或分配/读取失败时解引用空指针或把未初始化数据当作影像返回（前轮补漏发现，原文件未标注）；已修复：三者逐一判空并检查 CE_None，失败时释放已分配的 bandData、GDALClose 数据集并返回空 vector。
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


int Utils::saveImage1(std::string outfilepath, std::vector<float> &c,
                     int width, int height,int band, std::string proj, double trans[6]) {

    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");	// 支持中文路径
    GDALAllRegister();  //注册所有的驱动

//    GDALDataset *poDataset;   //GDAL数据集
//    GDALRasterBand* poBand = poDataset->GetRasterBand(band);
//    GDALDataType type = poBand->GetRasterDataType();

    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    // FIXED[中] 错误处理: driver/ods 未判空（目录不存在、无写权限、路径编码问题时 Create 返回 nullptr 直接崩溃），未校验 c.size()>=width*height（不足时 RasterIO 越界读），RasterIO 结果被忽略；已修复（saveImage 同改）：driver/ods 判空后返回 0，写入前校验尺寸为正且 c.size()>=width*height；RasterIO 的 CPLErr 与 proj/trans 地理参考仍未使用，保持 DEFERRED。
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

    GDALClose(ods);
    return 1;
}

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

    }
    GDALClose(ods);
    return 1;
}

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

float Utils::expint(float x)
{
	// DEFERRED[中] 数值: 用步长 0.001 的左矩形法在 t∈[1,100) 上积分 E1(x)=∫₁^∞ e^{-xt}/t dt，截断丢掉 E1(100x)，x=0.01 时相对误差约 5%、x=0.001 时约 29%（PROSPECT 近红外弱吸收波段 Kall 正处于此量级），且矩形法有 O(0.0005) 的系统偏差；每次调用 9.9 万次 exp，fluspect 每材质调用 2001 次，约 2×10⁸ 次 exp；建议：使用级数（x<1：-γ-ln x-Σ(-x)^k/(k·k!)）+ 连分式（x≥1）的标准实现。
	int i1 = 1000;
	int i2 = 100000;
	double sum = 0.0, ii;
	for (int i = i1; i < i2; i++)
	{
		ii = i / 1000.0;
		sum = sum + exp(-x * ii) / ii * 0.001;
	}
	return sum;

}

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
	// FIXED[高] 内存: 文件打不开时 num 未被赋值（保持调用方传入的值，可能是未初始化垃圾/负数），new float[num] 可能抛 bad_array_new_length 或分配巨量内存；已修复：打不开时置 num=0 并返回 nullptr，num<=0 时不再分配；返回裸 new[] 的接口改为 std::vector<float> 属 API 变更，保持 DEFERRED。
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

bool isnum(char n)
{
	return (n >= '0' && n <= '9');
}
std::vector <int> Utils::findnum(std::string& ch)
{
	std::vector <int> numVec;
	int k = ch.size();
	// FIXED[低] 内存: new int[k] 从未 delete[]，每次调用泄漏；连续数字串较长时 int result 有符号溢出（UB）；已修复：去掉临时数组，直接 push_back 到 numVec，不再泄漏（超长数字串的 int 溢出仍为 DEFERRED）。
	int result;
	int n = 0;
	while (n < k) {
		result = 0;
		if (isnum(ch[n]))
		{
			result = ch[n] - '0';
			while (n < k && isnum(ch[++n]))
				result = (ch[n] - '0') + 10 * result;
			numVec.push_back(result);
		}
		++n;
	}
	return numVec;
}



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

bool Utils::isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

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
