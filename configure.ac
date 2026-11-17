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

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
