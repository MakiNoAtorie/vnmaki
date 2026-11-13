# Dialogue: the presenter that turns say and narrate requests into a typed-out line, the say
# box that draws it (ticket 08), and skip and auto-advance (ticket 14).
target_sources(vnmaki_engine PRIVATE dialogue_presenter.cpp dialogue_presenter.hpp playback.cpp playback.hpp say_box.cpp say_box.hpp)
