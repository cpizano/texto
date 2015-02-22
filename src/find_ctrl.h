#include "stdafx.h"
// TExTO.  Copyright 2014, Carlos Pizano (carlos.pizano@gmail.com)
// TExTO is a text editor prototype. This is the find-in-document
// control.

class FindControl : public MessageTarget {
  const plx::DPI& dpi_;
  D2D1_SIZE_F box_;
  plx::ComPtr<IDCompositionVisual2> visual_;
  plx::ComPtr<IDCompositionVisual2> root_visual_;
  plx::ComPtr<IDCompositionSurface> surface_;
  plx::ComPtr<IDWriteFactory> dwrite_factory_;

  plx::ComPtr<IDWriteTextLayout> dwrite_layout_;
  plx::ComPtr<IDWriteTextFormat> dwrite_fmt_;
  plx::ComPtr<ID2D1Geometry> geometry_;

  std::wstring search_text_;

  enum BrushesHover {
    brush_text,
    brush_background,
    brush_last
  };
  plx::D2D1BrushManager brushes_;

  const int width_ = 200;
  const int height_ = 60;
  
public:
  FindControl(const plx::DPI& dpi,
              plx::ComPtr<IDCompositionDesktopDevice> dco_device,
              plx::ComPtr<IDCompositionVisual2> root_visual,
              plx::ComPtr<IDWriteFactory> dwrite_factory,
              plx::ComPtr<ID2D1Factory2> d2d1_factory) 
      : dpi_(dpi),
        box_(D2D1::SizeF(160.0f, 20.0f)),
        root_visual_(root_visual),
        dwrite_factory_(dwrite_factory),
        brushes_(brush_last) {

    surface_ = plx::CreateDCoSurface(
        dco_device,
        static_cast<unsigned int>(dpi_.to_physical_x(width_)),
        static_cast<unsigned int>(dpi_.to_physical_x(height_)));

    visual_ = plx::CreateDCoVisual(dco_device);
    visual_->SetContent(surface_.Get());
    root_visual_->AddVisual(visual_.Get(), TRUE, nullptr);

    {
      plx::ScopedD2D1DeviceContext dc(surface_, D2D1::SizeF(), dpi, nullptr);
      brushes_.set_solid(dc(), brush_text, 0xD68739, 1.0f);
      brushes_.set_solid(dc(), brush_background, 0x1E5D81, 0.5f);
    }

    dwrite_fmt_ = plx::CreateDWriteSystemTextFormat(
        dwrite_factory_, L"Consolas", 14.0f, plx::FontWSSParams::MakeNormal());

    geometry_ = plx::CreateD2D1Geometry(d2d1_factory,
        D2D1::RoundedRect(
            D2D1::Rect(3.0f, 3.0f, width_ - 6.0f, height_ - 6.0f),
            3.0f, 3.0f));

    update_layout(L"text to search");
    draw();
  }

  ~FindControl() {
    brushes_.release_all();
    root_visual_->RemoveVisual(visual_.Get());
  }
  
  void set_position(float x, float y) {
    visual_->SetOffsetX(x);
    visual_->SetOffsetY(y);
  }

  bool MessageTarget::got_focus() override {
    return true;
  }

  void MessageTarget::lost_focus() override {
  }

  LRESULT MessageTarget::message_handler(
      const UINT message, WPARAM wparam, LPARAM lparam, bool* handled) override {
    *handled = false;

    if (message == WM_CHAR) {
      auto c = static_cast<wchar_t>(wparam);
      if (c >= 0x20) {
        search_text_.append(1, c);
      } else if (c == 0x08) {
        // backspace.
        if (!search_text_.empty())
          search_text_.resize(search_text_.size() - 1);
      } else {
        return 0L;
      }
 
      update_layout(search_text_);
      draw();
      *handled = true;
    }
    return 0L;
  }

private:
  void update_layout(const std::wstring& text) {
    plx::Range<const wchar_t> r(&text[0], text.size());
    dwrite_layout_ = plx::CreateDWTextLayout(
        dwrite_factory_, dwrite_fmt_, r, box_);
    dwrite_layout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }

  void draw() {
    D2D1::ColorF bk_color(0x0, 0.1f);
    plx::ScopedD2D1DeviceContext dc(surface_, D2D1::SizeF(), dpi_, &bk_color);

    dc()->FillGeometry(geometry_.Get(), brushes_.solid(brush_background));
    dc()->DrawTextLayout(
        D2D1::Point2F(10.0f, 10.0f), dwrite_layout_.Get(), brushes_.solid(brush_text));
  }

};
 
