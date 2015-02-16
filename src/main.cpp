// This file is the main driver for the TExTO application. For more details see
// texto.h.

#include "stdafx.h"
#include "resource.h"
#include "texto.h"
#include "file_io.h"

// Ideas and Bugs:
// 1.  modified text (like VS ide) side column marker.
// 2.  column guides (80 cols, etc).
// 3.  dropbox folder aware.
// 4.  number of lines.
// 7.  don't scroll past the bottom.
// 8.  save to input file.
// 9.  spellchecker.
// 10. find in text.
// 13. text stats above text.
// 14. pg up and pg down.
// 16. make selection with keyboard only.
// 17. home to start of line, home again to begin of paragraph.
// 18. end to end of line, end again to end of paragraph.
// 19. help screen or pane.
// 20. ctrl + left / right moves over words.
// 21. deleting needs to redo the active_text.

template <typename T> int sgn(T val) {
  return (T(0) < val) - (val < T(0));
}

namespace ui_txt {
  const wchar_t no_file_title[] = L"TExTO v0.0.2a <no file> [F2: open]\n";
}

// force plex to generate these two symbols since file_io.h is not fed to
// the generator yet.
void* f00 = &plx::UTF8FromUTF16;
void* f01 = &plx::UTF16FromUTF8;

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

plx::ComPtr<ID2D1Geometry> CreateD2D1Geometry(
    plx::ComPtr<ID2D1Factory2> d2d1_factory,
    const D2D1_ELLIPSE& ellipse) {
  plx::ComPtr<ID2D1EllipseGeometry> geometry;
  auto hr = d2d1_factory->CreateEllipseGeometry(ellipse, geometry.GetAddressOf());
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);
  return geometry;
}

plx::ComPtr<ID2D1Geometry> CreateD2D1Geometry(
    plx::ComPtr<ID2D1Factory2> d2d1_factory,
    const D2D1_ROUNDED_RECT& rect) {
  plx::ComPtr<ID2D1RoundedRectangleGeometry> geometry;
  auto hr = d2d1_factory->CreateRoundedRectangleGeometry(rect, geometry.GetAddressOf());
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);
  return geometry;
}

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
  D2D1::Matrix3x2F scale_;

  std::wstring last_title_;

  plx::ComPtr<ID3D11Device> d3d_device_;
  plx::ComPtr<ID2D1Factory2> d2d_factory_;
  plx::ComPtr<ID2D1Device> d2d_device_;
  plx::ComPtr<IDCompositionDesktopDevice> dco_device_;
  plx::ComPtr<IDCompositionTarget> dco_target_;

  plx::ComPtr<IDCompositionVisual2> root_visual_;
  plx::ComPtr<IDCompositionSurface> root_surface_;

  plx::ComPtr<IDCompositionVisual2> hover_visual_;
  plx::ComPtr<IDCompositionSurface> hover_surface_;

  plx::ComPtr<IDWriteFactory> dwrite_factory_;

  // widgets.
  plx::ComPtr<ID2D1Geometry> geom_move_;
  plx::ComPtr<ID2D1Geometry> geom_close_;

  enum TxtFromat {
    fmt_mono_text,
    fmt_prop_large,
    fmt_title_right,
    fmt_last
  };

  plx::ComPtr<IDWriteTextFormat> text_fmt_[fmt_last];

  enum BrushesMain {
    brush_red,
    brush_blue,
    brush_frame,
    brush_sel,
    brush_drag,
    brush_last
  };

  enum BrushesHover {
    brush_hv_text,
    brush_hv_last
  };

  plx::D2D1BrushManager brushes_;
  plx::D2D1BrushManager text_brushes_;
  plx::D2D1BrushManager hover_brushes_;

  plx::ComPtr<IDWriteTextLayout> title_layout_;
  std::unique_ptr<plx::FilePath> file_path_;

  std::unique_ptr<TextView> textview_;

