# Animation (tickets 20 and 21): easings, the tween timeline, transitions and the presenter that
# ends the script's `wait` steps (docs/SPEC.md section 10).
target_sources(
  vnmaki_engine
  PRIVATE easing.cpp
          easing.hpp
          timeline.cpp
          timeline.hpp
          transition.cpp
          transition.hpp
          wait_presenter.cpp
          wait_presenter.hpp)
