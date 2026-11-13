# Dialogue: the presenter that turns say and narrate requests into a typed-out line, and the say
# box that draws it (ticket 08).
target_sources(vnmaki_engine PRIVATE dialogue_presenter.cpp dialogue_presenter.hpp say_box.cpp say_box.hpp)
