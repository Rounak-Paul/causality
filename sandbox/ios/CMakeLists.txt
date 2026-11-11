add_executable(causality_ios_sandbox MACOSX_BUNDLE main.m)
target_compile_options(causality_ios_sandbox PRIVATE -fobjc-arc)
target_link_libraries(causality_ios_sandbox PRIVATE causality "-framework UIKit")
set_target_properties(causality_ios_sandbox PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER com.causality.sandbox
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