public:
  DCoWindow(int width, int height)
      : width_(width), height_(height),
        scroll_v_(0.0f),
        scale_(D2D1::Matrix3x2F::Scale(1.0f, 1.0f)),
        brushes_(brush_last),
        text_brushes_(TextView::brush_last),
        hover_brushes_(brush_hv_last) {

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
    geom_close_ = CreateD2D1Geometry(d2d_factory_,
        D2D1::Ellipse(D2D1::Point2F(width_ - widget_pos_.x , widget_pos_.y),
                      widget_radius_.x, widget_radius_.y));
    widget_pos_.x += (widget_radius_.x * 2.0f ) + 8.0f;

    geom_move_ = CreateD2D1Geometry(d2d_factory_,
        D2D1::RoundedRect(D2D1::RectF(margin_tl_.x, 0, width_ - widget_pos_.x, 16),
                          3.0f, 3.0f));

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
      brushes_.set_solid(dc(), brush_red, 0xBD4B5B, 1.0f);
      brushes_.set_solid(dc(), brush_blue, 0x1E5D81, 1.0f);
      brushes_.set_solid(dc(), brush_frame, 0x00AE4A, 1.0f);
      brushes_.set_solid(dc(), brush_drag, 0x1E5D81, 0.4f);

      text_brushes_.set_solid(dc(), TextView::brush_text, 0xD68739, 1.0f);
      text_brushes_.set_solid(dc(), TextView::brush_caret, 0xBD4B5B, 1.0f);
      text_brushes_.set_solid(dc(), TextView::brush_line, 0x242424, 0.4f);
      text_brushes_.set_solid(dc(), TextView::brush_control, 0xBD4B5B, 1.0f);
      text_brushes_.set_solid(dc(), TextView::brush_lf, 0x1E5D81, 1.0f);
      text_brushes_.set_solid(dc(), TextView::brush_space, 0x1E5D81, 1.0f);
      text_brushes_.set_solid(dc(), TextView::brush_selection, 0x006400, 0.8f);
      text_brushes_.set_solid(dc(), TextView::brush_find, 0x9A2ED2, 0.7f);
    }

    make_textview(nullptr);
    update_screen();
  }

  void update_title(std::wstring& title) {
    if (title == last_title_)
      return;
    plx::Range<const wchar_t> r(&title[0], title.size());
    auto width = width_ - (widget_pos_.x + widget_radius_.x + 6.0f + margin_tl_.x);
    auto height = text_fmt_[fmt_title_right]->GetFontSize() * 1.2f;
    title_layout_ = plx::CreateDWTextLayout(dwrite_factory_,
        text_fmt_[fmt_title_right], r, D2D1::SizeF(width, height));
    last_title_.swap(title);
  }

  void update_title() {
    std::wstring title(
        L"cur: " + std::to_wstring(textview_->cursor()) +
        L" pos: " + std::to_wstring(textview_->start()) +
        L" scale: " + std::to_wstring(scale_._11).substr(0, 4) +  L"  ");
    if (!file_path_) {
      title += ui_txt::no_file_title;
    } else {
      title += file_path_->raw();
    }
    update_title(title);
  }

  void create_hover(int width, int height) {
    hover_surface_ = plx::CreateDCoSurface(
        dco_device_,
        static_cast<unsigned int>(dpi().to_physical_x(width)),
        static_cast<unsigned int>(dpi().to_physical_x(height)));

    hover_visual_ = plx::CreateDCoVisual(dco_device_);
    hover_visual_->SetContent(hover_surface_.Get());
    root_visual_->AddVisual(hover_visual_.Get(), TRUE, nullptr);

    {
      plx::ScopedD2D1DeviceContext dc(hover_surface_, zero_offset, dpi(), nullptr);
      hover_brushes_.set_solid(dc(), brush_hv_text, 0xD68739, 1.0f);
    }
  }

  void close_hover() {
    hover_brushes_.release_all();
    hover_surface_.Reset();
    root_visual_->RemoveVisual(hover_visual_.Get());
    hover_visual_.Reset();
  }

  bool clipboard_copy() {
    auto str = textview_->get_selection();
    if (str.empty())
      return false;
    plx::ScopedClipboard clipboard(window());
    if (!clipboard.did_open())
      return false;
    ::EmptyClipboard();
    auto sz_bytes = (str.size() + 1) * sizeof(wchar_t);
    auto gh = ::GlobalAlloc(GMEM_MOVEABLE, sz_bytes);
    if (!gh)
      return false;
    memcpy(::GlobalLock(gh), str.c_str(), sz_bytes);
    ::GlobalUnlock(gh);
    ::SetClipboardData(CF_UNICODETEXT, gh);
    return true;
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

    textview_->insert_text(str);
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
      textview_->move_cursor_left();
    } else if (vkey == VK_RIGHT) {
      textview_->move_cursor_right();
    } else if (vkey == VK_UP) {
      textview_->move_cursor_up();
    }
    else if (vkey == VK_DOWN) {
      textview_->move_cursor_down();
    } else {
      return 0L;
    }

    update_screen();
    return 0L;
  }

  LRESULT ctrl_handler(wchar_t code) {
    switch (code) {
      case 0x03 :                 // ctrl-c.
        clipboard_copy();
        break;
      case 0x08 :                 // backspace.
        textview_->back_erase();
        update_screen();
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
    // check the close button.
    geom_close_->FillContainsPoint(
        D2D1::Point2F(pts.x, pts.y), D2D1::Matrix3x2F::Identity(), &hit);
    if (hit != 0) {
      if (!down)
        ::PostQuitMessage(0);
      return 0L;
    }

    if (!down)
      return 0L;

    // Not in the close. check hit for move window widget.
    geom_move_->FillContainsPoint(
        D2D1::Point2F(pts.x, pts.y), D2D1::Matrix3x2F::Identity(), &hit);
    if (hit != 0) {
      ::SendMessageW(window(), WM_SYSCOMMAND, SC_MOVE|0x0002, 0);
    } else {
      // probably on the text. Move cursor there.
      if (move_cursor(pts, false))
        update_screen();
    }
    
    return 0L;
  }

  LRESULT left_mouse_dblclick_handler(POINTS pts) {
    textview_->select_word();
    update_screen();
    return 0L;
  }

  LRESULT mouse_move_handler(UINT_PTR state, POINTS pts) {
    if (state & MK_LBUTTON) {
      // change selection.
      if (move_cursor(pts, true))
        update_screen();
    }
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
      set_textview_size();

    } else {
      // scroll.
      // $$ read the divisor from the config file.
      auto lines = scale_._11 < 0.7 ? 5 : 2;
      textview_->v_scroll(sgn(-offset) * lines);
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
      FileSaveDialog dialog(window());
      if (!dialog.success())
        return 0L;

      PlainTextFileIO ptfio(dialog.path());
      ptfio.save(textview_->get_full_text());
      return 0L;
    }
    if (command_id == IDC_LOAD_PLAINTEXT) {
      FileOpenDialog dialog(window());
      if (!dialog.success())
        return 0L;
      
      PlainTextFileIO ptfio(dialog.path());
      make_textview(ptfio.load().release());
      
      file_path_ = std::make_unique<plx::FilePath>(dialog.path());
    }
    if (command_id == IDC_FIND) {
      find_control();
    }

    update_screen();
    return 0L;
  }

  void set_textview_size() {
    auto w = (width_ - (22 + 6));
    auto h = (height_ - (36 + 16)) / scale_._11;
    textview_->set_size(w, static_cast<uint32_t>(h));
  }

  void make_textview(std::wstring* text) {
    textview_ = std::make_unique<TextView>(dwrite_factory_, text_fmt_[fmt_mono_text], text);
    set_textview_size();
  }

  void add_character(wchar_t ch) {
    // add a character in the current block.
    textview_->insert_char(ch);
    update_screen();
  }

  bool move_cursor(POINTS pts, bool is_selection) {
    auto y = (pts.y / scale_._11) - margin_tl_.y;
    auto x = (pts.x - margin_tl_.x) / scale_._11;
    if (is_selection) {
      textview_->change_selection(x, y);
    } else {
      textview_->move_cursor_to(x, y);
    }
    return true;
  }

  void find_control() {
    if (hover_surface_) {
      close_hover();
      return;
    } 
    
    create_hover(200, 200);
    hover_visual_->SetOffsetX(static_cast<float>(width_ - 220));
    hover_visual_->SetOffsetY(margin_tl_.y);
  }

  void draw_frame(ID2D1DeviceContext* dc) {
    // draw widgets.
    dc->FillGeometry(geom_move_.Get(), brushes_.solid(brush_drag));
    dc->DrawGeometry(geom_close_.Get(), brushes_.solid(brush_red), 4.0f);
    // draw title.
    dc->DrawTextLayout(D2D1::Point2F(margin_tl_.x, widget_pos_.y - widget_radius_.y),
                       title_layout_.Get(), brushes_.solid(brush_frame));
    // draw left margin.
    dc->DrawLine(D2D1::Point2F(margin_tl_.x, margin_tl_.y),
                 D2D1::Point2F(margin_tl_.x, height_ - margin_br_.y),
                 brushes_.solid(brush_blue), 0.5f);
  }

  void update_screen() {
    update_title();

    if (root_surface_) {
      D2D1::ColorF bk_color(0x000000, flag_options_[opacity_50_percent] ? 0.5f : 0.9f);
      plx::ScopedD2D1DeviceContext dc(root_surface_, zero_offset, dpi(), &bk_color);
      draw_frame(dc());

      auto trans = D2D1::Matrix3x2F::Translation(margin_tl_.x, margin_tl_.y);
      dc()->SetTransform(trans * scale_);

      // draw the start of text line marker.
      if (scroll_v_ <= 0) {
        dc()->DrawLine(D2D1::Point2F(0.0f, -8.0f),
                       D2D1::Point2F(static_cast<float>(width_), -8.0f),
                       brushes_.solid(brush_blue), 0.5f);
      }
 
      auto mode = flag_options_[debug_text_boxes] ? TextView::show_marks : TextView::normal;
      textview_->draw(dc(), text_brushes_, mode);
    }

    if (hover_surface_) {
      D2D1::ColorF bk_color(0x000000, 0.1f);
      plx::ScopedD2D1DeviceContext dc(hover_surface_, zero_offset, dpi(), &bk_color);
      dc()->DrawRectangle(
          D2D1::RectF(0.0f, 0.0f, 50.0f, 50.0f), hover_brushes_.solid(brush_hv_text));
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
    {FVIRTKEY, VK_F9, IDC_ALT_FONT},
    {FVIRTKEY|FCONTROL, 'F', IDC_FIND}
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
