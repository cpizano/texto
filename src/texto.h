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
// notepad.exe and probably a worse notepad++.

struct Selection {
  size_t begin;
  size_t end;

  Selection() : begin(0), end(0) {}
  bool is_empty() { return begin == end; }

  uint32_t get_relative_begin(size_t start) {
    return plx::To<uint32_t>(begin - start);
  }

  uint32_t get_relative_end(size_t start) {
    return plx::To<uint32_t>(end - start);
  }

  void clear() {
    begin = end = 0;
  }

  size_t lenght() {
    return end - begin;
  }
};

class TextView {
  // the |box_| are the layout dimensions.
  D2D1_SIZE_F box_;
  // approximate number of characters that fill |box_| given 8pt monospace font.
  size_t block_size_;
  // |cursor_| is absolute.
  size_t cursor_;
  // when scrolling the previous cursor X coordinate.
  float cursor_ideal_x_;
  // the character that starts the next line. Layout always starts here.
  size_t start_;
  // the end is either the smaller of string size or 8KB.
  size_t end_;
  // the start and end positions from |full_text_| where |active_text_| was copied from.
  size_t active_start_;
  size_t active_end_;
  // stores previous values of |start_| when we scroll down. It allows to cheaply scroll up.
  std::vector<size_t> starts_;
  // the texr range for the interactive selection for copy & paste.
  Selection selection_;
  // stores the text as it should be on disk. If edits are in play it might be incomplete.
  std::unique_ptr<std::wstring> full_text_;
  // keeps the active text modifications, it is sort of a "delta" from |full_text_|.
  std::unique_ptr<std::wstring> active_text_;
  // used to compute the next line when we scroll down.
  std::vector<DWRITE_LINE_METRICS> line_metrics_;
  // the 3 directwrite objects are necessary for layout and rendering.
  plx::ComPtr<IDWriteTextLayout> dwrite_layout_;
  plx::ComPtr<IDWriteFactory> dwrite_factory_;
  plx::ComPtr<IDWriteTextFormat> dwrite_fmt_;

  // non-copiable.
  TextView& operator=(const TextView&) = delete;
  TextView(const TextView&) = delete;

public:
  TextView(plx::ComPtr<IDWriteFactory> dwrite_factory,
           plx::ComPtr<IDWriteTextFormat> dwrite_fmt,
           std::wstring* text) 
      : box_(D2D1::SizeF()),
        block_size_(0),
        cursor_(0), cursor_ideal_x_(-1.0f),
        start_(0), end_(0),
        active_start_(0), active_end_(0),
        dwrite_factory_(dwrite_factory),
        dwrite_fmt_(dwrite_fmt) {
    if (text) {
      full_text_.reset(text);
    } else {
      full_text_ = std::make_unique<std::wstring>();
    }
  }

  void set_size(uint32_t width, uint32_t height) {
    // every 85 square pixels you need a character. Think of it as an educated guess, like
    // a single character is 8.5 x 10 pixels in the worst case.
    block_size_ = (width * height) / 85;
    box_ = D2D1::SizeF(static_cast<float>(width), static_cast<float>(height));
    change_view(start_);
    invalidate();
  }

  void move_cursor_left() {
    if (cursor_ == 0)
      return;
    if (!selection_.is_empty()) {
      cursor_ = selection_.begin;
      selection_.clear();
    } else {
      --cursor_;
    }
    save_cursor_ideal_x();
  }

  void move_cursor_right() {
    if (cursor_ == end_)
      return;
    if (!selection_.is_empty()) {
      cursor_ = selection_.end;
      selection_.clear();
    } else {
      ++cursor_;
    }
    save_cursor_ideal_x();
  }

  void move_cursor_down() {
    selection_.clear();
    if (cursor_ == end_)
      return;
    cursor_ = cursor_at_line_offset(1);
  }

  void move_cursor_up() {
    selection_.clear();
    if (cursor_ == 0)
      return;
    cursor_ = cursor_at_line_offset(-1);
  }

