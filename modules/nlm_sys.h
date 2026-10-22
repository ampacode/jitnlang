#pragma once
// Модуль sys — argv/env/exec/cwd/exit.
// nlsys_capture вызывается из сгенерированного main() когда
// используется `using <sys>` — иначе args пуст.
#include <string>
#include <vector>

void nlsys_capture(int argc, char** argv);
long long nlsys_argc();
std::vector<std::string> nlsys_args();
std::string nlsys_arg(long long i);
std::string nlsys_env(const std::string& name);
void nlsys_envSet(const std::string& n, const std::string& v);
std::string nlsys_exec(const std::string& cmd);
long long nlsys_execCode(const std::string& cmd);
void nlsys_exit(long long code);
std::string nlsys_cwd();
