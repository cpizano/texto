#include "stdafx.h"
// TExTO.  Copyright 2014, Carlos Pizano (carlos.pizano@gmail.com)
// TExTO is a text editor prototype.
//
// Technology-wise it is a big experiment because it does not use GDI, OpenGL or DirectX at all.
// Everything is done via DirectComposition, DirectWrite or Direct2D. In particular the WM_PAINT
// message does nothing paint related. In order to understand how things are done, you probably
// need to read Kenny Kerr tutorials on all these technologies.
//
// TExTO, as a text editor, is still finding out who she is. It should straddle between a better
// notepad and probably a worse Notepad++.

class TextView {
  D2D1_SIZE_F box_;
  uint32_t cursor_;
  uint32_t v_scroll_;
  size_t active_start_;
  size_t active_end_;
  std::unique_ptr<std::wstring> full_text_;
  std::wstring active_text_;
  plx::ComPtr<IDWriteTextLayout> dwrite_layout_;
  plx::ComPtr<IDWriteFactory> dwrite_factory_;
  plx::ComPtr<IDWriteTextFormat> dwrite_fmt_;

  TextView& operator=(const TextView&) = delete;
  TextView(const TextView&) = delete;

public:
  TextView(plx::ComPtr<IDWriteFactory> dwrite_factory) 
      : box_(D2D1::SizeF()),
        cursor_(0),
        v_scroll_(0),
        active_start_(0), active_end_(0),
        dwrite_factory_(dwrite_factory) {
  }

  void set_size(uint32_t width, uint32_t height) {
    box_ = D2D1::SizeF(static_cast<float>(width), static_cast<float>(height));
    invalidate();
  }

  void set_text_fmt(plx::ComPtr<IDWriteTextFormat> dwrite_fmt) {
    dwrite_fmt_ = dwrite_fmt;
    invalidate();
  }

  void set_full_text(std::unique_ptr<std::wstring> text) {
    full_text_ = std::move(text);
    slice_active_text(0);
  }

  void set_cursor(uint32_t pos) {
    if (pos > active_text_.size())
      __debugbreak();
    cursor_ = pos;
  }

  void set_v_scroll(uint32_t v_scroll) {
    auto metrics = get_metrics();
    auto left =  v_scroll - metrics.lineCount;

    v_scroll_ = v_scroll;
  }

  void insert_char(wchar_t c) {
    active_text_.insert(cursor_, 1, c);
    ++cursor_;
    update_layout();
  }

  bool back_erase() {
    if (cursor_ <= 0)
      return false;
    active_text_.erase(--cursor_, 1);
    update_layout();
    return true;
  }

  void draw(ID2D1DeviceContext* dc,
            ID2D1Brush* text_brush,
            ID2D1Brush* caret_brush,
            ID2D1Brush* line_brush) {
    if (!dwrite_layout_)
      update_layout();
    dc->DrawTextLayout(D2D1::Point2F(), dwrite_layout_.Get(), text_brush);
    draw_caret(dc, caret_brush, line_brush);
  }

private:
  void slice_active_text(size_t from) {
    const size_t slice_size = 8 * 1024;  // $$ estimate it based on box_;
    active_text_ = full_text_->substr(from, from + slice_size);
    active_start_ = from;
    active_end_ = from + active_text_.size();
    update_layout();
  }

  void invalidate() {
    dwrite_layout_.Reset();
  }

  void update_layout() {
    plx::Range<const wchar_t> txt(active_text_.c_str(), active_text_.size());
    dwrite_layout_ = plx::CreateDWTextLayout(dwrite_factory_, dwrite_fmt_, txt, box_);
  }

  void draw_caret(ID2D1DeviceContext* dc, ID2D1Brush* caret_brush, ID2D1Brush* line_brush) {
    auto aa_mode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    DWRITE_HIT_TEST_METRICS hit_metrics;
    float x, y;
    auto hr = dwrite_layout_->HitTestTextPosition(cursor_, FALSE, &x, &y, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    auto yf = y + hit_metrics.height;
    // caret.
    dc->DrawRectangle(
        D2D1::RectF(x, y, x + 2.0f, yf), caret_brush, 1.0f);
    dc->SetAntialiasMode(aa_mode);
    // active line.
    dc->FillRectangle(
        D2D1::RectF(0, y, box_.width, yf), line_brush);
    dc->SetAntialiasMode(aa_mode);
  }

  DWRITE_TEXT_METRICS get_metrics() {
    DWRITE_TEXT_METRICS metrics;
    auto hr = dwrite_layout_->GetMetrics(&metrics);
    if (hr != S_OK)
      __debugbreak();
    return metrics;
  }

};



//////////////////////////////////////////////////////////////////////////////////////////////////
//  old design
//
// Offsets to text that is selected. Not meaniful without a TextBlock.
struct Selection {
  uint32_t start;
  uint32_t end;

