/* Copyright (c) 2012 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* From pp_macros.idl modified Thu Jan 23 14:14:38 2014. */

#ifndef PPAPI_C_PP_MACROS_H_
#define PPAPI_C_PP_MACROS_H_


#define PPAPI_RELEASE 36

/**
 * @file
 * Defines the common macros such as assert, inline, ...
 */



/*
 * @addtogroup PP
 * @{
 */

/* Use PP_INLINE to tell the compiler to inline functions.  The main purpose of
 * inline functions in ppapi is to allow us to define convenience functions in
 * the ppapi header files, without requiring clients or implementers to link a
 * PPAPI C library.  The "inline" keyword is not supported by pre-C99 C
 * compilers (such as MS Visual Studio 2008 and older versions of GCC).  MSVS
 * supports __forceinline and GCC supports __inline__.  Use of the static
 * keyword ensures (in C) that the function is not compiled on its own, which
 * could cause multiple definition errors.
 *  http://msdn.microsoft.com/en-us/library/z8y1yy88.aspx
 *  http://gcc.gnu.org/onlinedocs/gcc/Inline.html
 */
#if defined(__cplusplus)
/* The inline keyword is part of C++ and guarantees we won't get multiple
 * definition errors.
 */
# define PP_INLINE inline
#else
# if defined(_MSC_VER)
#  define PP_INLINE static __forceinline
# else
#  define PP_INLINE static __inline__
# endif
#endif

/* This is a compile-time assertion useful for ensuring that a given type is
   a given number of bytes wide.  The size of the array is designed to be 1
   (which should always be valid) if the enum's size is SIZE, and otherwise the
   size of the array will be -1 (which all/most compilers should flag as an
   error).  This is wrapped inside a struct, because if it is a simple global
   we get multiple definition errors at link time.

   NAME is the name of the type without any spaces or the struct or enum
   keywords.

   CTYPENAME is the typename required by C.  I.e., for a struct or enum, the
   appropriate keyword must be included.

   SIZE is the expected size in bytes.
 */
#define PP_COMPILE_ASSERT_SIZE_IN_BYTES_IMPL(NAME, CTYPENAME, SIZE) \
struct PP_Dummy_Struct_For_##NAME { \
char _COMPILE_ASSERT_FAILED_The_type_named_ \
## NAME ## _is_not_ ## SIZE ## \
_bytes_wide[(sizeof(CTYPENAME) == SIZE) ? 1 : -1]; }

/* PP_COMPILE_ASSERT_SIZE_IN_BYTES is for typenames that contain no spaces.
   E.g.:
   PP_COMPILE_ASSERT_SIZE_IN_BYTES(int, 4);
   typedef struct { int a; } Foo;
   PP_COMPILE_ASSERT_SIZE_IN_BYTES(Foo, 4);
 */
#define PP_COMPILE_ASSERT_SIZE_IN_BYTES(NAME, SIZE) \
PP_COMPILE_ASSERT_SIZE_IN_BYTES_IMPL(NAME, NAME, SIZE)

/* PP_COMPILE_ASSERT_STRUCT_SIZE_IN_BYTES is for typenames that contain 'struct'
   in C.  That is, struct names that are not typedefs.
   E.g.:
   struct Foo { int a; };
   PP_COMPILE_ASSERT_STRUCT_SIZE_IN_BYTES(Foo, 4);
 */
#define PP_COMPILE_ASSERT_STRUCT_SIZE_IN_BYTES(NAME, SIZE) \
PP_COMPILE_ASSERT_SIZE_IN_BYTES_IMPL(NAME, struct NAME, SIZE)

/* PP_COMPILE_ASSERT_ENUM_SIZE_IN_BYTES is for typenames that contain 'enum'
   in C.  That is, enum names that are not typedefs.
   E.g.:
   enum Bar { A = 0, B = 1 };
   PP_COMPILE_ASSERT_ENUM_SIZE_IN_BYTES(Foo, 4);
 */
#define PP_COMPILE_ASSERT_ENUM_SIZE_IN_BYTES(NAME, SIZE) \
PP_COMPILE_ASSERT_SIZE_IN_BYTES_IMPL(NAME, enum NAME, SIZE)

/* This is roughly copied from base/compiler_specific.h, and makes it possible
   to pass 'this' in a constructor initializer list, when you really mean it.
   E.g.:
   Foo::Foo(MyInstance* instance)
       : PP_ALLOW_THIS_IN_INITIALIZER_LIST(callback_factory_(this)) {}
 */
#if defined(_MSC_VER)
# define PP_ALLOW_THIS_IN_INITIALIZER_LIST(code) \
    __pragma(warning(push)) \
    __pragma(warning(disable:4355)) \
    code \
    __pragma(warning(pop))
#else
# define PP_ALLOW_THIS_IN_INITIALIZER_LIST(code) code
#endif

/**
 * @}
 * End of addtogroup PP
 */

#endif  /* PPAPI_C_PP_MACROS_H_ */

