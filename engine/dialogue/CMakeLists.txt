# Dialogue: the presenter that turns say and narrate requests into a typed-out line, the say
# box that draws it (ticket 08), skip and auto-advance (ticket 14), and the rollback inputs
# (ticket 13), and the backlog data (ticket 15).
target_sources(vnmaki_engine PRIVATE backlog.cpp backlog.hpp dialogue_presenter.cpp dialogue_presenter.hpp playback.cpp playback.hpp rollback_input.cpp rollback_input.hpp say_box.cpp say_box.hpp)
