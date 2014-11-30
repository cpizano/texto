#include "stdafx.h"

enum class HardFailures {
  none,
  config_missing
};

void HardfailMsgBox(HardFailures id, const wchar_t* info) {
  __debugbreak();
}

plx::File OpenConfigFile() {
  auto appdata_path = plx::GetAppDataPath(false);
  auto path = appdata_path.append(L"vortex\\texto\\config.json");
  plx::FileParams fparams = plx::FileParams::Read_SharedRead();
  return plx::File::Create(path, fparams, plx::FileSecurity());
}


int __stdcall wWinMain(HINSTANCE instance, HINSTANCE,
                       wchar_t* cmdline, int cmd_show) {

  try {
    auto config = plx::JsonFromFile(OpenConfigFile());


  } catch (plx::IOException& ex) {
    HardfailMsgBox(HardFailures::config_missing, ex.Name());
    return 1;
  }

  return 0;
}