  void move_cursor_to(float x, float y) {
    selection_.clear();
    cursor_ = text_position(x, y) + start_;
  }

  void change_selection(float x, float y) {
    if (selection_.is_empty())
      selection_.begin = cursor_;
    cursor_ = text_position(x, y) + start_;
    selection_.end = cursor_;
  }

  void select_word() {
    if (!cursor_in_text())
      return;

    auto cac = char_at(cursor_);
    if ( cac < 0x30) {
      // we are at a non printable char, just select it.
      selection_.begin = cursor_;
      selection_.end = cursor_ + 1;
    } else {
      // we are at a printable char, expand left and right.
      auto center = cursor_;
      std::wstring* str;
      if (!active_text_) {
        str = full_text_.get();
      } else {
        str = active_text_.get();
        center -= start_;
      }

      // go left first.
      auto left = center;
      while (left != 0) {
        if (str->at(left) < 0x30)
          break;
        --left;
      }
      // go right next.
      auto right = center;
      while (right != str->size()) {
        if (str->at(right) < 0x30)
          break;
        ++right;
      }

      ++left;
      if (left < right) {
        selection_.begin = active_text_ ? left + start_ : left;
        selection_.end = active_text_ ? right + start_ : right;
      }
    }

    cursor_ = selection_.end;
    save_cursor_ideal_x();
  }

  void v_scroll(int v_offset) {
    if (v_offset == 0)
      return;
    // scroll is fairly different going fwd than going backward. Going fwd requires
    // calling GetLineMetrics() and finding the character where the n-th line begins
    // while scrolling backwards requires looking at the |starts_| vector.
    if (v_offset < 0) {
      v_offset = -v_offset;
      if (v_offset > starts_.size())
        return;
      size_t start = 0;
      for (; v_offset; --v_offset) {
        start = starts_.back();
        starts_.pop_back();
      }
      change_view(start);
    } else {
      size_t pos = 0;
      for (auto& ln : line_metrics_) {
        pos += ln.length;
        if (--v_offset == 0)
          break;
      }
      starts_.push_back(start_);
      change_view(start_ + pos);
    }
    invalidate();
  }

  void insert_char(wchar_t c) {
    if (cursor_ < start_) {
      // $$ move view to cursor.
      return;
    }
    make_active_text();
    active_text_->insert(relative_cursor(), 1, c);
    ++cursor_;
    ++end_;
    invalidate();
  }

  void insert_text(const std::wstring text) {
    if (text.size() < 512) {
      make_active_text();
      active_text_->insert(relative_cursor(), text);
    } else {
      merge_active_text();
      full_text_->insert(cursor_, text);
    }
    cursor_ += plx::To<uint32_t>(text.size());
    end_ += text.size();
    invalidate();
  }

  bool back_erase() {
    if (cursor_ <= 0)
      return false;
    make_active_text();
    if (!selection_.is_empty()) {
      active_text_->erase(selection_.get_relative_begin(start_), selection_.lenght());
      cursor_ = selection_.begin;
      selection_.clear();
    } else {
      --cursor_;
      active_text_->erase(relative_cursor(), 1);
      --end_;
    }
    invalidate();
    return true;
  }

  enum DrawOptions {
    normal,
    show_marks,
  };

  enum DrawBrushes {
    brush_text,
    brush_caret,
    brush_line,
    brush_control,
    brush_lf,
    brush_space,
    brush_selection,
    brush_last
  };

  // draws everything. Note that the start of the text depends not of the device context transform
  // but on the |start_| of the text.
  void draw(ID2D1DeviceContext* dc,
            plx::D2D1BrushManager& brush,
            DrawOptions options) {
    // layout on demand.
    if (!dwrite_layout_) {
      update_layout();
    }

    draw_cursor_line(dc, brush.solid(brush_line));
    draw_selection(dc, brush.solid(brush_selection));
    dc->DrawTextLayout(D2D1::Point2F(), dwrite_layout_.Get(), brush.solid(brush_text));
    draw_caret(dc, brush.solid(brush_caret));

    // debugging aids.
    if (options == show_marks) {
      draw_marks(dc, 
                 brush.solid(brush_lf),
                 brush.solid(brush_space),
                 brush.solid(brush_control));
    }
  }

private:

