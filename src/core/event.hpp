#pragma once

#include <cstdint>
#include <vector>

namespace core {

template <typename... T> class Event {
public:
  typedef void (*Callback)(T...);

private:
  struct Listener {
    Callback callback;
    std::tuple<T...> members;
    Listener(T... values) : members(values...) {}
  };

public:
  inline void addListener(Callback callback, T... values) {
    Listener listener(values...);
    listeners.push_back(listener);
  }

  inline void removeListener(Callback callback, T... values) {
    for (uint32_t i = 0; i < listeners.size(); i++) {
      if (listeners[i].callback == callback && listeners[i].members == std::tuple<T...>(values...)) {
        listeners[i].callback = listeners.back().callback;
        listeners[i].members = listeners.back().members;
        listeners.pop_back();
        break;
      }
    }
  }

  inline void invoke() {
    for (Listener &l : listeners) {
      std::apply([&](T... args) { l.callback(args...); }, l.members);
    }
  }

private:
  std::vector<Listener> listeners;
};

template <typename S, typename... T> class FixedEvent {
public:
  typedef void (*Callback)(S, T...);

private:
  struct Listener {
    Callback callback;
    std::tuple<T...> members;
    Listener(T... values) : members(values...) {}
  };

public:
  inline void addListener(Callback callback, T... values) {
    Listener listener(values...);
    listeners.push_back(listener);
  }

  inline void removeListener(Callback callback, T... values) {
    for (uint32_t i = 0; i < listeners.size(); i++) {
      if (listeners[i].callback == callback && listeners[i].members == std::tuple<T...>(values...)) {
        listeners[i].callback = listeners.back().callback;
        listeners[i].members = listeners.back().members;
        listeners.pop_back();
        break;
      }
    }
  }

  inline void invoke(S self) {
    for (Listener &l : listeners) {
      std::apply([&](T... args) { l.callback(self, args...); }, l.members);
    }
  }

private:
  S self;
  std::vector<Listener> listeners;
};

} // namespace core