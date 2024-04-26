include_guard(GLOBAL)

if( LINK_WITH_MOLD )
  find_program(MOLD_BIN mold)
  if(MOLD_BIN)
    message(STATUS "Mold linker found: ${MOLD_BIN}. Enabling mold as active linker.")
    add_link_options("-fuse-ld=${MOLD_BIN}")
  else()
    message(STATUS "Mold linker not found. Using default linker.")
  endif()
endif()