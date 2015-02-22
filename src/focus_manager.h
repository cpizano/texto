#include "stdafx.h"
// TExTO.  Copyright 2014, Carlos Pizano (carlos.pizano@gmail.com)
// TExTO is a text editor prototype. This is the focus manager.

// There are 3 ways the focus manager re-directs keyboard focus
//

class MessageTarget {
public:
  virtual bool got_focus() = 0;
  virtual void lost_focus() = 0;
  virtual LRESULT message_handler(
      const UINT message, WPARAM wparam, LPARAM lparam, bool* handled) = 0;
};

class FocusManager {
  std::list<MessageTarget*> ctrls_;
  MessageTarget* current_;
  MessageTarget* previous_;

public:
  FocusManager() : current_(nullptr), previous_(nullptr) {}
  ~FocusManager() {}

  void add_target(MessageTarget* target) {
    ctrls_.emplace_front(target);
  }

  void remove_target(MessageTarget* target) {
    if (current_ == target)
      current_ = nullptr;
    if (previous_ == target)
      previous_ = nullptr;

    ctrls_.erase(
        std::remove(ctrls_.begin(), ctrls_.end(), target));
  }

  bool take_focus(MessageTarget* target) {
    if (target == current_)
      return true;
    if (!find_top(target))
      throw 5;
    do_focus(target);
    return true;
  }

  void reset_focus() {
    do_focus_lost();
    current_ = nullptr;
  }

  void focus_previous() {
    if (previous_ == current_)
      return;
    do_focus(previous_);
  }

  LRESULT message_handler(
      const UINT message, WPARAM wparam, LPARAM lparam, bool* handled) {
    if (!current_ || is_app_message(message)) {
      *handled = false;
      return 0L;
    }
    return current_->message_handler(message, wparam, lparam, handled);
  }

private:
  MessageTarget* find_top(MessageTarget* target) {
    for (auto ctrl : ctrls_) {
      if (ctrl == target)
        return ctrl;
    }
    return nullptr;
  }

  bool is_app_message(const UINT message) const {
    return (message == WM_DESTROY) ||
           (message == WM_DPICHANGED);
  }

  void do_focus_lost() {
    if (!current_)
      return;
    current_->lost_focus();
    previous_ = current_;
  }

  void do_focus(MessageTarget* target) {
    do_focus_lost();
    current_ = target;
    current_->got_focus();
  }

};
 
