#include "stdafx.h"
// TExTO.  Copyright 2014, Carlos Pizano (carlos.pizano@gmail.com)
// TExTO is a text editor prototype. This is the focus manager.

// There are 3 ways the focus manager re-directs keyboard focus
//

class MessageTarget {
public:
  virtual plx::ComPtr<ID2D1Geometry> geometry() = 0;
  virtual void got_focus() = 0;
  virtual void lost_focus() = 0;
  virtual LRESULT message_handler(
      const UINT message, WPARAM wparam, LPARAM lparam, bool* handled) = 0;
};

class FocusManager {
  using Node = std::tuple<plx::ComPtr<ID2D1Geometry>, MessageTarget*>;
  using get_target = decltype(std::get<1>(Node()));
  using get_geometry = decltype(std::get<0>(Node()));

  std::list<Node> ctrls_;
  Node* current_;

public:
  FocusManager() : current_(nullptr) {}
  ~FocusManager() {}

  void add_target(MessageTarget* target) {
    auto geom = target->geometry();
    ctrls_.emplace_front(geom, target);
  }

  void remove_target(MessageTarget* target) {
    if (get_target(*current_) == target)
      current_ = nullptr;

    ctrls_.erase(
        std::remove_if(ctrls_.begin(), ctrls_.end(), [target](const Node& node) {
            return (get_target(node) == target);
    }));
  }

  bool take_focus(MessageTarget* target) {
    auto node = find_top(target);
    if (!node)
      return false;
    notify_focus_lost();
    current_ = node;
    notify_got_focus();
    return true;
  }

  LRESULT message_handler(
      const UINT message, WPARAM wparam, LPARAM lparam, bool* handled) {
    if (!current_ || is_app_message(message)) {
      *handled = false;
      return 0L;
    }
    return get_target(*current_)->message_handler(message, wparam, lparam, handled);
  }

private:
  Node* find_top(MessageTarget* target) {
    for (auto& node : ctrls_) {
      if (get_target(node) == target)
        return &node;
    }
    return nullptr;
  }

  bool is_app_message(const UINT message) const {
    return (message == WM_DESTROY) ||
           (message == WM_DPICHANGED);
  }

  void notify_focus_lost() {
    if (!current_)
      return;
    get_target(*current_)->lost_focus();
  }

  void notify_got_focus() {
    get_target(*current_)->got_focus();
  }

};
 
