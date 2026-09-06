#pragma once

#include <string>

namespace nfsheatvr {

void InitialiseLogger();
void Log(const std::wstring& message);
void LogHr(const wchar_t* operation, long result);

} // namespace nfsheatvr
