// Linux 无头版 MOBase::WriteRegistryValue：替换上游 registry.cpp（其只读 ini 的处理会弹 Qt 对话框，
// CLI 里不可用）。语义：直接写 ini，失败返回 false，不弹窗、不修改文件属性。
#include <uibase/registry.h>

namespace MOBase
{

bool WriteRegistryValue(LPCWSTR appName, LPCWSTR keyName, LPCWSTR value, LPCWSTR fileName)
{
  return ::WritePrivateProfileStringW(appName, keyName, value, fileName) != 0;
}

}  // namespace MOBase
