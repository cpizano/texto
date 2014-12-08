#include "stdafx.h"
#include "resource.h"

namespace ui_txt {
  const wchar_t gretting[] = L"Welcome to TExTO editor";
}

enum class HardFailures {
  none,
  bad_config,
  com_error
};

void HardfailMsgBox(HardFailures id, const wchar_t* info) {
  // $$ implement.
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
  // $$ read & set something here.
  return Settings();
}

const D2D1_SIZE_F zero_offset = {0};

class ScopedDraw {
  bool drawing_;
  plx::ComPtr<IDCompositionSurface> ics_;
  const plx::DPI& dpi_;

public:
  ScopedDraw(plx::ComPtr<IDCompositionSurface> ics,
          const plx::DPI& dpi) 
    : drawing_(false),
      ics_(ics),
      dpi_(dpi) {
  }

  ~ScopedDraw() {
    end();
  }

  plx::ComPtr<ID2D1DeviceContext> begin(const D2D1_COLOR_F& clear_color,
                                        const D2D1_SIZE_F& offset) {
    auto dc = plx::CreateDCoDeviceCtx(ics_, dpi_, offset);
    dc->Clear(clear_color);
    drawing_ = true;
    return dc;
  }

  void end() {
    if (drawing_)
      ics_->EndDraw();
  }
};

plx::ComPtr<IDWriteFactory> CreateDWriteFactory() {
  plx::ComPtr<IDWriteFactory> factory;
  auto hr = ::DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED,
      __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);
  return factory;
}

plx::ComPtr<IDWriteTextFormat> CreateDWriteTextFormat(
    plx::ComPtr<IDWriteFactory> dw_factory,
    const wchar_t* font_family,
    DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
    DWRITE_FONT_STRETCH stretch, float size) {
  plx::ComPtr<IDWriteTextFormat> format;
  auto hr = dw_factory->CreateTextFormat(
      font_family, nullptr, weight, style, stretch, size, L"", format.GetAddressOf());
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);
  return format;
}

plx::ComPtr<IDWriteTextLayout> CreateDWTextLayout(
  plx::ComPtr<IDWriteFactory> dw_factory, plx::ComPtr<IDWriteTextFormat> format,
  const plx::Range<const wchar_t>& text, const D2D1_SIZE_F& size) {
  plx::ComPtr<IDWriteTextLayout> layout;
  auto hr = dw_factory->CreateTextLayout(
      text.start(), plx::To<UINT32>(text.size()),
      format.Get(),
      size.width, size.height,
      layout.GetAddressOf());
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);
  return layout;
}

enum Brushes {
  brush_black,
  brush_red,
  brush_text,
  brush_last
};

struct TextBlock {
  static const size_t max_size = 100;

  std::wstring text;
  DWRITE_TEXT_METRICS metrics;
  plx::ComPtr<IDWriteTextLayout> layout;
};

struct Cursor {
  int block;
  size_t offset;

  Cursor() : block(-1), offset(0) {}
};

class DCoWindow : public plx::Window <DCoWindow> {
  // width and height are in logical pixels.
  const int width_;
  const int height_;

  // the margins are insets from (width and height).
  D2D1_POINT_2F margin_tl_;
  D2D1_POINT_2F margin_br_;

  std::vector<TextBlock> text_;
  Cursor cursor_;
  float scroll_v_;

  plx::ComPtr<ID3D11Device> d3d_device_;
  plx::ComPtr<ID2D1Factory2> d2d_factory_;
  plx::ComPtr<ID2D1Device> d2d_device_;
  plx::ComPtr<IDCompositionDesktopDevice> dco_device_;
  plx::ComPtr<IDCompositionTarget> dco_target_;
  plx::ComPtr<IDCompositionVisual2> root_visual_;
  plx::ComPtr<IDCompositionSurface> root_surface_;

  plx::ComPtr<IDWriteFactory> dwrite_factory_;
  plx::ComPtr<IDWriteTextFormat> text_fmt_;

  plx::ComPtr<ID2D1SolidColorBrush> brushes_[brush_last];

public:
  DCoWindow(int width, int height) : width_(width), height_(height), scroll_v_(0.0f) {
    // $$ read from config.
    margin_tl_ = D2D1::Point2F(12.0f, 12.0f);
    margin_br_ = D2D1::Point2F(6.0f, 16.0f);

    // init the text system.
    text_.resize(1);
    cursor_.block = 0;

    // create the window.
    create_window(WS_EX_NOREDIRECTIONBITMAP,
                  WS_POPUP | WS_VISIBLE,
                  L"texto @ 2014",
                  nullptr, nullptr,
                  10, 10,
                  width_, height_,
                  nullptr,
                  nullptr);
    // create the 3 devices and 1 factory.
#if defined (_DEBUG)
    d3d_device_ = plx::CreateDeviceD3D11(D3D11_CREATE_DEVICE_DEBUG);
    d2d_factory_ = plx::CreateD2D1FactoryST(D2D1_DEBUG_LEVEL_INFORMATION);
#else
    d3d_device_ = plx::CreateDevice3D(0);
    d2d1_factory_ = plx::CreateD2D1FactoryST(D2D1_DEBUG_LEVEL_NONE);
#endif
    d2d_device_ = plx::CreateDeviceD2D1(d3d_device_, d2d_factory_);
    dco_device_ = plx::CreateDCoDevice2(d2d_device_);
    // create the composition target and the root visual.
    dco_target_ = plx::CreateDCoWindowTarget(dco_device_, window());
    root_visual_ = plx::CreateDCoVisual(dco_device_);
    // bind direct composition to our window.
    auto hr = dco_target_->SetRoot(root_visual_.Get());
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    // allocate the gpu surface and bind it to the root visual.
    root_surface_ = plx::CreateDCoSurface(
        dco_device_,
        static_cast<unsigned int>(dpi_.to_physical_x(width_)),
        static_cast<unsigned int>(dpi_.to_physical_x(height_)));
    hr = root_visual_->SetContent(root_surface_.Get());
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);

