# Localization (docs/SPEC.md section 16): `.po` files, string extraction and runtime languages.
target_sources(
  vnmaki_engine
  PRIVATE extract.cpp
          extract.hpp
          po.cpp
          po.hpp
          text_direction.hpp
          translator.cpp
          translator.hpp)
