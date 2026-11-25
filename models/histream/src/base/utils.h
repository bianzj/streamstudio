// ==========================================================================
// 文件：utils.h
// 功能：Utils 命名空间的通用工具函数声明（实现见 utils.cpp），同时汇集 histream 常用的第三方头文件。
// 主要模块：
//   1. 数学与容器辅助：getNextPow2Number、expint、Mat/vector 互转、FormatNumbers
//   2. 文本与路径：splitt、findnum、getMapIndex、readNumericRows、readPositionFile 等
//   3. GDAL 栅格读写：readImage*、saveImage*
//   4. 计时：MilliTimer（基于 nvh::Stopwatch）
//   5. 时间换算：calculateDeltaT、isLeapYear、calculateMonthAndDay
// 关系：被 histream 各模块广泛包含；依赖 Vulkan/nvpro 工具库、OpenCV、GDAL。
// 注意：多数读文件函数以返回值 0/空结果表示失败而非抛异常，调用方需检查（见 utils.cpp）。
// ==========================================================================

#pragma once
#include <vector>
#include <numbers>
#include <string>
#include <iostream>
#include <fstream>
#include <map>
#include <vulkan/vulkan.hpp>
#include "nvvk/resourceallocator_vk.hpp"
#include "nvvk/commands_vk.hpp"
#include <nvvk/images_vk.hpp>
#include "nvmath/nvmath.h"
#include <opencv2/opencv.hpp>
#include "gdal.h"
#include "gdal_priv.h"
#include <chrono>
#include <sstream>
#include <ios>

#include "nvh/nvprint.hpp"
#include "nvh/timesampler.hpp"

// 通用工具函数集合（无状态）。
namespace Utils {

	template<typename T>
	// 作用：返回不小于 number 的最小 2 的幂（经 double 计算后截断为 int）。
	int getNextPow2Number(T number) {
		return std::pow(2, std::ceil(std::log2(number)));
	}

	// 字符串切分：deli 为分隔字符集合，空字段被跳过。
	std::vector <std::string> splitt(std::string& s, std::string& deli);

    // GDAL 读影像：返回每个波段一个 float 向量（行优先）；失败返回空。
    std::vector<std::vector<float>> readImage(std::string infilename);


    // OpenCV Mat 与 std::vector 的互转模板（单通道/按行展开；vector→Mat 会深拷贝）。
    /***************** Mat转vector **********************/
    template<typename _Tp>
    std::vector<_Tp> convertMat2Vector(const cv::Mat &mat)
    {
        return (std::vector<_Tp>)(mat.reshape(1, 1));//通道数不变，按行转为一行
    }

    /****************** vector转Mat *********************/
    template<typename _Tp>
    cv::Mat convertVector2Mat(std::vector<_Tp> v, int channels, int rows)
    {
        cv::Mat mat = cv::Mat(v);//将vector变成单列的mat，这里需要clone(),因为这里的赋值操作是浅拷贝
        cv::Mat dest = mat.reshape(channels, rows).clone();
        return dest;
    }

	// 指数积分 E1(x)（x <= 0 返回 +inf），用于 PROSPECT 叶片光学模型。
	float expint(float x);
	// 读文本第 col 列为 new[] 数组（调用方 delete[]），num 返回长度；失败返回 nullptr。
	float* infile2num(std::string infileName, int skip, int col, int& num);


	// 按名称取索引，找不到返回 0。
	int getMapIndex(std::map<std::string, int> maps, std::string name);



    // GDAL 读栅格系列：成功返回 1，失败返回 0；width/height/nband 为输出。
    //   readImageinout：多波段（c 先清空再按波段依次存入）；readImageinout1：仅第 1 波段（c 被整体覆盖）。
    int readImageinout(std::string infilename,std::vector<std::vector<float>> &c,
                       int &width, int &height, int &nband);

    int readImageinout1(std::string infilename,std::vector<float> &c,
                        int &width, int &height, int &nband);

    // GDAL 写 Float32 GTiff：saveImage1 单波段，saveImage 多波段；成功返回 1，失败（含 RasterIO 写入失败）返回 0。
    // proj、trans 当前未使用，输出不含地理参考。
    int saveImage1(std::string outfilepath, std::vector<float> &c,
                  int width, int height,int band, std::string proj, double trans[6]);


    int saveImage(std::string outfilepath, std::vector<std::vector<float>> &c,
                  int width, int height,int band, std::string proj, double trans[6]);


    // 对象实例位置记录：位置 (x, y, z)、缩放 scale（缺省 1）、旋转 rotation（缺省 0）。
    // 坐标单位与旋转单位由调用方约定（待核对）。
    struct PositionRecord
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        float scale{1.0f};
        float rotation{0.0f};
    };

    // 数值表与实例文件读取（行解析规则见 utils.cpp 中 readNumericRows / readPositionFile）。
    // Rows of a whitespace/tab/comma separated numeric table. Blank lines,
    // '#' and leading '//' comments and rows with a non-numeric field are
    // skipped and counted. Throws when the file cannot be opened.
    std::vector<std::vector<float>> readNumericRows(const std::string& path,
                                                    const std::string& description,
                                                    size_t& skippedRows);

    // Instance rows "x y z [scale] [rotation]"; throws when the file cannot be
    // opened or holds no valid instance.
    std::vector<PositionRecord> readPositionFile(const std::string& path);

    // 读空格分隔文本的第 col 列到 data（跳过前 skip 行）；num 为有效行数；成功返回 1。
    int readascfileinout(std::string infileName, int skip, int col,
                         std::vector<float> &data, int& num);

    /*void imageToBuffer(VkDevice m_device, int m_queueIndex, const nvvk::Texture& imgIn, nvmath::vec3i size, const vk::Buffer& pixelBufferOut);
	void bufferToBuffer(VkDevice m_device, int m_queueIndex, const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut);*/

    // 路径拆分：目录部分与文件名部分（同时识别 '/' 与 '\'）。
    std::string getDirectoryPath(const std::string& fullPath);

    std::string getFileName(const std::string& fullPath);

	// 提取字符串中所有连续数字串并转为 int（过长时饱和为 INT_MAX）。
	std::vector <int> findnum(std::string& ch);


    // 毫秒计时器：print() 以 LOGI 输出 elapsed() 的值（单位按 %5.3f ms 打印）。
    struct MilliTimer : public nvh::Stopwatch
    {
        void print() { LOGI(" --> (%5.3f ms)\n", elapsed()); }
    };


// 把数值格式化为当前区域设置的千分位表示（固定小数格式）。
// Formating with local number representation
    template <class T>
    std::string FormatNumbers(T value)
    {
        std::stringstream ss;
        ss.imbue(std::locale(""));
        ss << std::fixed << value;
        return ss.str();
    }


    // 时间换算：ΔT = TT - UT（秒）；闰年判断；年内日序 → 月、日（越界抛异常）。
    double calculateDeltaT(int year, int month);

    bool isLeapYear(int year);

    void calculateMonthAndDay(int kyear, int kdoy, int *kmonth, int *kday);

}