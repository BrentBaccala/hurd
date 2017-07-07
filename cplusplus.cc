/* -*- mode: C; indent-tabs-mode: nil -*-

 A test program to show how Hurd's syntax in
 libshouldbeinlibc/refcount.h breaks g++.

 (try to compile this program)
*/

#include <stdint.h>

typedef union _references refcounts_t;

struct references {
  /* We chose the layout of this struct so that when it is used in the
     union _references, the hard reference counts occupy the least
     significant bits.  We rely on this layout for atomic promotion
     and demotion of references.  See refcounts_promote and
     refcounts_demote for details.  */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint32_t hard;
  uint32_t weak;
#else
  uint32_t weak;
  uint32_t hard;
#endif
};

/* We use a union to convert struct reference values to uint64_t which
   we can manipulate atomically.  While this behavior is not
   guaranteed by the C standard, it is supported by all major
   compilers.  */
union _references {
  struct references references;
  uint64_t value;
};

void
refcounts_promote (refcounts_t *ref, struct references *result)
{
  /* To promote a weak reference, we need to atomically subtract 1
     from the weak reference count, and add 1 to the hard reference
     count.

     We can subtract by 1 by adding the two's complement of 1 = ~0 to
     a fixed-width value, discarding the overflow.

     We do the same in our uint64_t value, but we have chosen the
     layout of struct references so that when it is used in the union
     _references, the weak reference counts occupy the most
     significant bits.  When we add ~0 to the weak references, the
     overflow will be discarded as unsigned arithmetic is modulo 2^n.
     So we just add a hard reference.  In combination, this is the
     desired operation.  */
  struct references oop = { weak : ~0U, hard : 1};
  const union _references op =
    { references : { weak : ~0U, hard : 1} };
}
