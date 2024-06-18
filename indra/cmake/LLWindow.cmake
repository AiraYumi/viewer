# -*- cmake -*-

include(Variables)
include(GLEXT)
include(Prebuilt)

include_guard()
add_library( ll::SDL INTERFACE IMPORTED )


if (LINUX)
  #Must come first as use_system_binary can exit this file early
  target_compile_definitions( ll::SDL INTERFACE LL_SDL_VERSION=3 LL_SDL)

  use_prebuilt_binary(SDL3)
  target_link_libraries( ll::SDL INTERFACE SDL3 X11)
endif (LINUX)