    dwrite_factory_ = CreateDWriteFactory();
    text_fmt_ = CreateDWriteTextFormat(
        dwrite_factory_, L"Candara", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 20.0f);

    {
      ScopedDraw sd(root_surface_, dpi_);
      auto dc = sd.begin(D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.7f), zero_offset);

      // create solid brushes.
      dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), 
          brushes_[brush_black].GetAddressOf());
      dc->CreateSolidColorBrush(D2D1::ColorF(9.0f, 0.0f, 0.0f, 1.0f), 
          brushes_[brush_red].GetAddressOf());
      dc->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.9f, 0.8f, 1.0f), 
          brushes_[brush_text].GetAddressOf());

      //// Render start UI ////////////////////////////////////////////////////////////////////
      auto txt = plx::RangeFromLitStr(ui_txt::gretting);
      auto size = D2D1::SizeF(static_cast<float>(width_), static_cast<float>(height_));
      auto greetings = CreateDWTextLayout(dwrite_factory_, text_fmt_, txt, size);

      plx::ComPtr<ID2D1SolidColorBrush> brush;
      dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.5f, 1.0f, 1.0f), brush.GetAddressOf());
      dc->DrawTextLayout(D2D1::Point2F(2.0f, 2.0f), greetings.Get(), brush.Get());
    }
    dco_device_->Commit();
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
#if 0
      case WM_WINDOWPOSCHANGING: {
        // send to fixed size windows when there is a device loss. do nothing
        // to prevent the default window proc from resizing to 640x400.
        return 0;
      }
#endif
      case WM_KEYDOWN: {
        if (wparam == VK_DOWN) {
          scroll_v_ += 24.0f;
          update_screen();
        }
        if (wparam == VK_UP) {
          scroll_v_ -= 24.0f;
          update_screen();
        }
        return 0L;
      }
      case WM_CHAR: {
        new_char_handler(static_cast<wchar_t>(wparam));
        return 0L;
      }
      case WM_MOUSEMOVE: {
        return mouse_move_handler(wparam, MAKEPOINTS(lparam));
      }
      case WM_LBUTTONDOWN: {
        return left_mouse_button_handler(MAKEPOINTS(lparam));
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

  void new_char_handler(wchar_t code) {
    if (code == 0x0D)
      add_character('\n');
    else
      add_character(code);
  }

  LRESULT left_mouse_button_handler(POINTS pts) {
    return 0;
  }

  LRESULT mouse_move_handler(UINT_PTR state, POINTS pts) {
    return 0;
  }

  LRESULT dpi_changed_handler(LPARAM lparam) {
    // $$ test this.
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

  void add_character(wchar_t ch) {
    bool needs_layout = false;

    auto& block = text_[cursor_.block];
    if (ch == '\n') {
      // start a new block if necessary.
      if (block.text.size() >= TextBlock::max_size) {
        text_.emplace_back();
        ++cursor_.block;
        cursor_.offset = 0;
        return;
      } else {
        block.text.append(1, ch);
        ++cursor_.offset;
      }
    } else if (ch == 0x08) {
      // delete last character.
      if (!block.text.empty()) {
        block.text.resize(block.text.size() -1);
        --cursor_.offset;
        needs_layout = true;
      } else {
        if (cursor_.block == 0)
          return;
        // not the first block, it can be deleted.
        auto it = begin(text_) + cursor_.block;
        text_.erase(it);
        --cursor_.block;
        cursor_.offset = text_[cursor_.block].text.size();
      }
    } else {
      // add a character in the current block.
      block.text.append(1, ch);
      ++cursor_.offset;
      needs_layout = true;
    }

    if (needs_layout) {
      // Need to re-layout.
      auto box = D2D1::SizeF(
          static_cast<float>(width_) - margin_tl_.x - margin_br_.x,
          static_cast<float>(height_)- margin_tl_.y - margin_tl_.y);
      plx::Range<const wchar_t> txt(block.text.c_str(), block.text.size());
      block.layout = CreateDWTextLayout(dwrite_factory_, text_fmt_, txt, box);
      auto hr = block.layout->GetMetrics(&block.metrics);
      if (hr != S_OK) {
        __debugbreak();
      }
    }
    update_screen();
  }

  void update_screen() {
    {
      ScopedDraw sd(root_surface_, dpi_);
      auto dc = sd.begin(D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.9f), zero_offset);

      float bottom = 0.0f;
      auto v_min = scroll_v_;
      auto v_max = scroll_v_ + static_cast<float>(height_);

      for (auto& tb : text_) {
        tb.metrics.top = bottom;
        bottom += tb.metrics.height;

        if (((bottom > v_min) && (bottom < v_max)) || 
            ((tb.metrics.top > v_min) && (tb.metrics.top < v_max))) {
          // in view, paint it.
          dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, tb.metrics.top - scroll_v_));
          dc->DrawTextLayout(margin_tl_, tb.layout.Get(), brushes_[brush_text].Get()); 
          // debugging rectangle.
          dc->DrawRectangle(
              D2D1::RectF(tb.metrics.left + margin_tl_.x,  margin_tl_.y,
                          tb.metrics.width + margin_tl_.x, tb.metrics.height + margin_tl_.y),
              brushes_[brush_red].Get(),
              0.5f);
        }
      }
    }
    dco_device_->Commit();
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
  } catch (plx::ComException&) {
    HardfailMsgBox(HardFailures::com_error, L"COM");
    return 2;
  }
}
