#include "stdafx.h"
#include "resource.h"

// Ideas
// 1. Modified text (like VS ide) side column marker
// 2. Column guides (80 cols, etc)
// 3. Dropbox folder aware
// 4. Number of lines
// 5. darken the entire line of the cursor
//

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

plx::ComPtr<IDWriteTextFormat> CreateDWriteTextFormat(
    plx::ComPtr<IDWriteFactory> dw_factory,
    const wchar_t* font_family, float size,
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL,
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL) {
  plx::ComPtr<IDWriteTextFormat> format;
  auto hr = dw_factory->CreateTextFormat(
      font_family, nullptr, weight, style, stretch, size, L"", format.GetAddressOf());
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);
  return format;
}

#pragma region text_management

struct TextBlock {
  static const size_t target_size = 512;

  std::shared_ptr<std::wstring> text;
  plx::ComPtr<IDWriteTextLayout> layout;
  DWRITE_TEXT_METRICS metrics;

  TextBlock() : text(std::make_shared<std::wstring>()) {
    set_needs_layout();
  }

  TextBlock(const std::wstring& txt) : text(std::make_shared<std::wstring>(txt)) {
    set_needs_layout();
  }

  bool needs_layout() const { return metrics.lineCount == -1; }
  void set_needs_layout() { metrics.lineCount = -1; }
};

class Cursor {
  uint32_t block_;
  uint32_t offset_;
  uint32_t desired_col_;
  std::vector<TextBlock>& text_;

public:
  Cursor(std::vector<TextBlock>& text) 
      : block_(0), offset_(0), desired_col_(0),
        text_(text) {
    text_.resize(1);
  }

  uint32_t block_len() const {
    return plx::To<uint32_t>(current_block().text->size());
  }

  uint32_t current_offset() const {
    return offset_;
  }

  uint32_t block_number() const {
    return block_;
  }

  bool move_left() {
    desired_col_ = 0;
    if (offset_ > 0) {
      --offset_;
    } else if (!is_first_block()) {
      --block_;
      offset_ = block_len();
    } else {
      return false;
    }
    return true;
  }

  bool move_right() {
    desired_col_ = 0;
    if (offset_ < block_len()) {
      ++offset_;
    } else if (!is_last_block()) {
      ++block_;
      offset_ = 0;
    } else {
      return false;
    }
    return true;
  }

  bool move_down() {
    return vertical_move(false);
  }

  bool move_up() {
    return vertical_move(true);
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
    current_block().text->insert(offset_, 1, ch);
    ++offset_;
  }

