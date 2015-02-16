#include "stdafx.h"
// TExTO.  Copyright 2014, Carlos Pizano (carlos.pizano@gmail.com)
// TExTO is a text editor prototype this is the find-in-document
// control.

class FindControl {
  const plx::DPI& dpi_;
  plx::ComPtr<IDCompositionVisual2> visual_;
  plx::ComPtr<IDCompositionVisual2> root_visual_;
  plx::ComPtr<IDCompositionSurface> surface_;

  enum BrushesHover {
    brush_text,
    brush_last
  };
  plx::D2D1BrushManager brushes_;

  const int width_ = 200;
  const int height_ = 200;
  
public:
  FindControl(const plx::DPI& dpi,
              plx::ComPtr<IDCompositionDesktopDevice> dco_device,
              plx::ComPtr<IDCompositionVisual2> root_visual) 
      : dpi_(dpi),
        root_visual_(root_visual),
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
    }

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

  void draw() {
    D2D1::ColorF bk_color(0x000000, 0.1f);
    plx::ScopedD2D1DeviceContext dc(surface_, D2D1::SizeF(), dpi_, &bk_color);
    dc()->DrawRectangle(
        D2D1::RectF(0.0f, 0.0f, 50.0f, 50.0f), brushes_.solid(brush_text));
  }

};
 
