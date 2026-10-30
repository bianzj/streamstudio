//
// Created by zhongsy on 2024/8/9.
//

#ifndef FIELD_MYFUNCTION_H
#define FIELD_MYFUNCTION_H
#pragma once
#include <vector>
#include <random>
// DEFERRED[低] 可维护性: 声明中使用 std::string 却未 #include <string>，依赖包含顺序才能编译；myfunction.cpp 以 "myFunction.h"（大小写不同）包含本文件，在区分大小写的文件系统（Linux/WSL 构建、部分 CI）上找不到头文件；建议：补 #include <string>，统一文件名大小写。

class myFunction
{
public:
    myFunction() {};
    ~myFunction() {};

//    static bool sensorNameExist(std::vector<SensorStruct> sensorDatasets, std::string name);
    static float generateRandomFloat(int min, int max);

//    static int GetListNodeLen(struct ListNode* l1);
    static std::vector<std::string> mySplitStr(const std::string& str, const std::string& delim);
    static std::vector<int> mySplitInt(const std::string& str, const std::string& delim);
    static std::vector<float> mySplitFloat(const std::string& str, const std::string& delim);
};

#endif //FIELD_MYFUNCTION_H
