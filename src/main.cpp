#include "stdafx.h"
#include "resource.h"

// Ideas
// 1. Modified text (like VS ide) side column marker
// 2. Column guides (80 cols, etc)
// 3. Dropbox folder aware
// 4. Number of lines
// 5. darken the entire line of the cursor
// 6. be more permissive with the utf16 conversion
// 7. don't scroll past the top or bottom
// 8. save to input file

namespace ui_txt {
  const wchar_t no_file_title[] = L"TExTO v0.0.0a <no file> [F2: open]\n";
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

  bool is_last_block() const {
    return block_ >= (text_.size() - 1);
  }

  bool is_first_block() const {
    return block_ <= 0;
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
    if (!block_len())
      return remove_block();
    if (offset_ != 0)
      return erase_char();
    if (is_first_block())
      return false;
    else 
      return move_block_up();
  }

private:
  bool erase_char() {
    auto& txt = current_block().text;
    txt->erase(--offset_, 1);
    return true;
  }

  bool remove_block() {
    if (is_first_block())
      return false;
    auto it = begin(text_) + block_;
    text_.erase(it);
    --block_;
    offset_ = block_len();
    return true;
  }

  bool move_block_up() {
    auto& txt = current_block().text;
    std::wstring moved;
    auto lf = txt->find_first_of(L'\n');
    if (lf == std::wstring::npos) {
      // move the entire block.
      txt->swap(moved);
      auto it = begin(text_) + block_;
      text_.erase(it);
    } else {
      // move just a chunk of text.
      txt->substr(0, lf).swap(moved);
      txt->erase(0, lf + 1);
    }
    --block_;
    current_block().text->append(moved);
    offset_ = plx::To<uint32_t>(current_block().text->size() - moved.size());
    current_block().set_needs_layout();
    return true;
  }

