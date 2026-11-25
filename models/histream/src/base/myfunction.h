// ==========================================================================
// 文件：myfunction.h
// 功能：myFunction —— 全静态的小工具类声明：区间随机数、按分隔符切分字符串并转换为 string/int/float。
// 主要模块：
//   1. generateRandomFloat：在 [min, max) 内取一个随机浮点数
//   2. mySplitStr / mySplitInt / mySplitFloat：把 "a,b,c" 形式的文本切成数组
// 与其他文件：实现在 myfunction.cpp；fileio.cpp 用 mySplitFloat/mySplitStr 解析 XML 中以逗号
//   分隔的数值与名称列表（波段、角度、网格名、光谱名等）。
// 约定：切分函数的 delim 是“字符集合”（其中任一字符都是分隔符），连续分隔符与首尾分隔符
//   产生的空项会被丢弃；空字符串返回空数组。
// ==========================================================================

//
// Created by zhongsy on 2024/8/9.
//

#ifndef FIELD_MYFUNCTION_H
#define FIELD_MYFUNCTION_H
#pragma once
#include <vector>
#include <random>
// DEFERRED[低] 可维护性: 声明中使用 std::string 却未 #include <string>，依赖包含顺序才能编译；myfunction.cpp 以 "myFunction.h"（大小写不同）包含本文件，在区分大小写的文件系统（Linux/WSL 构建、部分 CI）上找不到头文件；建议：补 #include <string>，统一文件名大小写。

// 类：myFunction —— 仅含静态方法的工具类，无状态。
class myFunction
{
public:
    myFunction() {};
    ~myFunction() {};

//    static bool sensorNameExist(std::vector<SensorStruct> sensorDatasets, std::string name);
    // 在 [min, max) 内均匀取一个随机数（每次调用都重新播种，见 myfunction.cpp）。
    static float generateRandomFloat(int min, int max);

//    static int GetListNodeLen(struct ListNode* l1);
    // 按 delim 中的任一字符切分字符串；跳过空项，不做 trim；空串返回空数组。
    // mySplitInt / mySplitFloat 在此基础上用 atoi / atof 转换，非法数字静默得 0。
    static std::vector<std::string> mySplitStr(const std::string& str, const std::string& delim);
    static std::vector<int> mySplitInt(const std::string& str, const std::string& delim);
    static std::vector<float> mySplitFloat(const std::string& str, const std::string& delim);
};

#endif //FIELD_MYFUNCTION_H