  bool remove_char() {
    auto& txt = current_block().text;
    if (offset_ == 0)
      return false;   // $$$ handle erasure here.

    if (!txt->empty()) {
      txt->erase(--offset_, 1);
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

  bool is_last_block() const {
    return block_ >= (text_.size() - 1);
  }

  bool is_first_block() const {
    return block_ <= 0;
  }

private:
  bool vertical_move(bool up_or_down) {
    std::vector<DWRITE_LINE_METRICS> line_metrics;
    auto& cb = current_block();
    if (cb.metrics.lineCount == -1)
      __debugbreak();

    line_metrics.resize(cb.metrics.lineCount);
    uint32_t actual_line_count;
    auto hr = cb.layout->GetLineMetrics(
        &line_metrics.front(), cb.metrics.lineCount, &actual_line_count);

    if (actual_line_count == 0)
      return false;

    uint32_t acc = 0;
    size_t line_no = 0;
    uint32_t line_len = 0;
    for (auto& lm : line_metrics) {
      if ((acc + lm.length) > offset_) {
        line_len = lm.length;
        break;
      }
      acc += lm.length;
      ++line_no;
    }

    if (!line_len) {
      if (acc == offset_)
        --line_no;
      else
        __debugbreak();
    }

    auto line_offset = desired_col_ ? desired_col_ : offset_ - acc;

    if (!up_or_down) {
      // going down =============================================
      if ((line_no + 1) == cb.metrics.lineCount) {
        // end of the block. Move to next block.
        // $$ this is mostly wrong.
        if (is_last_block()) {
          offset_ = line_len;
          return false;
        }
        ++block_;
        offset_ = line_offset;
        return true;
      }

      auto& next_metrics = line_metrics[line_no + 1];
      if (next_metrics.length < line_offset) {
        // next line is shorter, remember the column so we can try again.
        desired_col_ = line_offset;
        line_offset = next_metrics.length - next_metrics.newlineLength;
      }

      offset_ = (acc + line_len) + line_offset;
    } else {
      // going up ================================================
      if (line_no == 0) {
        // end of the block. Move to previous block.
        if (is_first_block()) {
          offset_ = 0;
          return false;
        }
        --block_;
        offset_ = 0;
        return true;
      }

      auto& prev_metrics = line_metrics[line_no - 1];
      if (prev_metrics.length < line_offset) {
        // prev line is shorter, emember the column so we can try again.
        desired_col_ = line_offset;
        line_offset = prev_metrics.length - prev_metrics.newlineLength;
      }

      offset_ = (acc - prev_metrics.length) + line_offset;
    }

    return true;
  }
};

#pragma endregion

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

  void load(std::vector<TextBlock>& text) {
    auto file = plx::File::Create(
        path_, 
        plx::FileParams::ReadWrite_SharedRead(OPEN_EXISTING),
        plx::FileSecurity());

    // need to read the whole file at once because the UTF16 conversion fails if
    // we end up in the middle of multi-byte sequence.
    std::wstring fulltext = file_from_disk(file);

    size_t brk = 0;
    size_t start = 0;

    while (true) {
      brk = fulltext.find_first_of(L'\n', brk);

      if (brk == std::wstring::npos) {
        // last block.
        text.emplace_back(fulltext.substr(start, brk));
        return;
      }

      auto sz = brk - start;
      if (sz < TextBlock::target_size) {
        // current block size is small, advance half the difference.
        auto offset = TextBlock::target_size - sz;
        brk += (offset > 20) ? offset / 2 : 10;
        continue;
      }

      text.emplace_back(fulltext.substr(start, sz));
      start = brk + 1;
      ++brk;
    }

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
    return plx::UTF16FromUTF8(plx::Range<const uint8_t>(block.start(), bytes_read));
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

    dwrite_factory_ = plx::CreateDWriteFactory();
    text_fmt_[0] = CreateDWriteTextFormat(dwrite_factory_, L"Candara", 20.0f);
    text_fmt_[1] = CreateDWriteTextFormat(dwrite_factory_, L"Consolas", 14.0f);

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
      auto title_fmt = CreateDWriteTextFormat(dwrite_factory_, L"Candara", 34.0f);
      auto txt = plx::RangeFromLitStr(ui_txt::gretting);
      auto size = D2D1::SizeF(static_cast<float>(width_), static_cast<float>(height_));
      auto greetings = plx::CreateDWTextLayout(dwrite_factory_, title_fmt, txt, size);

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
    if (vkey == VK_UP) {
      cursor_.move_up();
    }
    if (vkey == VK_DOWN) {
      cursor_.move_down();
    }

    ensure_cursor_in_view();
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
      PlainTextFileIO ptfio(plx::FilePath(L"c:\\test\\texto_file_out.txt"));
      ptfio.save(text_);
    }
    if (command_id == IDC_LOAD_PLAINTEXT) {
      text_.clear();
      PlainTextFileIO ptfio(plx::FilePath(L"c:\\test\\texto_file_in.txt"));
      ptfio.load(text_);
    }

    update_screen();
    return 0L;
  }

  void add_character(wchar_t ch) {
    bool needs_layout = false;

    auto& block = cursor_.current_block();
    if (ch == '\n') {
      // new line.
      if (cursor_.block_len() >= TextBlock::target_size) {
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
    plx::Range<const wchar_t> txt(block.text->c_str(), block.text->size());
    auto fmt_index = flag_options_[alternate_font] ? 1 : 0;
    block.layout = plx::CreateDWTextLayout(dwrite_factory_, text_fmt_[fmt_index], txt, box);
    auto hr = block.layout->GetMetrics(&block.metrics);
    if (hr != S_OK) {
      __debugbreak();
    }
  }

  void layout_all() {
    for (auto& tb : text_) {
      tb.set_needs_layout();
    }
  }

  void ensure_cursor_in_view() {
    auto& cb = cursor_.current_block();
    if (cb.needs_layout())
      return;

    DWRITE_HIT_TEST_METRICS hit_metrics;
    float x, y;
    auto hr = cb.layout->HitTestTextPosition(
        cursor_.current_offset(), FALSE, &x, &y, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);

    y += cb.metrics.top;
    auto actual_y = margin_tl_.y + y - scroll_v_;

    if (actual_y > (height_ - margin_br_.y))
      scroll_v_ = y - height_ + margin_tl_.y + margin_br_.y;

    if (actual_y < margin_tl_.y)
      scroll_v_ = y; 
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
      bool painting = false;

      for (auto& tb : text_) {
        if (tb.needs_layout())
          layout(tb);

        tb.metrics.top = bottom;
        bottom += tb.metrics.height;

        if (((bottom > v_min) && (bottom < v_max)) || 
            ((tb.metrics.top > v_min) && (tb.metrics.top < v_max)) ||
            ((tb.metrics.top < v_min) && (bottom  > v_max))) {
          painting = true;
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
        } else if (painting) {
          // we went outside of the viewport, no need to do more work right now.
          break;
        }
        ++block_number;
      }
    }
    dco_device_->Commit();
  }

};

#pragma endregion

HACCEL LoadAccelerators() {
  // $$ read this from the config file.
  ACCEL accelerators[] = {
    {FVIRTKEY, VK_F1, IDC_SAVE_PLAINTEXT},
    {FVIRTKEY, VK_F2, IDC_LOAD_PLAINTEXT},
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
