#include "stdafx.h"
#include "resource.h"

namespace ui_txt {
  const wchar_t gretting[] = L"Welcome to the TExTO editor\n"
                             L"      by AlienRancher      \n"
                             L"                           \n"
                             L" -- start typing to begin -- ";;
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

plx::ComPtr<ID2D1Geometry> CreateD2D1Geometry(
    plx::ComPtr<ID2D1Factory2> d2d1_factory,
    const D2D1_ELLIPSE& ellipse) {
  plx::ComPtr<ID2D1EllipseGeometry> geometry;
  auto hr = d2d1_factory->CreateEllipseGeometry(ellipse, geometry.GetAddressOf());
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);
  return geometry;
}

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

struct TextBlock {
  static const size_t max_size = 100;

  std::wstring text;
  DWRITE_TEXT_METRICS metrics;
  plx::ComPtr<IDWriteTextLayout> layout;
};

class Cursor {
  uint32_t block_;
  uint32_t offset_;
  std::vector<TextBlock>& text_;

public:
  Cursor(std::vector<TextBlock>& text) 
      : text_(text), block_(0), offset_(0) {
    text_.resize(1);
  }

  uint32_t block_len() const {
    return plx::To<uint32_t>(current_block().text.size());
  }

  uint32_t current_offset() const {
    return offset_;
  }

  uint32_t block_number() const {
    return block_;
  }

  bool move_left() {
    if (offset_ > 0) {
      --offset_;
    } else if (block_ > 0) {
      --block_;
      offset_ = block_len();
    } else {
      return false;
    }
    return true;
  }

  bool move_right() {
    if (offset_ < block_len()) {
      ++offset_;
    } else if (block_ < (text_.size() - 1)) {
      ++block_;
      offset_ = 0;
    } else {
      return false;
    }
    return true;
  }

  TextBlock& current_block() {
    return text_[block_];
  }

  const TextBlock& current_block() const {
    return text_[block_];
  }

  void new_block() {
    text_.emplace_back();
    ++block_;
    offset_ = 0;
  }

  void insert_char(wchar_t ch) {
    current_block().text.insert(offset_, 1, ch);
    ++offset_;
  }

  bool remove_char() {
    auto& txt = current_block().text;
    if (offset_ == 0)
      return false;   // $$$ handle erasure here.

    if (!txt.empty()) {
      txt.erase(--offset_, 1);
    } else if (block_ != 0) {
      auto it = begin(text_) + block_;
      text_.erase(it);
      --block_;
      offset_ = block_len();
    } else {
      return false;
    }
    return true;
  }

};

class PlainTextFileIO {
  plx::File file_;

public:
  PlainTextFileIO(plx::FilePath& path) 
      : file_(plx::File::Create(
            path, 
            plx::FileParams::ReadWrite_SharedRead(CREATE_ALWAYS),
            plx::FileSecurity())) {
  }

  void save(std::vector<TextBlock>& text) {
    std::wstring content;
    for (auto& block : text) {
      content.append(block.text);
      content.append(1, L'\n');
      if (content.size() > (32 * 1024)) {
        block_to_disk(plx::RangeFromString(content));
        content.clear();
      }
    }
    if (!content.empty())
      block_to_disk(plx::RangeFromString(content));
  }

  void block_to_disk(plx::Range<const uint16_t>& block) {
    auto utf8 = plx::UTF8FromUTF16(block);
    file_.write(plx::RangeFromString(utf8));
  }
};

enum FlagOptions {
  debug_text_boxes,
  opacity_50_percent,
  alternate_font,
  flag_op_last
};

class DCoWindow : public plx::Window <DCoWindow> {
  // width and height are in logical pixels.
  const int width_;
  const int height_;

  std::bitset<flag_op_last> flag_options_;

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

  plx::ComPtr<ID2D1Geometry> circle_geom_;

  plx::ComPtr<IDWriteFactory> dwrite_factory_;
  plx::ComPtr<IDWriteTextFormat> text_fmt_[2];

  enum Brushes {
    brush_black,
    brush_red,
    brush_text,
    brush_last
  };

