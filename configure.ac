AC_INIT([openwar], [0.1.0])
AC_CONFIG_SRCDIR([src/campaign.cpp])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_PROG_CXX
AC_PROG_RANLIB

AC_LANG_PUSH([C++])
saved_CXXFLAGS=$CXXFLAGS
CXXFLAGS="$CXXFLAGS -std=c++20"
AC_MSG_CHECKING([whether the C++ compiler supports C++20])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <span>]], [[return std::span<const int>{}.empty() ? 0 : 1;]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_FAILURE([a C++20 compiler and standard library are required])])
CXXFLAGS=$saved_CXXFLAGS
AC_LANG_POP([C++])

AC_ARG_ENABLE([viewer],
    [AS_HELP_STRING([--enable-viewer], [build the optional raylib campaign viewer])],
    [], [enable_viewer=no])
AS_CASE([$enable_viewer], [yes|no], [], [AC_MSG_ERROR([--enable-viewer expects yes or no])])
AC_ARG_VAR([RAYLIB_CFLAGS], [preprocessor flags for raylib])
AC_ARG_VAR([RAYLIB_LIBS], [linker flags for raylib])
AS_IF([test "x$enable_viewer" = xyes], [
    AS_IF([test -z "$RAYLIB_LIBS"], [
        AC_PATH_TOOL([PKG_CONFIG], [pkg-config])
        AS_IF([test -n "$PKG_CONFIG" && "$PKG_CONFIG" --exists raylib], [
            AS_IF([test -z "$RAYLIB_CFLAGS"], [RAYLIB_CFLAGS=`"$PKG_CONFIG" --cflags raylib`])
            RAYLIB_LIBS=`"$PKG_CONFIG" --libs raylib`
        ], [AC_MSG_ERROR([raylib is required for the viewer. Set RAYLIB_CFLAGS and RAYLIB_LIBS if it is installed outside pkg-config.])])
    ])
    AC_LANG_PUSH([C++])
    saved_CPPFLAGS=$CPPFLAGS
    saved_LIBS=$LIBS
    CPPFLAGS="$CPPFLAGS $RAYLIB_CFLAGS"
    LIBS="$RAYLIB_LIBS $LIBS"
    AC_MSG_CHECKING([whether raylib can be linked])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <raylib.h>]], [[return IsWindowReady();]])],
        [AC_MSG_RESULT([yes])],
        [AC_MSG_FAILURE([could not compile and link raylib. Check RAYLIB_CFLAGS and RAYLIB_LIBS.])])
    CPPFLAGS=$saved_CPPFLAGS
    LIBS=$saved_LIBS
    AC_LANG_POP([C++])
])
AM_CONDITIONAL([BUILD_VIEWER], [test "x$enable_viewer" = xyes])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
