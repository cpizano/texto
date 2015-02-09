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

std::vector<DWRITE_LINE_METRICS> GetDWLineMetrics(IDWriteTextLayout* layout) {
  uint32_t line_count = 0;
  auto hr = layout->GetLineMetrics(nullptr, 0, &line_count);
  if (hr != E_NOT_SUFFICIENT_BUFFER)
    throw plx::ComException(__LINE__, hr);

  std::vector<DWRITE_LINE_METRICS> metrics(line_count);
  hr = layout->GetLineMetrics(&metrics.front(), line_count, &line_count);
  if (hr != S_OK)
    throw plx::ComException(__LINE__, hr);

  return metrics;
}

class TextView {
  const float scroll_width = 22.0f;

  // the |box_| are the outer layout dimensions.
  D2D1_SIZE_F box_;
  // the scroll area.
  D2D1_POINT_2F scroll_box_;
  // approximate number of characters that fill |box_| given 8pt monospace font.
  size_t block_size_;
  // the caret,|cursor_| is absolute.
  size_t cursor_;
  // contains the absolute position of the line where the cursor is.
  size_t cursor_line_;
  // when scrolling the previous cursor X coordinate.
  float cursor_ideal_x_;
  // the character that starts the next line. Layout always starts here.
  size_t start_;
  // (one past) the end is either the smaller of string size or 8KB.
  size_t end_;
  // (one past) the end of the visible text.
  size_t end_view_;
  // the start and end positions from |full_text_| where |active_text_| was copied from.
  size_t active_start_;
  size_t active_end_;
  // the texr range for the interactive selection for copy & paste.
  Selection selection_;
  // stores the text as it should be on disk. If edits are in play it might be incomplete.
  std::unique_ptr<std::wstring> full_text_;
  // keeps the active text modifications, it is sort of a "delta" from |full_text_|.
  std::unique_ptr<std::wstring> active_text_;
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
        cursor_(0), cursor_line_(0), cursor_ideal_x_(-1.0f),
        start_(0), end_(0), end_view_(0),
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
    box_ = D2D1::SizeF(width - scroll_width, static_cast<float>(height));
    scroll_box_ = D2D1::Point2F(box_.width, 0.0f);
    change_view(start_);
  }

  size_t cursor() const { return cursor_; }

  void move_cursor_left() {
    if (cursor_ == 0)
      return;
    view_to_cursor();
    if (!selection_.is_empty()) {
      cursor_ = selection_.begin;
      selection_.clear();
    } else {
      --cursor_;
    }
    if (cursor_ < start_) {
      v_scroll(-1);
    }
    save_cursor_info();
  }

  void move_cursor_right() {
    if (!cursor_in_text())
      return;

    view_to_cursor();
    if (!selection_.is_empty()) {
      cursor_ = selection_.end;
      selection_.clear();
    } else {
      ++cursor_;
    }
    save_cursor_info();
  }

  void move_cursor_down() {
    selection_.clear();
    view_to_cursor();
    cursor_ = cursor_at_line_offset(1);
    if (cursor_ >= end_view_)
      v_scroll(2);
  }

  void move_cursor_up() {
    selection_.clear();
    if (cursor_ == 0)
      return;
    view_to_cursor();
    auto next_cursor = cursor_at_line_offset(-1);
    if (next_cursor == cursor_) {
      // we didn't scroll, we must be at the top so
      // we need to scroll one unit up.
      v_scroll(-1);
      next_cursor = cursor_at_line_offset(-1);
      if (next_cursor == cursor_)
        cursor_ = 0;
    }
    cursor_ = next_cursor;
  }

  void view_to_cursor() {
    if ((cursor_line_ < start_) || (cursor_line_ > end_view_)) {
      change_view(cursor_line_);
    }
  }

  void move_cursor_to(float x, float y) {
    if (x > box_.width) {
      //scrollbox hit.
      scrollbox_move(y / box_.height);
    } else {
      // moving cursor.
      selection_.clear();
      cursor_ = text_position(x, y) + start_;
      save_cursor_info();
    }
  }

  void change_selection(float x, float y) {
    auto prev_cursor = cursor_;
    cursor_ = text_position(x, y) + start_;

    if (cursor_ == prev_cursor)
      return;

    if (selection_.is_empty()) {
      selection_.begin = std::min(cursor_, prev_cursor);
      selection_.end =  std::max(cursor_, prev_cursor);
      return;
    }

    if (cursor_ < selection_.begin) {
      selection_.begin = cursor_;
    } else if (cursor_ > selection_.end) {
      selection_.end = cursor_;
    } else {
      if (cursor_ > prev_cursor)
        selection_.begin = cursor_;
      else
        selection_.end = cursor_;
    }
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
    save_cursor_info();
  }

  std::wstring get_selection() {
    if (selection_.is_empty())
      return std::wstring();
    merge_active_text();
    return std::wstring(full_text_->substr(selection_.begin, selection_.lenght()));
  }

  void v_scroll(int v_offset) {
    if (v_offset == 0)
      return;
    save_cursor_info();
    if (v_offset < 0) {
      if (!start_)
        return;
      // $$$ we are not honoring the offset here.
      v_offset = -v_offset;
      change_view(find_start_above(start_ -1));
    } else {
      size_t pos = 0;
      auto metrics = GetDWLineMetrics(dwrite_layout_.Get());
      for (auto& ln : metrics) {
        pos += ln.length;
        if (--v_offset == 0)
          break;
      }
      change_view(start_ + pos);
    }
  }

  void scrollbox_move(float y_fraction) {
    merge_active_text();
    size_t target = static_cast<size_t>(full_text_->size() * y_fraction);
    change_view(find_start_above(target));
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
    draw_scroll(dc, brush.solid(brush_caret), brush.solid(brush_space));

    // debugging aids.
    if (options == show_marks) {
      draw_marks(dc, 
                 brush.solid(brush_lf),
                 brush.solid(brush_space),
                 brush.solid(brush_control));
    }
  }

  const std::wstring& get_full_text() {
    merge_active_text();
    return *full_text_.get();
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

  size_t last_position_in_view() {
    return text_position(box_.width, box_.height) + start_ + 1;
  }

  size_t find_previous_nl_start(size_t target) {
    if (!target)
      return 0;

    for (size_t ix = target - 1; ix != 0; --ix) {
      if ((*full_text_)[ix] == L'\n')
        return ix + 1;
    }
    return 0;
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
    invalidate();
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

  void save_cursor_info() {
    if (cursor_ < start_)
      return;
    if (cursor_ > end_view_)
      return;

    auto pt = point_from_txtpos(relative_cursor(), nullptr);
    cursor_ideal_x_ = pt.x;
    cursor_line_ = text_position(0, pt.y) + start_;
  }

  uint32_t cursor_at_line_offset(int32_t delta) {
    float height;
    auto pt = point_from_txtpos(relative_cursor(), &height);

    if (cursor_ideal_x_ > 0.0f)
      pt.x = cursor_ideal_x_;

    auto text_pos = text_position(pt.x, pt.y + (height * delta));
    return plx::To<uint32_t>(text_pos + start_);
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
    end_view_ = last_position_in_view();
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
    if (cursor_ > end_view_)
      return;

    float hm_height;
    auto pt = point_from_txtpos(relative_cursor(), &hm_height);
    auto yf = pt.y + hm_height;

    auto aa_mode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    if (caret_brush) {
      // caret.
      dc->DrawRectangle(
          D2D1::RectF(pt.x, pt.y, pt.x + 2.0f, yf), caret_brush, 1.0f);
    }
    if (line_brush) {
      // active line.
      dc->FillRectangle(D2D1::RectF(0, pt.y, box_.width, yf), line_brush);
    }
    dc->SetAntialiasMode(aa_mode);
  }

  void draw_scroll(ID2D1DeviceContext* dc,
                   ID2D1Brush* brush_gripper, ID2D1Brush* brush_cursor) {
    auto pt = point_from_txtpos(plx::To<uint32_t>(end_), nullptr);
    if ((pt.y < box_.height) && (start_ < 10))
      return;

    if (full_text_->empty())
      __debugbreak();

    auto aa_mode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    auto inset_x = scroll_box_.x + 4.0f;

    // the scroll box.
    dc->DrawRectangle(
        D2D1::RectF(inset_x, scroll_box_.y,
                    scroll_box_.x + scroll_width, box_.height),
        brush_gripper, 1.0f);

    auto pos_start = box_.height * float(start_) / float(full_text_->size());

    auto gripper_height = std::max(
        4.0f,
        ((end_view_ - start_) * box_.height) / full_text_->size());
    // view box.
    dc->FillRectangle(
        D2D1::RectF(inset_x, pos_start,
                    scroll_box_.x + scroll_width, pos_start + gripper_height),
        brush_gripper);

    auto pos_curs = box_.height * float(cursor_) / float(full_text_->size());

    // cursor mark.
    dc->FillRectangle(
        D2D1::RectF(inset_x, pos_curs,
                    scroll_box_.x + scroll_width - 1.0f, pos_curs + 2.0f),
        brush_cursor);

    dc->SetAntialiasMode(aa_mode);
  }

  void draw_selection(ID2D1DeviceContext* dc, ID2D1Brush* sel_brush) {
    if (selection_.is_empty())
      return;

    if (selection_.begin < start_)
      return;

    float hm0_height, hm1_height;
    auto p0 = point_from_txtpos(selection_.get_relative_begin(start_), &hm0_height);
    auto p1 = point_from_txtpos(selection_.get_relative_end(start_), &hm1_height);

    auto aa_mode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    auto yf0 = p0.y + hm0_height;
    auto yf1 = p1.y + hm1_height;

    if (p1.y - p0.y < 0.01) {
      // single line select.
      dc->FillRectangle(D2D1::RectF(p0.x, p0.y, p1.x, yf0), sel_brush);
    } else {
      // multi-line select.
      dc->FillRectangle(D2D1::RectF(p0.x, p0.y, box_.width, yf0), sel_brush);
      dc->FillRectangle(D2D1::RectF(0, yf0, box_.width, p1.y), sel_brush);
      dc->FillRectangle(D2D1::RectF(0, p1.y, p1.x, yf1), sel_brush);
    }

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
        float hm_height;
        auto pt = point_from_txtpos(offset, &hm_height);
        pt.y += (2.0f * hm_height) / 3.0f;
        pt.x += x_offset;
        dc->DrawRectangle(D2D1::RectF(pt.x, pt.y, pt.x + width, pt.y + height), brush);
      }
      offset += cm.length;
    }
  }

  uint32_t text_position(float x, float y) {
    if (!dwrite_layout_)
      update_layout();

    DWRITE_HIT_TEST_METRICS hit_metrics;
    BOOL is_trailing;
    BOOL is_inside;
    auto hr = dwrite_layout_->HitTestPoint(x, y, &is_trailing, &is_inside, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    return hit_metrics.textPosition;
  }

  D2D1_POINT_2F point_from_txtpos(uint32_t text_position, float* height) {
    if (!dwrite_layout_)
      update_layout();

    DWRITE_HIT_TEST_METRICS hit_metrics;
    float x, y;
    auto hr = dwrite_layout_->HitTestTextPosition(text_position, FALSE, &x, &y, &hit_metrics);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    if (height)
      *height = hit_metrics.height;

    return D2D1_POINT_2F {x, y};
  }

  size_t find_start_above(size_t target) {
    merge_active_text();
    auto prev = find_previous_nl_start(target);
    
    auto txt = plx::Range<const wchar_t>(&(*full_text_)[prev],
                                         &(*full_text_)[target + 1]);
    auto layout = plx::CreateDWTextLayout(dwrite_factory_, dwrite_fmt_, txt, box_);
    auto metrics = GetDWLineMetrics(layout.Get());

    size_t sum = prev;
    for (const auto& line : metrics) {
      if (sum + line.length > target)
        break;
      sum += line.length;
    }
    return sum;
  }

};
