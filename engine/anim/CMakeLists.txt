# Animation (ticket 20): easings, the tween timeline and the presenter that ends the script's
# `wait` steps (docs/SPEC.md section 10).
target_sources(
  vnmaki_engine
  PRIVATE easing.cpp
          easing.hpp
          timeline.cpp
          timeline.hpp
          wait_presenter.cpp
          wait_presenter.hpp)
