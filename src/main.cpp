#include "stdafx.h"
#include "resource.h"
#include "texto.h"

// Ideas
// 1.  modified text (like VS ide) side column marker
// 2.  column guides (80 cols, etc)
// 3.  dropbox folder aware
// 4.  number of lines
// 6.  be more permissive with the utf16 conversion
// 7.  don't scroll past the top or bottom
// 8.  save to input file
// 9.  spellchecker
// 10. find in text
// 11. text selection
// 12. impl copy
// 13. text stats above text.

namespace ui_txt {
  const wchar_t no_file_title[] = L"TExTO v0.0.0b <no file> [F2: open]\n";
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

class FileOpenDialog {
  plx::ComPtr<IShellItem> item_;

public:
  FileOpenDialog(HWND owner) {
    plx::ComPtr<IFileDialog> dialog;
    auto hr = ::CoCreateInstance(CLSID_FileOpenDialog, NULL, 
                                 CLSCTX_INPROC_SERVER,
                                 __uuidof(dialog),
                                 reinterpret_cast<void **>(dialog.GetAddressOf()));
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);

    DWORD flags;
    hr = dialog->GetOptions(&flags);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM);
    hr = dialog->Show(owner);
    if (hr != S_OK)
      return;
    
    dialog->GetResult(item_.GetAddressOf());
  }

  bool success() const {
    return item_ ? true : false;
  }

  plx::FilePath path() const {
    wchar_t* file_path = nullptr;
    auto hr = item_->GetDisplayName(SIGDN_FILESYSPATH, &file_path);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    plx::FilePath fp(file_path);
    ::CoTaskMemFree(file_path);
    return fp;
  }
};

const D2D1_SIZE_F zero_offset = {0};

plx::ComPtr<ID2D1Geometry> CreateD2D1Geometry(
    plx::ComPtr<ID2D1Factory2> d2d1_factory,
    const D2D1_ELLIPSE& ellipse) {
  plx::ComPtr<ID2D1EllipseGeometry> geometry;
  auto hr = d2d1_factory->CreateEllipseGeometry(ellipse, geometry.GetAddressOf());
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);
  return geometry;
}

class PlainTextFileIO {
  const plx::FilePath path_;
  const size_t io_size = 32 * 1024;

public:
  PlainTextFileIO(plx::FilePath& path) : path_(path) {
  }

  void save(std::vector<TextBlock>& text) {
    auto file = plx::File::Create(
        path_, 
        plx::FileParams::ReadWrite_SharedRead(CREATE_ALWAYS),
        plx::FileSecurity());

    std::wstring content;
    for (auto& block : text) {
      content.append(*block.text);
      content.append(1, L'\n');
      if (content.size() > io_size) {
        block_to_disk(file, content);
        content.clear();
      }
    }
    if (!content.empty())
      block_to_disk(file, content);
  }

  void load(Cursor& cursor) {
    auto file = plx::File::Create(
        path_, 
        plx::FileParams::ReadWrite_SharedRead(OPEN_EXISTING),
        plx::FileSecurity());
    // need to read the whole file at once because the UTF16 conversion fails if
    // we end up trying to convert in the middle of multi-byte sequence.
    cursor.add_string(file_from_disk(file), false);
  }

private:
  void block_to_disk(plx::File& file, const std::wstring& str_block) {
    auto block = plx::RangeFromString(str_block);
    auto utf8 = plx::UTF8FromUTF16(block);
    file.write(plx::RangeFromString(utf8));
  }

  std::wstring file_from_disk(plx::File& file) {
    auto fsz = file.size_in_bytes();
    plx::Range<uint8_t> block(nullptr, fsz);
    auto heap = plx::HeapRange(block);
    auto bytes_read = file.read(block);
    // remove all CR so we only end up with LF.
    auto last = std::remove(block.start(), block.end(), '\r');
    return plx::UTF16FromUTF8(plx::Range<const uint8_t>(block.start(), last));
  }
};

enum FlagOptions {
  debug_text_boxes,
  opacity_50_percent,
  alternate_font,
  flag_op_last
};

#pragma region window

class DCoWindow : public plx::Window <DCoWindow> {
  // width and height are in logical pixels.
  const int width_;
  const int height_;

  std::bitset<flag_op_last> flag_options_;

  // the margins are insets from (width and height).
  D2D1_POINT_2F margin_tl_;
  D2D1_POINT_2F margin_br_;
  // widgets insets from right side.
  D2D1_POINT_2F widget_pos_;
  D2D1_POINT_2F widget_radius_;