  plx::ComPtr<ID2D1SolidColorBrush> brushes_[brush_last];

public:
  DCoWindow(int width, int height)
      : width_(width), height_(height), cursor_(text_), scroll_v_(0.0f) {
    // $$ read from config.
    margin_tl_ = D2D1::Point2F(12.0f, 36.0f);
    margin_br_ = D2D1::Point2F(6.0f, 16.0f);

    // init the text system.


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
    d3d_device_ = plx::CreateDeviceD3D11(0);
    d2d_factory_ = plx::CreateD2D1FactoryST(D2D1_DEBUG_LEVEL_NONE);
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

    circle_geom_ = CreateD2D1Geometry(
        d2d_factory_,
        D2D1::Ellipse(D2D1::Point2F(width_ - 18.0f , 18.0f), 8.0f, 8.0f));

    dwrite_factory_ = CreateDWriteFactory();
    text_fmt_[0] = CreateDWriteTextFormat(
        dwrite_factory_, L"Candara", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 20.0f);
    text_fmt_[1] = CreateDWriteTextFormat(
        dwrite_factory_, L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f);

    {
      ScopedDraw sd(root_surface_, dpi_);
      auto dc = sd.begin(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.9f), zero_offset);

      // create solid brushes.
      dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, 1.0f), 
          brushes_[brush_black].GetAddressOf());
      dc->CreateSolidColorBrush(D2D1::ColorF(0xE90000, 1.0f), 
          brushes_[brush_red].GetAddressOf());
      dc->CreateSolidColorBrush(D2D1::ColorF(RGB(57, 135, 214), 1.0f), 
          brushes_[brush_text].GetAddressOf());

      //// Render start UI ////////////////////////////////////////////////////////////////////
      auto title_fmt = CreateDWriteTextFormat(
          dwrite_factory_, L"Candara", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, 24.0f);
      auto txt = plx::RangeFromLitStr(ui_txt::gretting);
      auto size = D2D1::SizeF(static_cast<float>(width_), static_cast<float>(height_));
      auto greetings = CreateDWTextLayout(dwrite_factory_, title_fmt, txt, size);

      plx::ComPtr<ID2D1SolidColorBrush> brush;
      dc->CreateSolidColorBrush(D2D1::ColorF(RGB(74, 174, 0), 1.0), brush.GetAddressOf());
      DWRITE_TEXT_METRICS text_metrics;
      greetings->GetMetrics(&text_metrics);
      auto offset_x = (static_cast<float>(width_) - text_metrics.width) / 2.0f; 
      dc->DrawTextLayout(D2D1::Point2F(offset_x, 40.0f), greetings.Get(), brush.Get());
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
      case WM_KEYDOWN: {
        return keydown_handler(static_cast<int>(wparam));
      }
      case WM_CHAR: {
        return char_handler(static_cast<wchar_t>(wparam));
      }
      case WM_COMMAND: {
        return ui_command_handler(LOWORD(wparam));
      }
      case WM_MOUSEMOVE: {
        return mouse_move_handler(wparam, MAKEPOINTS(lparam));
      }
      case WM_LBUTTONDOWN: {
        return left_mouse_button_handler(true, MAKEPOINTS(lparam));
      }
      case WM_LBUTTONUP: {
        return left_mouse_button_handler(false, MAKEPOINTS(lparam));
      }
      case WM_MOUSEWHEEL: {
        return mouse_wheel_handler(HIWORD(wparam), LOWORD(wparam));
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

  LRESULT keydown_handler(int vkey) {
    if (vkey == VK_LEFT) {
      if (!cursor_.move_left()) {
        ::Beep(440, 10);
        return 0L;
      }
    }
    if (vkey == VK_RIGHT) {
      if (!cursor_.move_right()) {
        ::Beep(440, 10);
        return 0L;
      }
    }

    update_screen();
    return 0L;
  }
    
  LRESULT char_handler(wchar_t code) {
    if (code == 0x0D)
      add_character('\n');
    else
      add_character(code);
    return 0L;
  }

  LRESULT left_mouse_button_handler(bool down, POINTS pts) {
    if (down) {
      BOOL hit = 0;
      circle_geom_->FillContainsPoint(
          D2D1::Point2F(pts.x, pts.y), D2D1::Matrix3x2F::Identity(), &hit);
      if (hit != 0) {
        ::SendMessageW(window(), WM_SYSCOMMAND, SC_MOVE|0x0002, 0);
      }
    } else {

    }
    return 0L;
  }

  LRESULT mouse_move_handler(UINT_PTR state, POINTS pts) {
    return 0L;
  }

  LRESULT mouse_wheel_handler(int16_t offset, int16_t vkey) {
    // $$ read the divisor from the config file.
    scroll_v_ -= offset / 4;
    update_screen();
    return 0L;
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
    return 0L;
  }

  LRESULT ui_command_handler(int command_id) {
    if (command_id == IDC_DBG_TEXT_BOXES) {
      flag_options_[debug_text_boxes].flip();
    }
    if (command_id == IDC_50P_TRANSPARENT) {
      flag_options_[opacity_50_percent].flip();
    }
    if (command_id == IDC_ALT_FONT) {
      flag_options_[alternate_font].flip();
      layout_all();
    }
    if (command_id == IDC_SAVE_PLAINTEXT) {
      PlainTextFileIO ptfio(plx::FilePath(L"c:\\test\\texto_file.txt"));
      ptfio.save(text_);
    }

    update_screen();
    return 0L;
  }

  void add_character(wchar_t ch) {
    bool needs_layout = false;

    auto& block = cursor_.current_block();
    if (ch == '\n') {
      // new line.
      if (cursor_.block_len() >= TextBlock::max_size) {
        // start a new block.
        cursor_.new_block();
        layout(cursor_.current_block());
      } else {
        // new line goes in existing block.
        cursor_.insert_char(ch);
        needs_layout = true;
      }
    } else if (ch == 0x08) {
      // deletion of one character.
      cursor_.remove_char();
      needs_layout = true;
    } else {
      // add a character in the current block.
      cursor_.insert_char(ch);
      needs_layout = true;
    }

    if (needs_layout)
      layout(block);
    update_screen();
  }

  void layout(TextBlock& block) {
    auto box = D2D1::SizeF(
        static_cast<float>(width_) - margin_tl_.x - margin_br_.x,
        static_cast<float>(height_)- margin_tl_.y - margin_tl_.y);
    plx::Range<const wchar_t> txt(block.text.c_str(), block.text.size());
    auto fmt_index = flag_options_[alternate_font] ? 1 : 0;
    block.layout = CreateDWTextLayout(dwrite_factory_, text_fmt_[fmt_index], txt, box);
    auto hr = block.layout->GetMetrics(&block.metrics);
    if (hr != S_OK) {
      __debugbreak();
    }
  }

  void layout_all() {
    for (auto& tb : text_) {
      layout(tb);
    }
  }

  void update_screen() {
    {
      ScopedDraw sd(root_surface_, dpi_);
      auto bk_alpha = flag_options_[opacity_50_percent] ? 0.5f : 0.9f;
      auto dc = sd.begin(D2D1::ColorF(0x000000, bk_alpha), zero_offset);
      dc->DrawGeometry(circle_geom_.Get(), brushes_[brush_red].Get());

      float bottom = 0.0f;
      auto v_min = scroll_v_;
      auto v_max = scroll_v_ + static_cast<float>(height_);

      uint32_t block_number = 0;
      for (auto& tb : text_) {
        tb.metrics.top = bottom;
        bottom += tb.metrics.height;

        if (((bottom > v_min) && (bottom < v_max)) || 
            ((tb.metrics.top > v_min) && (tb.metrics.top < v_max))) {
          // in view, paint it.
          dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, tb.metrics.top - scroll_v_));
          dc->DrawTextLayout(margin_tl_, tb.layout.Get(), brushes_[brush_text].Get()); 
          // debugging rectangle.
          if (flag_options_[debug_text_boxes]) {
            dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            auto debug_rect = 
                D2D1::RectF(tb.metrics.left + margin_tl_.x, margin_tl_.y,
                            tb.metrics.width + margin_tl_.x, tb.metrics.height + margin_tl_.y);
            dc->DrawRectangle(debug_rect, brushes_[brush_red].Get(), 1.0f);
          }
          if (cursor_.block_number() == block_number) {
            // draw caret since it is visible.
            dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            DWRITE_HIT_TEST_METRICS hit_metrics;
            float x, y;
            auto hr = tb.layout->HitTestTextPosition(
                cursor_.current_offset(), FALSE, &x, &y, &hit_metrics);
            if (hr != S_OK)
              throw plx::ComException(__LINE__, hr);
            x += margin_tl_.x;
            y += margin_tl_.y;
            dc->DrawRectangle(
                D2D1::RectF(x, y, x + 2.0f, y + hit_metrics.height),
                brushes_[brush_red].Get());
          }
        }
        ++block_number;
      }
    }
    dco_device_->Commit();
  }

};

HACCEL LoadAccelerators() {
  // $$ read this from the config file.
  ACCEL accelerators[] = {
    {FVIRTKEY, VK_F1, IDC_SAVE_PLAINTEXT},
    {FVIRTKEY, VK_F10, IDC_DBG_TEXT_BOXES},
    {FVIRTKEY, VK_F11, IDC_50P_TRANSPARENT},
    {FVIRTKEY, VK_F9, IDC_ALT_FONT}
  };

  return ::CreateAcceleratorTableW(accelerators, _countof(accelerators));
}

int __stdcall wWinMain(HINSTANCE instance, HINSTANCE,
                       wchar_t* cmdline, int cmd_show) {
  try {
    auto settings = LoadSettings();
    DCoWindow window(settings.window_width, settings.window_height);

    auto accel_table = LoadAccelerators();

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
