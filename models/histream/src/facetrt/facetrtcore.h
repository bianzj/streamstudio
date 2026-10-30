#pragma once

#include <string>

int runFacetRTCore(const std::string& inputPath,
                   const std::string& shaderDirectory,
                   const std::string& outputFile = {});

int runFacetEBCore(const std::string& inputPath,
                   const std::string& shaderDirectory,
                   const std::string& outputFile = {});