  uint32_t relative_cursor() {
    return plx::To<uint32_t>(cursor_ - start_);
  }

  bool cursor_in_text() {
    if (!active_text_)
      return cursor_  < full_text_->size();
    else
      return (cursor_ - start_) < active_text_->size();
  }

  wchar_t char_at(size_t offset) {
    if (!active_text_)
      return full_text_->at(offset);
    else
      return active_text_->at(offset - start_);
  }

  // we change view when we scroll. |from| is always a line start.
  void change_view(size_t from) {
    // we always merge the modified text back before scrolling. This can be
    // expensive if we are talking many MB of text in |full_text_|.
    merge_active_text();
    
    if (from > full_text_->size())
      __debugbreak();

    start_ = from;
    end_ = from + std::min(block_size_, full_text_->size() - from);
  }

  // the user has made a text modification, we store and layout now from the
  // |active_text_| until we scroll.
  void make_active_text() {
    if (active_text_)
      return;
    // remember the range we copied, which we need to use when we merge.
    active_start_ = start_;
    active_end_ = end_;
    // offset the cursor since are operating on |active_text_|.
    // slice full_text now.
    active_text_ = std::make_unique<std::wstring>(
        full_text_->substr(active_start_, active_end_ - active_start_));
  }

  // the user is scrolling or saving, we need to have |full_text_| be the sole
  // source of truth.
  void merge_active_text() {
    if (!active_text_)
      return;
    if (active_text_->empty())
      return;
    // undo the offset done in make_active_text().
    // This is slow if many MB are stored in |full_text_|. 
    full_text_->erase(active_start_, active_end_ - active_start_);
    full_text_->insert(start_, *active_text_);
    active_text_.reset();
  }

  void invalidate() {
    dwrite_layout_.Reset();
  }

  void update_layout() {
    plx::Range<const wchar_t> txt;
    if (!active_text_) {
      // layout from |full_text_|
      txt = plx::Range<const wchar_t>(&(*full_text_)[0] + start_,
                                      &(*full_text_)[0] + end_);
    } else {
      // layout from |active_text_|
      txt = plx::Range<const wchar_t>(active_text_->c_str(), active_text_->size());
    }
    dwrite_layout_ = plx::CreateDWTextLayout(dwrite_factory_, dwrite_fmt_, txt, box_);
    line_metrics_ = get_line_metrics();
  }

  void draw_cursor_line(ID2D1DeviceContext* dc, ID2D1Brush* line_brush) {
    draw_helper(dc, nullptr, line_brush);
  }

  void draw_caret(ID2D1DeviceContext* dc, ID2D1Brush* caret_brush) {
    draw_helper(dc, caret_brush, nullptr);
  }

  void draw_helper(ID2D1DeviceContext* dc, ID2D1Brush* caret_brush, ID2D1Brush* line_brush) {
    if (cursor_ < start_)
      return;
    if (cursor_ > end_)
      return;
    DWRITE_HIT_TEST_METRICS hit_metrics;
    float x, y;
    auto hr = dwrite_layout_->HitTestTextPosition(relative_cursor(), FALSE, &x, &y, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    auto yf = y + hit_metrics.height;

    auto aa_mode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    if (caret_brush) {
      // caret.
      dc->DrawRectangle(
          D2D1::RectF(x, y, x + 2.0f, yf), caret_brush, 1.0f);
      dc->SetAntialiasMode(aa_mode);
    }
    if (line_brush) {
      // active line.
      dc->FillRectangle(D2D1::RectF(0, y, box_.width, yf), line_brush);
    }
    dc->SetAntialiasMode(aa_mode);
  }

  void draw_selection(ID2D1DeviceContext* dc, ID2D1Brush* sel_brush) {
    if (selection_.is_empty())
      return;

    if (selection_.begin < start_)
      return;

    DWRITE_HIT_TEST_METRICS hitm0, hitm1;
    float x0, x1, y0, y1;
    auto hr = dwrite_layout_->HitTestTextPosition(
        selection_.get_relative_begin(start_), FALSE, &x0, &y0, &hitm0);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);

    hr = dwrite_layout_->HitTestTextPosition(
        selection_.get_relative_end(start_), FALSE, &x1, &y1, &hitm1);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);

