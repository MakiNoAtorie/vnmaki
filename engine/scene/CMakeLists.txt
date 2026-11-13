# Scene module (ticket 09): the displayable interface, the layer stack and the scene state that
# `scene`/`show`/`hide` change (docs/SPEC.md sections 6, 7 and 9).
target_sources(vnmaki_engine PRIVATE camera.cpp camera.hpp displayable.cpp displayable.hpp
                                          frame_animation.cpp frame_animation.hpp
                                          layered_sprite.cpp layered_sprite.hpp
                                          movie_displayable.cpp movie_displayable.hpp scene.cpp
                                          scene.hpp)
