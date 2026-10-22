#pragma once
// Модуль file — работа с файлами.
#include <string>
#include <vector>

std::string nlfile_read(const std::string& path);
void nlfile_write(const std::string& path, const std::string& data);
void nlfile_append(const std::string& path, const std::string& data);
bool nlfile_exists(const std::string& path);
std::vector<std::string> nlfile_lines(const std::string& path);
long long nlfile_size(const std::string& path);
bool nlfile_remove(const std::string& path);
std::vector<std::string> nlfile_list(const std::string& path);
std::string nlfile_name(const std::string& path);