  bool vertical_move(bool up_or_down, bool inner = false) {
    uint32_t line_count;
    auto hr = current_block().layout->GetLineMetrics(
        nullptr, 0, &line_count);
    if (hr != E_NOT_SUFFICIENT_BUFFER) {
      __debugbreak();
    }

    std::vector<DWRITE_LINE_METRICS> line_metrics(line_count);
    hr = current_block().layout->GetLineMetrics(
        &line_metrics.front(), line_count, &line_count);
    if (hr != S_OK)
      __debugbreak();

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

    if (inner) {
      // called from vertical_move() on block transition.
      if (!up_or_down) {
        offset_ = std::min(line_metrics[0].length, desired_col_);
      } else {
        offset_ = acc + std::min(line_metrics[line_no].length, desired_col_);
      }
      return true;
    }

    auto line_offset = desired_col_ ? desired_col_ : offset_ - acc;

    if (!up_or_down) {
      // going down ===
      if ((line_no + 1) == line_count) {
        // end of the block. Move to next block.
        if (is_last_block()) {
          offset_ = line_len;
          return false;
        }
        // across blocks we need to recurse to learn the offset.
        ++block_;
        offset_ = 0;
        desired_col_ = line_offset;
        return vertical_move(false, true);
      }

      auto& next_metrics = line_metrics[line_no + 1];
      if (next_metrics.length < line_offset) {
        // next line is shorter, remember the column so we can try again.
        desired_col_ = line_offset;
        line_offset = next_metrics.length - next_metrics.newlineLength;
      }

      offset_ = (acc + line_len) + line_offset;
    } else {
      // going up ===
      if (line_no == 0) {
        // end of the block. Move to previous block.
        if (is_first_block()) {
          offset_ = 0;
          return false;
        }
        // across blocks we need to recurse to learn the offset.
        --block_;
        offset_ = block_len() - 1;
        desired_col_ = line_offset;
        return vertical_move(true, true);
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
  // widgets insets from right side.
  D2D1_POINT_2F widget_pos_;
  D2D1_POINT_2F widget_radius_;

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

  plx::ComPtr<ID2D1Geometry> circle_geom_move_;
  plx::ComPtr<ID2D1Geometry> circle_geom_close_;

  plx::ComPtr<IDWriteFactory> dwrite_factory_;

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
    brush_last
  };
  plx::ComPtr<ID2D1SolidColorBrush> brushes_[brush_last];

  plx::ComPtr<IDWriteTextLayout> title_layout_;
  std::unique_ptr<plx::FilePath> file_path_;

public:
  DCoWindow(int width, int height)
      : width_(width), height_(height), cursor_(text_), scroll_v_(0.0f) {
    // $$ read from config.
    margin_tl_ = D2D1::Point2F(22.0f, 36.0f);
    margin_br_ = D2D1::Point2F(8.0f, 16.0f);
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
        static_cast<unsigned int>(dpi_.to_physical_x(width_)),
        static_cast<unsigned int>(dpi_.to_physical_x(height_)));
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

    // create fonts.
    dwrite_factory_ = plx::CreateDWriteFactory();
    text_fmt_[fmt_mono_text] = CreateDWriteTextFormat(dwrite_factory_, L"Consolas", 14.0f);
    text_fmt_[fmt_prop_large] = CreateDWriteTextFormat(dwrite_factory_, L"Candara", 20.0f);
    text_fmt_[fmt_title_right] = CreateDWriteTextFormat(dwrite_factory_, L"Consolas", 12.0f);

    text_fmt_[fmt_title_right]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text_fmt_[fmt_title_right]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    {
      ScopedDraw sd(root_surface_, dpi_);
      auto dc = sd.begin(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.9f), zero_offset);

      // create solid brushes.
      dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, 1.0f), 
          brushes_[brush_black].GetAddressOf());
      dc->CreateSolidColorBrush(D2D1::ColorF(0xBD4B5B, 1.0f), 
          brushes_[brush_red].GetAddressOf());
      dc->CreateSolidColorBrush(D2D1::ColorF(0x1E5D81, 1.0f), 
          brushes_[brush_blue].GetAddressOf());
      dc->CreateSolidColorBrush(D2D1::ColorF(RGB(74, 174, 0), 1.0),
          brushes_[brush_green].GetAddressOf());
      dc->CreateSolidColorBrush(D2D1::ColorF(RGB(57, 135, 214), 1.0f), 
          brushes_[brush_text].GetAddressOf());
    }
    
    if (!file_path_)
      update_title(plx::RangeFromLitStr(ui_txt::no_file_title));
    else {
      // $$ schedule a task to load the file.
    }

    update_screen();
  }

  void update_title(plx::Range<const wchar_t>& title) {
    auto width = width_ - (widget_pos_.x + widget_radius_.x + 6.0f + margin_tl_.x);
    auto height = text_fmt_[fmt_title_right]->GetFontSize() * 1.2f;
    title_layout_ = plx::CreateDWTextLayout(dwrite_factory_,
        text_fmt_[fmt_title_right], title, D2D1::SizeF(width, height));
  }

  void update_title(const plx::FilePath& path) {
    std::wstring title(path.raw());
    update_title(plx::Range<const wchar_t>(&title[0], title.size()));
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
    BOOL hit = 0;
    if (down) {
      circle_geom_move_->FillContainsPoint(
          D2D1::Point2F(pts.x, pts.y), D2D1::Matrix3x2F::Identity(), &hit);
      if (hit != 0) {
        ::SendMessageW(window(), WM_SYSCOMMAND, SC_MOVE|0x0002, 0);
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
      FileOpenDialog dialog(window());
      if (!dialog.success())
        return 0L;
      text_.clear();
      PlainTextFileIO ptfio(dialog.path());
      ptfio.load(text_);
      file_path_ = std::make_unique<plx::FilePath>(dialog.path());
      update_title(*file_path_);
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

  void draw_marks(ID2D1DeviceContext* dc, IDWriteTextLayout* tl) {
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
        width = 1.0f;
        height = 1.0f;
        x_offset = cm.width / 3.0f;
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

  void update_screen() {
    {
      ScopedDraw sd(root_surface_, dpi_);
      auto bk_alpha = flag_options_[opacity_50_percent] ? 0.5f : 0.9f;
      auto dc = sd.begin(D2D1::ColorF(0x000000, bk_alpha), zero_offset);
      // draw widgets.
      dc->DrawGeometry(circle_geom_move_.Get(), brushes_[brush_blue].Get(), 4.0f);
      dc->DrawGeometry(circle_geom_close_.Get(), brushes_[brush_red].Get(), 4.0f);
      // draw margin.
      dc->DrawLine(margin_tl_, D2D1::Point2F(margin_tl_.x, height_ - margin_br_.y),
                   brushes_[brush_blue].Get(), 0.5f);
      // draw title.
      dc->DrawTextLayout(D2D1::Point2F(margin_tl_.x, widget_pos_.y - widget_radius_.y),
                         title_layout_.Get(), brushes_[brush_green].Get());
      // draw contents.
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
          dc->SetTransform(D2D1::Matrix3x2F::Translation(
              margin_tl_.x,
              tb.metrics.top - scroll_v_ + margin_tl_.y));
          dc->DrawTextLayout(D2D1::Point2F(), tb.layout.Get(), brushes_[brush_text].Get()); 
          // debugging visual aids.
          if (flag_options_[debug_text_boxes]) {
            dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            auto debug_rect = 
                D2D1::RectF(tb.metrics.left, 0, tb.metrics.width, tb.metrics.height);
            dc->DrawRectangle(debug_rect, brushes_[brush_red].Get(), 1.0f);
            // show space and line breaks.
            draw_marks(dc.Get(), tb.layout.Get());
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
  } catch (plx::ComException&) {
    HardfailMsgBox(HardFailures::com_error, L"COM");
    return 2;
  }
}
