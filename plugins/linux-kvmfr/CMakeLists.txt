project(linux-kvmfr)

if(DISABLE_KVMFR)
	message(STATUS "KVMFR support disabled")
	return()
endif()

set(linux-kvmfr_SOURCES
    linux-kvmfr.c
    kvmfr-source.c)

add_library(linux-kvmfr MODULE
            ${linux-kvmfr_SOURCES})
target_link_libraries(linux-kvmfr
                      libobs)

install_obs_plugin_with_data(linux-kvmfr data)