  float scroll_v_;
  //float scale_;
  D2D1::Matrix3x2F scale_;

  plx::ComPtr<ID3D11Device> d3d_device_;
  plx::ComPtr<ID2D1Factory2> d2d_factory_;
  plx::ComPtr<ID2D1Device> d2d_device_;
  plx::ComPtr<IDCompositionDesktopDevice> dco_device_;
  plx::ComPtr<IDCompositionTarget> dco_target_;
  plx::ComPtr<IDCompositionVisual2> root_visual_;
  plx::ComPtr<IDCompositionSurface> root_surface_;
  plx::ComPtr<IDWriteFactory> dwrite_factory_;

  // widgets.
  plx::ComPtr<ID2D1Geometry> circle_geom_move_;
  plx::ComPtr<ID2D1Geometry> circle_geom_close_;

  enum TxtFromat {
    fmt_mono_text,
    fmt_prop_large,
    fmt_title_right,
    fmt_last
  };
  plx::ComPtr<IDWriteTextFormat> text_fmt_[fmt_last];

  enum Brushes {
    brush_black,
    brush_red,
    brush_blue,
    brush_green,
    brush_text,
    brush_sline,
    brush_sel,
    brush_last
  };
  plx::ComPtr<ID2D1SolidColorBrush> brushes_[brush_last];