  Selection(): start(0), end(0) {}
  bool is_empty() const { return start == end; }
  void clear() { start = end = 0; }
};

// A TextBlock is chunck of text that is rendered in a rectangle. What the user sees on screen
// is a series of TextBlocks one afer another with no overlap between them. It exists because it
// is not reasonable to store the text as a single string and on the other hand the DirectWrite API
// does not consume something like Span Tables [1], therefore we are forced to have what ammounts
// to a vector of paragraphs.
//
// For performance reasons |text| is always up to date, but |layout| is computed when needed and
// |metrics| is computed by an up to date |layout|.
//
// [1] see http://www.catch22.net/tuts/memory-techniques-part-2
//
struct TextBlock {
  enum class State : int {
    none,
    regular,
    eof
  };

  static const size_t target_size = 512;

  std::shared_ptr<std::wstring> text;
  plx::ComPtr<IDWriteTextLayout> layout;
  DWRITE_TEXT_METRICS metrics;
  State state;
  Selection selection;
  
  TextBlock() 
      : text(std::make_shared<std::wstring>()),
        state(State::regular) {
  }

  TextBlock(const std::wstring& txt) 
      : text(std::make_shared<std::wstring>(txt)),
        state(State::regular) {
  }

  bool needs_layout() const { return layout.Get() == nullptr; }
  void set_needs_layout() { layout.Reset(); }

  bool eof_block() const { return state == State::eof; }
};


// The cursor controls where the caret is and has initmate knowledge of the array-of-textblocks
// and encapsulates all the nasty corner cases of managing the cursor movement and text insertion
// and deletion.
class Cursor {
  uint32_t block_;
  uint32_t offset_;
  uint32_t desired_col_;
  std::vector<uint32_t> sel_blocks_;
  std::vector<TextBlock>& text_;

public:
  Cursor(std::vector<TextBlock>& text) 
      : block_(0), offset_(0), desired_col_(0),
        text_(text) {
  }

  void init() {
    block_ = 0;
    offset_ = 0;
    // The minimal configuration has a regular textblock and the end of file block.
    if (text_.empty())
      text_.resize(1);
    TextBlock tb;
    tb.state = TextBlock::State::eof;
    text_.push_back(tb);
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
    return text_[block_ + 1].eof_block();
  }

  bool is_first_block() const {
    return block_ <= 0;
  }

  bool move_to(uint32_t block, uint32_t offset) {
    if (block >= text_.size())
      return false;
    block_ = block;
    offset_ = offset > block_len() ? block_len() : offset;
    return true;
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

  std::wstring insert_run(const std::wstring& str) {
    current_block().set_needs_layout();
    auto txt = current_block().text;
    auto prev_offset = offset_;
    // no matter what, we are going to create a new block and move
    // the cursor over there.
    ++block_;
    text_.insert(begin(text_) + block_, TextBlock());
    offset_ = 0;

    if (txt->empty()) {
      *txt = str;
      return std::wstring();
    } else {
      auto leftover = txt->substr(prev_offset, std::wstring::npos);
      txt->erase(prev_offset, leftover.size());
      txt->append(str);
      return leftover;
    }
  }

  void add_string(const std::wstring& str, bool use_insert) {
    size_t brk = 0;
    size_t start = 0;
    std::wstring first_block_leftover;

    size_t insert_count = 0;
    while (true) {
      brk = str.find_first_of(L'\n', brk);

      if (brk == std::wstring::npos) {
        // last block.
        if (use_insert) {
          if (insert_count == 0) {
            current_block().text->insert(offset_, str);
            current_block().set_needs_layout();
          } else {
            insert_run(str.substr(start, brk) + first_block_leftover);
          }
        } else {
          text_.emplace_back(str.substr(start, brk));
        }
        return;
      }

      auto sz = brk - start;
      if (sz < TextBlock::target_size) {
        // input block size is small, advance half the difference.
        auto offset = TextBlock::target_size - sz;
        brk += (offset > 20) ? offset / 2 : 10;
        continue;
      } 

      // good enough size, add text as a new block.
      if (use_insert) {
        if (insert_count++ == 0)
          first_block_leftover = insert_run(str.substr(start, sz));
        else
          insert_run(str.substr(start, sz));
      } else {
        text_.emplace_back(str.substr(start, sz));
      }
      start = brk + 1;
      ++brk;
    }
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

  // make the char at the cursor selected.
  wchar_t select() {
    if (!block_len())
      return 0;
    if (offset_ == block_len())
      return 0;

    clear_selection();
    auto& cb = current_block();
    cb.selection.start = offset_;
    cb.selection.end = offset_ + 1;
    sel_blocks_.push_back(block_number());

    return cb.text->at(offset_);
  }

  Selection selection() const {
    return current_block().selection;
  }

  void clear_selection() {
    for (auto ix : sel_blocks_)
      text_[ix].selection.clear();
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
