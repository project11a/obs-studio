project(linux-looking-glass)

if(DISABLE_LOOKING_GLASS)
	message(STATUS "Looking Glass support disabled")
	return()
endif()

set(linux-looking-glass_SOURCES
    linux-looking-glass.c
    looking-glass-source.c)

add_library(linux-looking-glass MODULE
            ${linux-looking-glass_SOURCES})
target_link_libraries(linux-looking-glass
                      libobs)

install_obs_plugin_with_data(linux-looking-glass data)
