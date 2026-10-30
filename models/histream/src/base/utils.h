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

namespace Utils {

	template<typename T>
	int getNextPow2Number(T number) {
		return std::pow(2, std::ceil(std::log2(number)));
	}

	std::vector <std::string> splitt(std::string& s, std::string& deli);

    std::vector<std::vector<float>> readImage(std::string infilename);


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

	float expint(float x);
	float* infile2num(std::string infileName, int skip, int col, int& num);


	int getMapIndex(std::map<std::string, int> maps, std::string name);



    int readImageinout(std::string infilename,std::vector<std::vector<float>> &c,
                       int &width, int &height, int &nband);

    int readImageinout1(std::string infilename,std::vector<float> &c,
                        int &width, int &height, int &nband);

    int readImageinout11(std::string infilename,std::vector<float> &c,
                         int &width, int &height, int &nband,double *trans, std::string &proj);

    int saveImage1(std::string outfilepath, std::vector<float> &c,
                  int width, int height,int band, std::string proj, double trans[6]);


    int saveImage(std::string outfilepath, std::vector<std::vector<float>> &c,
                  int width, int height,int band, std::string proj, double trans[6]);


    struct PositionRecord
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        float scale{1.0f};
        float rotation{0.0f};
    };

    // Rows of a whitespace/tab/comma separated numeric table. Blank lines,
    // '#' and leading '//' comments and rows with a non-numeric field are
    // skipped and counted. Throws when the file cannot be opened.
    std::vector<std::vector<float>> readNumericRows(const std::string& path,
                                                    const std::string& description,
                                                    size_t& skippedRows);

    // Instance rows "x y z [scale] [rotation]"; throws when the file cannot be
    // opened or holds no valid instance.
    std::vector<PositionRecord> readPositionFile(const std::string& path);

    int readascfileinout(std::string infileName, int skip, int col,
                         std::vector<float> &data, int& num);

    /*void imageToBuffer(VkDevice m_device, int m_queueIndex, const nvvk::Texture& imgIn, nvmath::vec3i size, const vk::Buffer& pixelBufferOut);
	void bufferToBuffer(VkDevice m_device, int m_queueIndex, const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut);*/

    std::string getDirectoryPath(const std::string& fullPath);

    std::string getFileName(const std::string& fullPath);

	template <typename T>
	void saveOut(std::string outFile, T data, int num);

	std::vector <int> findnum(std::string& ch);


    struct MilliTimer : public nvh::Stopwatch
    {
        void print() { LOGI(" --> (%5.3f ms)\n", elapsed()); }
    };


// Formating with local number representation
    template <class T>
    std::string FormatNumbers(T value)
    {
        std::stringstream ss;
        ss.imbue(std::locale(""));
        ss << std::fixed << value;
        return ss.str();
    }


    double calculateDeltaT(int year, int month);

    bool isLeapYear(int year);

    void calculateMonthAndDay(int kyear, int kdoy, int *kmonth, int *kday);

}