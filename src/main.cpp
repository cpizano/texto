#include "stdafx.h"

enum class HardFailures {
  none,
  bad_config
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

struct Settings {
  std::string user_name;
  int window_width = 1000;
  int window_height = 1000;
};

Settings LoadSettings() {
  auto config = plx::JsonFromFile(OpenConfigFile());
  if (config.type() != plx::JsonType::OBJECT)
    throw plx::IOException(__LINE__, L"<unexpected json>");

  return Settings();
}


int __stdcall wWinMain(HINSTANCE instance, HINSTANCE,
                       wchar_t* cmdline, int cmd_show) {

  try {

    auto settings = LoadSettings();

  } catch (plx::IOException& ex) {
    HardfailMsgBox(HardFailures::bad_config, ex.Name());
    return 1;
  }

  return 0;
}
