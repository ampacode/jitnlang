#pragma once
#include <string>
#include "nvm.h"

std::string stripComments(const std::string& src);

struct CompileResult {
    std::string cppCode;
    Nvm vm;
};

CompileResult get_code(const std::string& filename);
