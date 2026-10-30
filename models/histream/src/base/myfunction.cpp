//
// Created by zhongsy on 2024/8/9.
//
#include "myFunction.h"

//bool myFunction::sensorNameExist(std::vector<SensorStruct> sensorDatasets, std::string name)
//{
//    for (auto data : sensorDatasets)
//    {
//        if (data.name == name)
//        {
//            return true;
//        }
//    }
//    return false;
//}

float myFunction::generateRandomFloat(int min, int max)
{
    // DEFERRED[中] 性能: 每次调用都构造 std::random_device 和 mt19937（约 5 KB 状态并读取系统熵源），循环中调用极慢，且 random_device 在部分实现上可能是确定性的；参数为 int 无法生成 [0,0.5) 这类小数区间；理由：改 thread_local 引擎与 float 版签名会改变现有随机数序列/调用点语义，且不属本轮允许的缺陷类别，未改。
    // 用 dis 变换 gen 生成的随机 unsigned int 为 [1, 2) 中的 double
    // 每次调用 dis(gen) 都生成新的随机 double
    std::random_device              rd;  // Will be used to obtain a seed for the random number engine
    std::mt19937                    gen(rd());  // Standard mersenne_twister_engine seeded with rd()
    std::uniform_real_distribution<> disx(min, max);
    float offx = disx(gen);
    return offx;
}

std::vector<std::string> myFunction::mySplitStr(const std::string& str, const std::string& delim)
{
    std::vector<std::string> res;
    if ("" == str) return res;
    // FIXED[中] 内存: strs 与 d 两个 new char[] 从未 delete[]，每次调用都泄漏（mySplitInt/mySplitFloat 同样）；已修复：三个 mySplit* 均改为基于 std::string::find_first_of 的无堆分配分割，不再 new char[]，函数签名与全部调用点不变。
    std::size_t begin = str.find_first_not_of(delim);
    while (begin != std::string::npos) {
        const std::size_t end = str.find_first_of(delim, begin);
        res.push_back(str.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        begin = (end == std::string::npos) ? std::string::npos : str.find_first_not_of(delim, end);
    }
    return res;
}

std::vector<int> myFunction::mySplitInt(const std::string& str, const std::string& delim)
{
    std::vector<int> res;
    if ("" == str) return res;
    std::size_t begin = str.find_first_not_of(delim);
    while (begin != std::string::npos) {
        // DEFERRED[低] 错误处理: atoi/atof 对非法输入静默返回 0、溢出时为 UB/未定义值，配置中的错别字会被当作 0 使用（mySplitFloat 同理）；理由：本轮只修复 new[] 泄漏，改用 from_chars/strtol 并报错属另一类改动，未做。
        const std::size_t end = str.find_first_of(delim, begin);
        const std::string token = str.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        res.push_back(atoi(token.c_str())); //分割得到的字符串转换为int类型
        begin = (end == std::string::npos) ? std::string::npos : str.find_first_not_of(delim, end);
    }
    return res;
}

std::vector<float> myFunction::mySplitFloat(const std::string& str, const std::string& delim)
{
    std::vector<float> res;
    if ("" == str) return res;
    std::size_t begin = str.find_first_not_of(delim);
    while (begin != std::string::npos) {
        const std::size_t end = str.find_first_of(delim, begin);
        const std::string token = str.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        res.push_back(atof(token.c_str())); //分割得到的字符串转换为float类型
        begin = (end == std::string::npos) ? std::string::npos : str.find_first_not_of(delim, end);
    }
    return res;
}

//int myFunction::GetListNodeLen(struct ListNode* l1)
//{
//    if (l1 == NULL)
//    {
//        return 0;
//    }
//    struct ListNode* p=l1;
//    int aListLen = 0;
//    while(p != NULL)    //判断当前节点指针是否为空
//    {
//        aListLen ++;
//        p = p->next;
//    }
//    return aListLen;
//}

