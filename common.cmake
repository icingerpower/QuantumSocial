# Shared CMake helpers for the Qt/QCoro layer.
#
# setup_qcoro(COMPONENTS ...) locates the QCoro6 package next to the configured
# Qt install (copied from ../AmazonTemplate3/common.cmake so QuantumSocial does
# not depend on that sibling tree at build time). Call AFTER find_package(Qt6...)
# in the consuming subproject; QT_VERSION_MAJOR / Qt6_DIR must already be set.
include_guard(GLOBAL)

macro(setup_qcoro)
    if(NOT QT_VERSION_MAJOR)
        set(QT_VERSION_MAJOR 6)
    endif()
    set(Qt_DIR_HINT "${Qt${QT_VERSION_MAJOR}_DIR}")
    message(STATUS "QCoro: Qt hint ${Qt_DIR_HINT}")
    set(QCoro_DIR_HINT_1 "/usr/local/lib/cmake/QCoro6")
    set(QCoro_DIR_HINT_2 "/usr/lib/cmake/QCoro6")
    set(QCoro_DIR_HINT_3 "${Qt_DIR_HINT}/../../../../lib/cmake/QCoro6")
    set(QCoro_DIR_HINT_4 "${Qt_DIR_HINT}/../../../../lib/cmake/lib/cmake/QCoro6")
    set(QCoro_DIR_HINT_5 "${Qt_DIR_HINT}/../QCoro6")
    set(QCoro_DIR_HINT_6 "${Qt_DIR_HINT}/../lib/cmake/QCoro6")
    message("QCoro hints: ${QCoro_DIR_HINT_1} ; ${QCoro_DIR_HINT_2} ; ${QCoro_DIR_HINT_3} ; ${QCoro_DIR_HINT_4} ; ${QCoro_DIR_HINT_5} ; ${QCoro_DIR_HINT_6}")
    list(APPEND CMAKE_PREFIX_PATH "${Qt_DIR_HINT}/../../../../lib/cmake")
    list(APPEND CMAKE_PREFIX_PATH "${Qt_DIR_HINT}/..")
    find_package(QCoro6 REQUIRED ${ARGN}
        HINTS
            "${QCoro_DIR_HINT_1}"
            "${QCoro_DIR_HINT_2}"
            "${QCoro_DIR_HINT_3}"
            "${QCoro_DIR_HINT_4}"
            "${QCoro_DIR_HINT_5}"
            "${QCoro_DIR_HINT_6}"
    )
endmacro()
