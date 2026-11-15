cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(knob_s3)

# Workaround (esp_audio_codec 2.6.2): its CMakeLists adds the link item "-L <its lib dir>" unquoted, which breaks the
# link when the project path contains a space ("Local Documents"). The item is redundant: the component also links
# both of its .a files by full path. Drop it.
idf_component_get_property(_acodec_lib espressif__esp_audio_codec COMPONENT_LIB)
foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_items ${_acodec_lib} ${_prop})
    if(_items)
        list(FILTER _items EXCLUDE REGEX "-L ")
        set_target_properties(${_acodec_lib} PROPERTIES ${_prop} "${_items}")
    endif()
endforeach()
