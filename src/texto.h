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

// The cursor controls where the caret is and has initmate knowledge of the array-of-textblocks
// and encapsulates all the nasty corner cases of managing the cursor movement and text insertion
// and deletion.
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
