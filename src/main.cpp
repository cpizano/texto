#include "stdafx.h"
#include "resource.h"

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
  int window_width = 1200;
  int window_height = 1000;
};

Settings LoadSettings() {
  auto config = plx::JsonFromFile(OpenConfigFile());
  if (config.type() != plx::JsonType::OBJECT)
    throw plx::IOException(__LINE__, L"<unexpected json>");

  return Settings();
}

class DCoWindow : public plx::Window <DCoWindow> {
  const int width_;
  const int height_;

public:
  DCoWindow(int width, int height) : width_(width), height_(height) {
    create_window(WS_EX_NOREDIRECTIONBITMAP,
                  WS_POPUP | WS_VISIBLE,
                  L"texto @ 2014",
                  nullptr, nullptr,
                  10, 10,
                  width_, height_,
                  nullptr,
                  nullptr);
  }

  LRESULT message_handler(const UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_DESTROY: {
        ::PostQuitMessage(0);
        return 0;
      }
      case WM_PAINT: {
        paint_handler();
        break;
      }
      case WM_LBUTTONDOWN: {
        return left_mouse_button_handler(MAKEPOINTS(lparam));
      }
      case WM_MOUSEMOVE: {
        return mouse_move_handler(wparam, MAKEPOINTS(lparam));
      }
      case WM_WINDOWPOSCHANGING: {
        // send to fixed size windows when there is a device loss. do nothing
        // to prevent the default window proc from resizing to 640x400.
        return 0;
      }
      case WM_DPICHANGED: {
        return dpi_changed_handler(lparam);
      }
    }

    return ::DefWindowProc(window_, message, wparam, lparam);
  }

  void paint_handler() {
    // just recovery here when using direct composition.
  }

  LRESULT left_mouse_button_handler(POINTS pts) {
    return 0;
  }

  LRESULT mouse_move_handler(UINT_PTR state, POINTS pts) {
    return 0;
  }

  LRESULT dpi_changed_handler(LPARAM lparam) {
    plx::RectL r(plx::SizeL(
          static_cast<long>(dpi_.to_physical_x(width_)),
          static_cast<long>(dpi_.to_physical_x(height_))));
    
    auto suggested = reinterpret_cast<const RECT*> (lparam);
    ::AdjustWindowRectEx(&r, 
        ::GetWindowLong(window_, GWL_STYLE),
        FALSE,
        ::GetWindowLong(window_, GWL_EXSTYLE));
    ::SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                   r.width(), r.height(),
                   SWP_NOACTIVATE | SWP_NOZORDER);
    return 0;
  }

};


int __stdcall wWinMain(HINSTANCE instance, HINSTANCE,
                       wchar_t* cmdline, int cmd_show) {

  try {

    auto settings = LoadSettings();
    DCoWindow window(settings.window_width, settings.window_height);


    auto accel_table =
        ::LoadAcceleratorsW( instance, MAKEINTRESOURCE(IDR_ACCEL_USA));
    MSG msg = {0};
    while (::GetMessage(&msg, NULL, 0, 0)) {
      if (!::TranslateAccelerator(msg.hwnd, accel_table, &msg)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
      }
    }

    return (int) msg.wParam;

  } catch (plx::IOException& ex) {
    HardfailMsgBox(HardFailures::bad_config, ex.Name());
    return 1;
  }

  return 0;
}
