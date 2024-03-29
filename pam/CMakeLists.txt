find_package (PkgConfig REQUIRED)

pkg_check_modules (GLIB2 REQUIRED glib-2.0)
pkg_check_modules (GDBUS REQUIRED dbus-glib-1)
pkg_check_modules (GIO_UNIX REQUIRED gio-unix-2.0)
pkg_check_modules (GLIB_JSON REQUIRED json-glib-1.0)
pkg_check_modules (KIRAN_CC_DAEMON REQUIRED kiran-cc-daemon)

find_library(PAM_LIBRARY pam)

include_directories(${GLIB2_INCLUDE_DIRS} ${GDBUS_INCLUDE_DIRS} ${GIO_UNIX_INCLUDE_DIRS} ${GLIB_JSON_INCLUDE_DIRS} ${KIRAN_CC_DAEMON_INCLUDE_DIRS})
include_directories(${CMAKE_BINARY_DIR}/src/)
include_directories(${CMAKE_BINARY_DIR}/pam/)
include_directories(${SRC_DIR})

find_program(GDBUS_CODEGEN "gdbus-codegen")

if (NOT GDBUS_CODEGEN)
    message(FATAL_ERROR "gdbus-codegen program not found")
else()
    message("found ${GDBUS_CODEGEN}")
endif (NOT GDBUS_CODEGEN)

add_custom_command(OUTPUT kiran-user-gen.c kiran-user-gen.h
    COMMAND ${GDBUS_CODEGEN} --c-namespace Kiran --interface-prefix com.kylinsec.Kiran.SystemDaemon --generate-c-code kiran-user-gen  ${PROJECT_SOURCE_DIR}/data/com.kylinsec.Kiran.SystemDaemon.Accounts.User.xml
    DEPENDS ${PROJECT_SOURCE_DIR}/data/com.kylinsec.Kiran.SystemDaemon.Accounts.User.xml
)

add_custom_command(OUTPUT kiran-accounts-gen.c kiran-accounts-gen.h
    COMMAND ${GDBUS_CODEGEN} --c-namespace Kiran --interface-prefix com.kylinsec.Kiran.SystemDaemon --generate-c-code kiran-accounts-gen  ${PROJECT_SOURCE_DIR}/data/com.kylinsec.Kiran.SystemDaemon.Accounts.xml
    DEPENDS  ${PROJECT_SOURCE_DIR}/data/com.kylinsec.Kiran.SystemDaemon.Accounts.xml
)

find_program(GLIB_GENMARSHAL "glib-genmarshal")
if (NOT GLIB_GENMARSHAL)
    message(FATAL_ERROR "glib-genmarshal program not found")
else()
    message("found ${GLIB_GENMARSHAL}")
endif (NOT GLIB_GENMARSHAL)

add_custom_command(OUTPUT marshal.c marshal.h
    COMMAND ${GLIB_GENMARSHAL} --prefix=biometrics_marshal ${PROJECT_SOURCE_DIR}/data/biometrics-marshal.list --header > marshal.h
    COMMAND ${GLIB_GENMARSHAL} --prefix=biometrics_marshal ${PROJECT_SOURCE_DIR}/data/biometrics-marshal.list --body --header > marshal.c
    DEPENDS ${PROJECT_SOURCE_DIR}/data/biometrics-marshal.list
)

add_library(pam_kiran_fprintd MODULE pam-kiran-fprintd.c kiran-pam.c marshal.c)
add_dependencies(pam_kiran_fprintd kiran-biometrics-proxy.h)
set_target_properties(pam_kiran_fprintd PROPERTIES PREFIX "")
target_link_libraries(pam_kiran_fprintd pam_misc ${GLIB2_LIBRARIES} ${GDBUS_LIBRARIES})

add_library(pam_kiran_authmode MODULE pam-kiran-authmode.c kiran-pam.c kiran-accounts-gen.c kiran-user-gen.c)
set_target_properties(pam_kiran_authmode PROPERTIES PREFIX "")
target_link_libraries(pam_kiran_authmode pam_misc ${GLIB2_LIBRARIES} ${GDBUS_LIBRARIES} ${GLIB_JSON_LIBRARIES})

if ("${ENABLE_FACE}" STREQUAL "yes")
    add_library(pam_kiran_face MODULE pam-kiran-face.c kiran-pam.c marshal.c)
    add_dependencies(pam_kiran_face kiran-biometrics-proxy.h)
    set_target_properties(pam_kiran_face PROPERTIES PREFIX "")
    target_link_libraries(pam_kiran_face pam_misc ${GLIB2_LIBRARIES} ${GDBUS_LIBRARIES})
    install(TARGETS pam_kiran_face LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/security/)
endif()

install(TARGETS pam_kiran_fprintd LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/security/)
install(TARGETS pam_kiran_authmode LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/security/)
install(FILES kiran-pam-msg.h DESTINATION /usr/include/)