    auto yf = y0 + hitm0.height;
    auto aa_mode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    dc->FillRectangle(D2D1::RectF(x0, y0, x1, yf), sel_brush);

    dc->SetAntialiasMode(aa_mode);
  }

  void draw_marks(ID2D1DeviceContext* dc,
                  ID2D1Brush* lf_brush, ID2D1Brush* space_brush, ID2D1Brush* control_brush) {
    uint32_t count = 0;
    auto hr = dwrite_layout_->GetClusterMetrics(nullptr,  0, &count);
    if (hr == S_OK)
      return;

    if (hr != E_NOT_SUFFICIENT_BUFFER) {
      __debugbreak();
    }
    std::vector<DWRITE_CLUSTER_METRICS> metrics(count);
    hr = dwrite_layout_->GetClusterMetrics(&metrics[0], count, &count);
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
        brush = lf_brush;
        width = 3.0f;
        height = 3.0f;
        x_offset = 1.0f;
      } else if (cm.isWhitespace) {
        brush = space_brush;
        height = 1.0f;
        if (cm.width == 0) {
          // control char (rare, possibly a bug).
          brush = control_brush;
          width = 2.0f;
          height = -5.0f;
          x_offset = cm.width / 3.0f;
        } else if (char_at(offset) == L'\t') {
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
        hr = dwrite_layout_->HitTestTextPosition(offset, FALSE, &x, &y, &hit_metrics);
        if (hr != S_OK)
          __debugbreak();
        y += (2.0f * hit_metrics.height) / 3.0f;
        x += x_offset;
        dc->DrawRectangle(D2D1::RectF(x, y, x + width, y + height), brush);
      }
      offset += cm.length;
    }
  }

  std::vector<DWRITE_LINE_METRICS> get_line_metrics() {
    uint32_t line_count = 0;
    auto hr = dwrite_layout_->GetLineMetrics(nullptr, 0, &line_count);
    if (hr != E_NOT_SUFFICIENT_BUFFER)
      __debugbreak();

    std::vector<DWRITE_LINE_METRICS> metrics(line_count);
    hr = dwrite_layout_->GetLineMetrics(&metrics.front(), line_count, &line_count);
    if (hr != S_OK)
      __debugbreak();

    return metrics;
  }

  void save_cursor_ideal_x() {
    DWRITE_HIT_TEST_METRICS hit_metrics;
    float x, y;
    auto hr = dwrite_layout_->HitTestTextPosition(relative_cursor(), FALSE, &x, &y, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    cursor_ideal_x_ = x;
  }

  uint32_t cursor_at_line_offset(int32_t delta) {
    DWRITE_HIT_TEST_METRICS hit_metrics;
    float x, y;
    auto hr = dwrite_layout_->HitTestTextPosition(relative_cursor(), FALSE, &x, &y, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);

    if (cursor_ideal_x_ > 0.0f)
      x = cursor_ideal_x_;

    auto text_pos = text_position(x, hit_metrics.height * delta);
    return plx::To<uint32_t>(text_pos + start_);
  }

  uint32_t text_position(float x, float y) {
    DWRITE_HIT_TEST_METRICS hit_metrics;
    BOOL is_trailing;
    BOOL is_inside;
    auto hr = dwrite_layout_->HitTestPoint(x, y, &is_trailing, &is_inside, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    return hit_metrics.textPosition;
  }

};
