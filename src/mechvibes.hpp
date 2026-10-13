#pragma once
#include "core.hpp"

namespace ck
{
Pack load_mechvibes_pack(const std::filesystem::path &config);
std::string import_mechvibes_zip(const std::filesystem::path &zip, const std::filesystem::path &packs);
unsigned mechvibes_key(const std::string &key);
} // namespace ck