  plx::ComPtr<IDWriteTextLayout> title_layout_;
  std::unique_ptr<plx::FilePath> file_path_;

public:
  DCoWindow(int width, int height)
      : width_(width), height_(height),
        scroll_v_(0.0f),
        scale_(D2D1::Matrix3x2F::Scale(1.0f, 1.0f)) {
    // $$ read from config.
    margin_tl_ = D2D1::Point2F(22.0f, 36.0f);
    margin_br_ = D2D1::Point2F(16.0f, 16.0f);
    widget_pos_ = D2D1::Point2F(18.0f, 18.0f);
    widget_radius_ = D2D1::Point2F(8.0f, 8.0f);

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
        static_cast<unsigned int>(dpi().to_physical_x(width_)),
        static_cast<unsigned int>(dpi().to_physical_x(height_)));
    hr = root_visual_->SetContent(root_surface_.Get());
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);

    // create widget's geometry.
    circle_geom_close_ = CreateD2D1Geometry(d2d_factory_,
        D2D1::Ellipse(D2D1::Point2F(width_ - widget_pos_.x , widget_pos_.y),
                      widget_radius_.x, widget_radius_.y));
    widget_pos_.x += (widget_radius_.x * 2.0f ) + 8.0f;
    circle_geom_move_ = CreateD2D1Geometry(d2d_factory_,
        D2D1::Ellipse(D2D1::Point2F(width_ - widget_pos_.x , widget_pos_.y),
                      widget_radius_.x, widget_radius_.y));

    dwrite_factory_ = plx::CreateDWriteFactory();
    // create fonts.
    auto normal_sytle = plx::FontWSSParams::MakeNormal();
    text_fmt_[fmt_mono_text] =
        plx::CreateDWriteSystemTextFormat(dwrite_factory_, L"Consolas", 14.0f, normal_sytle);
    text_fmt_[fmt_prop_large] =
        plx::CreateDWriteSystemTextFormat(dwrite_factory_, L"Candara", 20.0f, normal_sytle);
    text_fmt_[fmt_title_right] =
        plx::CreateDWriteSystemTextFormat(dwrite_factory_, L"Consolas", 12.0f, normal_sytle);

    text_fmt_[fmt_title_right]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text_fmt_[fmt_title_right]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    {
      plx::ScopedD2D1DeviceContext dc(root_surface_, zero_offset, dpi(), nullptr);
      // create solid brushes.
      dc()->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, 1.0f), 
          brushes_[brush_black].GetAddressOf());
      dc()->CreateSolidColorBrush(D2D1::ColorF(0xBD4B5B, 1.0f), 
          brushes_[brush_red].GetAddressOf());
      dc()->CreateSolidColorBrush(D2D1::ColorF(0x1E5D81, 1.0f), 
          brushes_[brush_blue].GetAddressOf());
      dc()->CreateSolidColorBrush(D2D1::ColorF(RGB(74, 174, 0), 1.0),
          brushes_[brush_green].GetAddressOf());
      dc()->CreateSolidColorBrush(D2D1::ColorF(0xD68739, 1.0f), 
          brushes_[brush_text].GetAddressOf());
      dc()->CreateSolidColorBrush(D2D1::ColorF(0xD68739, 0.1f),
          brushes_[brush_sline].GetAddressOf());
      dc()->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::LightGray, 0.8f),
          brushes_[brush_sel].GetAddressOf());
    }
    
    update_title();
    update_screen();
  }

  void update_title(const std::wstring& title) {
    plx::Range<const wchar_t> r(&title[0], title.size());
    auto width = width_ - (widget_pos_.x + widget_radius_.x + 6.0f + margin_tl_.x);
    auto height = text_fmt_[fmt_title_right]->GetFontSize() * 1.2f;
    title_layout_ = plx::CreateDWTextLayout(dwrite_factory_,
        text_fmt_[fmt_title_right], r, D2D1::SizeF(width, height));
  }

  void update_title() {
    std::wstring title(L"scale: " + std::to_wstring(scale_._11).substr(0, 4) + L"  ");
    if (!file_path_) {
      title += ui_txt::no_file_title;
    } else {
      title += file_path_->raw();
    }
    update_title(title);
  }

  bool clipboard_copy() {
    return false;
  }

  bool clipboard_paste() {
    if (!::IsClipboardFormatAvailable(CF_TEXT))
      return false;
    plx::ScopedClipboard clipboard(window());
    if (!clipboard.did_open())
      return false;
    auto gmem = ::GetClipboardData(CF_UNICODETEXT);
    if (!gmem)
      return false;
    auto data = reinterpret_cast<wchar_t*>(::GlobalLock(gmem));
    if (!data)
      return false;
    std::wstring str(data);

    // remove CRs.
    auto last = std::remove(begin(str), end(str), L'\r');
    str.erase(last, end(str));
    
    // replace other control chars with ctrl-z
    for (wchar_t& c : str) {
      if (c >= 0x20)
        continue;
      if (c != L'\n')
        c = 0x1A;
    }

    // $$ here add the text.
    update_screen();
    ::GlobalUnlock(gmem);
    return true;
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
      case WM_LBUTTONDBLCLK: {
        return left_mouse_dblclick_handler(MAKEPOINTS(lparam));
      }
      case WM_MOUSEWHEEL: {
        return mouse_wheel_handler(HIWORD(wparam), LOWORD(wparam));
      }
      case WM_DPICHANGED: {
        return dpi_changed_handler(lparam);
      }
    }

    return ::DefWindowProc(window(), message, wparam, lparam);
  }

  void paint_handler() {
    // just recovery here when using direct composition.
  }

  LRESULT keydown_handler(int vkey) {
    if (vkey == VK_LEFT) {


    } else if (vkey == VK_RIGHT) {


    } else if (vkey == VK_UP) {

    }
    else if (vkey == VK_DOWN) {

    } else {
      return 0L;
    }

    ensure_cursor_in_view();
    update_screen();
    return 0L;
  }

  LRESULT ctrl_handler(wchar_t code) {
    switch (code) {
      case 0x03 :                 // ctrl-c.
        clipboard_copy();
        break;
      case 0x08 :                 // backspace.
        add_character(0x08);
        break;
      case 0x0A :                 // line feed.
      case 0x0D :                 // carriage return.
        add_character('\n');
        break;
      case 0x16:                  // ctrl-v.
        clipboard_paste();
        break;
      default:
        ; // $$$ beep or flash.
    }
    return 0L;
  }
    
  LRESULT char_handler(wchar_t code) {
    if (code < 0x20)
      return ctrl_handler(code);
    add_character(code);
    return 0L;
  }

  LRESULT left_mouse_button_handler(bool down, POINTS pts) {
    BOOL hit = 0;
    if (down) {
      // check hit for move window widget.
      circle_geom_move_->FillContainsPoint(
          D2D1::Point2F(pts.x, pts.y), D2D1::Matrix3x2F::Identity(), &hit);
      if (hit != 0) {
        ::SendMessageW(window(), WM_SYSCOMMAND, SC_MOVE|0x0002, 0);
      } else {
        // probably on the text. Move cursor there.
        if (move_cursor(pts))
          update_screen();
      }
      
    } else {
      circle_geom_close_->FillContainsPoint(
          D2D1::Point2F(pts.x, pts.y), D2D1::Matrix3x2F::Identity(), &hit);
      if (hit != 0) {
        ::PostQuitMessage(0);
      }
    }
    return 0L;
  }

  LRESULT left_mouse_dblclick_handler(POINTS pts) {

    update_screen();
    return 0L;
  }

  LRESULT mouse_move_handler(UINT_PTR state, POINTS pts) {
    return 0L;
  }

  LRESULT mouse_wheel_handler(int16_t offset, int16_t mk) {
    if (mk == MK_CONTROL) {
      // zoom.
      auto scalar_scale = scale_._11;
      if (offset < 0) {
        if (scalar_scale < 0.3f)
          return 0L;
        scalar_scale -= 0.1f;
      } else if (offset > 0) {
        if (scalar_scale > 2.4f)
          return 0L;
        scalar_scale += 0.1f;
      } else {
        return 0L;
      }

      scale_ = D2D1::Matrix3x2F::Scale(
          scalar_scale, scalar_scale, D2D1::Point2F(margin_tl_.x, 0));

      update_title();


    } else {
      // scroll.
      // $$ read the divisor from the config file.
      if ((offset > 0) && (scroll_v_ < -100.0f))
        return 0L;
      scroll_v_ -= offset / 4;
    }

    update_screen();
    return 0L;
  }

  LRESULT dpi_changed_handler(LPARAM lparam) {
    // $$ test this.
    plx::RectL r(plx::SizeL(
          static_cast<long>(dpi().to_physical_x(width_)),
          static_cast<long>(dpi().to_physical_x(height_))));
    
    auto suggested = reinterpret_cast<const RECT*> (lparam);
    ::AdjustWindowRectEx(&r, 
        ::GetWindowLong(window(), GWL_STYLE),
        FALSE,
        ::GetWindowLong(window(), GWL_EXSTYLE));
    ::SetWindowPos(window(), nullptr, suggested->left, suggested->top,
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

    }
    if (command_id == IDC_SAVE_PLAINTEXT) {
      PlainTextFileIO ptfio(plx::FilePath(L"c:\\test\\texto_file_out.txt"));
    }
    if (command_id == IDC_LOAD_PLAINTEXT) {
      FileOpenDialog dialog(window());
      if (!dialog.success())
        return 0L;
      
      PlainTextFileIO ptfio(dialog.path());
      
      file_path_ = std::make_unique<plx::FilePath>(dialog.path());
      update_title();
    }

    update_screen();
    return 0L;
  }

  void add_character(wchar_t ch) {
    bool needs_layout = false;

    if (ch == '\n') {
 
    } else if (ch == 0x08) {
      // deletion of one character.

    } else {
      // add a character in the current block.
    }

    update_screen();
  }


  void ensure_cursor_in_view() {

  }

  bool move_cursor(POINTS pts) {
    auto y = (pts.y / scale_._11)  + scroll_v_ - margin_tl_.y;
    auto x = (pts.x - margin_tl_.x) / scale_._11;

    return false;
  }

  void draw_marks(ID2D1DeviceContext* dc, IDWriteTextLayout* tl, const wchar_t* text) {
    uint32_t count = 0;
    auto hr = tl->GetClusterMetrics(nullptr,  0, &count);
    if (hr == S_OK)
      return;

    if (hr != E_NOT_SUFFICIENT_BUFFER) {
      __debugbreak();
    }
    std::vector<DWRITE_CLUSTER_METRICS> metrics(count);
    hr = tl->GetClusterMetrics(&metrics[0], count, &count);
    if (hr != S_OK) {
      __debugbreak();
    }

    uint32_t offset = 0;
    float width = 0.0f;
    float height = 0.0f;
    float x_offset = 0.0f;

    for (auto& cm : metrics) {
      ID2D1Brush* brush = nullptr;
      if (cm.isNewline) {
        brush = brushes_[brush_blue].Get();
        width = 3.0f;
        height = 3.0f;
        x_offset = 1.0f;
      } else if (cm.isWhitespace) {
        brush = brushes_[brush_blue].Get();
        height = 1.0f;
        if (cm.width == 0) {
          // control char (rare, possibly a bug).
          brush = brushes_[brush_green].Get();
          width = 2.0f;
          height = -5.0f;
          x_offset = cm.width / 3.0f;
        } else if (text[offset] == L'\t') {
          // tab.
          x_offset = cm.width / 8.0f;
          width = cm.width - (2 * x_offset); 
        } else {
          // space.
          width = 1.0f;
          x_offset = cm.width / 3.0f;
        }
      }

      if (brush) {
        DWRITE_HIT_TEST_METRICS hit_metrics;
        float x, y;
        hr = tl->HitTestTextPosition(offset, FALSE, &x, &y, &hit_metrics);
        if (hr != S_OK)
          __debugbreak();
        y += (2.0f * hit_metrics.height) / 3.0f;
        x += x_offset;
        dc->DrawRectangle(D2D1::RectF(x, y, x + width, y + height), brush);
      }
      offset += cm.length;
    }
  }

  void draw_caret(ID2D1DeviceContext* dc, IDWriteTextLayout* tl) {
    auto aa_mode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    DWRITE_HIT_TEST_METRICS hit_metrics;
    float x, y;
    auto hr = tl->HitTestTextPosition(0, FALSE, &x, &y, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    auto yf = y + hit_metrics.height;
    // caret.
    dc->DrawRectangle(
        D2D1::RectF(x, y, x + 2.0f, yf), brushes_[brush_red].Get(), 1.0f / scale_._11);
    dc->SetAntialiasMode(aa_mode);
    // active line.
    dc->FillRectangle(
        D2D1::RectF(0, y, static_cast<float>(width_), yf), brushes_[brush_sline].Get());
    dc->SetAntialiasMode(aa_mode);
  }

  void draw_selection(ID2D1DeviceContext* dc, IDWriteTextLayout* tl, Selection& sel) {
    if (sel.is_empty())
      return;
    auto aa_mode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    DWRITE_HIT_TEST_METRICS hit_metrics;
    float x0, y0;
    auto hr = tl->HitTestTextPosition(sel.start, FALSE, &x0, &y0, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    auto y1 = y0 + hit_metrics.height;
    auto x1 = x0 + hit_metrics.width;
    // active line.
    dc->FillRectangle(
        D2D1::RectF(x0, y0, x1, y1), brushes_[brush_sel].Get());
    dc->SetAntialiasMode(aa_mode);
  }

  void update_screen() {
    {
      D2D1::ColorF bk_color(0x000000, flag_options_[opacity_50_percent] ? 0.5f : 0.9f);
      plx::ScopedD2D1DeviceContext dc(root_surface_, zero_offset, dpi(), &bk_color);
      
      // draw widgets.
      dc()->DrawGeometry(circle_geom_move_.Get(), brushes_[brush_blue].Get(), 4.0f);
      dc()->DrawGeometry(circle_geom_close_.Get(), brushes_[brush_red].Get(), 4.0f);
      // draw title.
      dc()->DrawTextLayout(D2D1::Point2F(margin_tl_.x, widget_pos_.y - widget_radius_.y),
                         title_layout_.Get(), brushes_[brush_green].Get());

      dc()->SetTransform(scale_);

      // draw left margin.
      dc()->DrawLine(D2D1::Point2F(margin_tl_.x, margin_tl_.y),
                   D2D1::Point2F(margin_tl_.x, height_ - margin_br_.y),
                   brushes_[brush_blue].Get(), 0.5f);

      // draw the start of text line marker.
      if (scroll_v_ < 0) {
        dc()->SetTransform(D2D1::Matrix3x2F::Translation(
            margin_tl_.x, margin_tl_.y - scroll_v_) * scale_);
        dc()->DrawLine(D2D1::Point2F(0.0f, -8.0f),
                     D2D1::Point2F(static_cast<float>(width_), -8.0f),
                     brushes_[brush_blue].Get(), 0.5f);
      }

      // draw contents.
      float bottom = 0.0f;
      auto v_min = scroll_v_;
      auto v_max = scroll_v_ + static_cast<float>(height_) / scale_._11;

 
      auto trans = D2D1::Matrix3x2F::Translation(margin_tl_.x, margin_tl_.y - scroll_v_);
      dc()->SetTransform(trans * scale_);
      //draw_selection(..);
      //draw text ..
      
      // debugging visual aids.
      if (flag_options_[debug_text_boxes]) {
        //dc()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        //draw_marks(..);
      }

      // draw_caret(..);
    }
    dco_device_->Commit();
  }

};

#pragma endregion

HACCEL LoadAccelerators() {
  // $$ read this from the config file.
  ACCEL accelerators[] = {
    {FVIRTKEY, VK_F1, IDC_VIEW_HELP},
    {FVIRTKEY, VK_F2, IDC_LOAD_PLAINTEXT},
    {FVIRTKEY, VK_F3, IDC_SAVE_PLAINTEXT},
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
  } catch (plx::ComException& ex) {
    auto l = ex.Line();
    HardfailMsgBox(HardFailures::com_error, L"COM");
    return 2;
  }
}
