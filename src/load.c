#include <mruby.h>
#include <mruby/value.h>
#include <mruby/marshal.h>

#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/class.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/object.h>
#include <mruby/proc.h>

#include <mruby/presym.h>

#include <stdlib.h>
#include "common.h"

#include <mruby/khash.h>
KHASH_DECLARE(symbol_load_table, mrb_int, mrb_sym, 1);
KHASH_DECLARE(object_load_table, mrb_int, mrb_value, 1);

#define kh_mrb_value_hash_func(mrb, v) mrb_obj_id(v)
KHASH_DEFINE(symbol_load_table, mrb_int, mrb_sym, 1, kh_int_hash_func, kh_int_hash_equal);
KHASH_DEFINE(object_load_table, mrb_int, mrb_value, 1, kh_int_hash_func, kh_int_hash_equal);

struct load_arg
{
  mrb_value src;
  mrb_uint position;
  mrb_marshal_reader_t reader;

  mrb_value *proc;

  kh_symbol_load_table_t *symbols;
  kh_object_load_table_t *data;
};

static void
check_load_arg(mrb_state *mrb, struct load_arg *arg, mrb_sym sym)
{
  if (!arg->symbols)
  {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "Marshal.load reentered at %s", mrb_sym_name(mrb, sym));
  }
}

#define r_entry(mrb, v, arg) r_entry0(mrb, (v), kh_size(arg->data), (arg))
static mrb_value r_object(mrb_state *, struct load_arg *);
static mrb_sym r_symbol(mrb_state *, struct load_arg *);

static mrb_int
r_prepare(mrb_state *mrb, struct load_arg *arg)
{
  mrb_int idx = kh_size(arg->data);
  kh_value(arg->data, kh_put(object_load_table, mrb, arg->data, idx)) = mrb_undef_value();
  return idx;
}

static int
r_byte(mrb_state *mrb, struct load_arg *arg)
{
  int8_t c;
  mrb_uint len = arg->reader(mrb, arg->src, &c, sizeof(c), arg->position);
  if (!len)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "marshal data too short"); // TODO: EOF ERROR
  arg->position += len;
  return c;
}

#if __STDC__
#define SIGN_EXTEND_CHAR(c) ((signed char)(c))
#else /* not __STDC__ */
/* As in Harbison and Steele.  */
#define SIGN_EXTEND_CHAR(c) ((((unsigned char)(c)) ^ 128) - 128)
#endif

static long
r_long(mrb_state *mrb, struct load_arg *arg)
{
  register long x;
  int c = SIGN_EXTEND_CHAR(r_byte(mrb, arg));
  long i;

  if (c == 0)
    return 0;
  if (c > 0)
  {
    if (4 < c && c < 128)
    {
      return c - 5;
    }
    if (c > (int)sizeof(long))
      mrb_raise(mrb, E_TYPE_ERROR, "long too big for this architecture");
    x = 0;
    for (i = 0; i < c; i++)
    {
      x |= (long)r_byte(mrb, arg) << (8 * i);
    }
  }
  else
  {
    if (-129 < c && c < -4)
    {
      return c + 5;
    }
    c = -c;
    if (c > (int)sizeof(long))
      mrb_raise(mrb, E_TYPE_ERROR, "long too big for this architecture");
    x = -1;
    for (i = 0; i < c; i++)
    {
      x &= ~((long)0xff << (8 * i));
      x |= (long)r_byte(mrb, arg) << (8 * i);
    }
  }
  return x;
}

#define r_bytes(mrb, arg) r_bytes0(mrb, r_long(mrb, arg), (arg))

static mrb_value
r_bytes0(mrb_state *mrb, long len, struct load_arg *arg)
{
  mrb_value buf;
  mrb_uint buf_len;
  if (len == 0)
    return mrb_str_new_cstr(mrb, "");
  buf = mrb_str_buf_new(mrb, len);
  buf_len = arg->reader(mrb, arg->src, RSTRING_PTR(buf), len, arg->position);
  if (!buf_len)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "marshal data too short"); // TODO: EOF ERROR
  arg->position += len;
  mrb_str_resize(mrb, buf, buf_len);
  return buf;
}

static mrb_sym
r_symlink(mrb_state *mrb, struct load_arg *arg)
{
  long num = r_long(mrb, arg);

  {
    khint_t i = kh_get(symbol_load_table, mrb, arg->symbols, num);
    if (i != kh_end(arg->symbols) && kh_exist(arg->symbols, i))
    {
      return kh_value(arg->symbols, i);
    }
  }

  mrb_raise(mrb, E_ARGUMENT_ERROR, "bad symbol");
  return ~0;
}

static mrb_sym
r_symreal(mrb_state *mrb, struct load_arg *arg, int ivar)
{
  volatile mrb_value s = r_bytes(mrb, arg);
  mrb_sym id;
  // int idx = -1;
  mrb_int n = kh_size(arg->symbols);

  khint_t x = kh_put(symbol_load_table, mrb, arg->symbols, n);
  kh_value(arg->symbols, x) = 0;
  if (ivar)
  {
    long num = r_long(mrb, arg);
    while (num-- > 0)
    {
      id = r_symbol(mrb, arg);
      // idx = id2encidx(id, r_object(arg));
    }
  }
  // if (idx < 0)
  // idx = rb_usascii_encindex();
  // rb_enc_associate_index(s, idx);
  id = mrb_intern_str(mrb, s);
  kh_value(arg->symbols, x) = id;

  return id;
}

static mrb_sym
r_symbol(mrb_state *mrb, struct load_arg *arg)
{
  int type, ivar = 0;

again:
  switch ((type = r_byte(mrb, arg)))
  {
  case TYPE_IVAR:
    ivar = 1;
    goto again;
  case TYPE_SYMBOL:
    return r_symreal(mrb, arg, ivar);
  case TYPE_SYMLINK:
    if (ivar)
    {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "dump format error (symlink with encoding)");
    }
    return r_symlink(mrb, arg);
  default:
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "dump format error for symbol(0x%d)", type);
    break;
  }
}

static mrb_value
r_unique(mrb_state *mrb, struct load_arg *arg)
{
  return mrb_sym_str(mrb, r_symbol(mrb, arg));
}

static mrb_value
r_string(mrb_state *mrb, struct load_arg *arg)
{
  return r_bytes(mrb, arg);
}

static mrb_value
r_entry0(mrb_state *mrb, mrb_value v, mrb_int num, struct load_arg *arg)
{
  // st_data_t real_obj = (VALUE)Qundef;
  // if (st_lookup(arg->compat_tbl, v, &real_obj))
  // {
  //   st_insert(arg->data, num, (st_data_t)real_obj);
  // }
  // else
  // {
  kh_value(arg->data, kh_put(object_load_table, mrb, arg->data, num)) = v;
  //   st_insert(arg->data, num, (st_data_t)v);
  // }
  // if (arg->infection &&
  //     TYPE(v) != T_CLASS && TYPE(v) != T_MODULE)
  // {
  //   FL_SET(v, arg->infection);
  //   if ((VALUE)real_obj != Qundef)
  //     FL_SET((VALUE)real_obj, arg->infection);
  // }
  return v;
}

static mrb_value
r_leave(mrb_state *mrb, mrb_value v, struct load_arg *arg)
{
  if (arg->proc)
  {
    mrb_assert(mrb_proc_p(*arg->proc));
    v = mrb_funcall_id(mrb, *arg->proc, s_call, 1, v);
    check_load_arg(mrb, arg, s_call);
  }
  return v;
}

static void
r_ivar(mrb_state *mrb, mrb_value obj, int *has_encoding, struct load_arg *arg)
{
  long len;

  len = r_long(mrb, arg);
  if (len > 0)
  {
    do
    {
      mrb_sym id = r_symbol(mrb, arg);
      mrb_value val = r_object(mrb, arg);
      // int idx = id2encidx(id, val);
      // if (idx >= 0)
      // {
      //   rb_enc_associate_index(obj, idx);
      //   if (has_encoding)
      //     *has_encoding = TRUE;
      // }
      // else
      // {
      mrb_iv_set(mrb, obj, id, val);
      //   rb_ivar_set(obj, id, val);
      // }
    } while (--len > 0);
  }
}

static mrb_value
obj_alloc_by_path(mrb_state *mrb, mrb_value path, struct load_arg *arg)
{
  struct RClass *klass;
  klass = mrb_class_get(mrb, RSTRING_CSTR(mrb, path));
  return mrb_obj_value(mrb_obj_alloc(mrb, MRB_INSTANCE_TT(klass), klass));
}

#define load_mantissa(d, buf, len) (d)

static mrb_value
r_object0(mrb_state *mrb, struct load_arg *arg, int *ivp, mrb_value extmod)
{
  mrb_value v = mrb_nil_value();
  int type = r_byte(mrb, arg);
  long id;

  switch (type)
  {
  case TYPE_LINK:
    id = r_long(mrb, arg);

    {
      khint_t i = kh_get(object_load_table, mrb, arg->data, id);
      if (i != kh_end(arg->data) && kh_exist(arg->data, i))
      {
        v = kh_value(arg->data, i);
        if (arg->proc)
        {
          mrb_assert(mrb_proc_p(*arg->proc));
          v = mrb_funcall_id(mrb, *arg->proc, s_call, 1, v);
          check_load_arg(mrb, arg, s_call);
          break;
        }
      }
    }

    mrb_raise(mrb, E_ARGUMENT_ERROR, "dump format error (unlinked)");
    break;

  case TYPE_IVAR:
  {
    int ivar = TRUE;

    v = r_object0(mrb, arg, &ivar, extmod);
    if (ivar)
      r_ivar(mrb, v, NULL, arg);
  }
  break;

    // case TYPE_EXTENDED:
    // {
    //   VALUE m = path2module(r_unique(arg));

    //   if (NIL_P(extmod))
    //     extmod = rb_ary_new2(0);
    //   rb_ary_push(extmod, m);

    //   v = r_object0(arg, 0, extmod);
    //   while (RARRAY_LEN(extmod) > 0)
    //   {
    //     m = rb_ary_pop(extmod);
    //     rb_extend_object(v, m);
    //   }
    // }
    // break;

  case TYPE_UCLASS:
  {
    struct RClass *c = mrb_class_get(mrb, RSTRING_CSTR(mrb, r_unique(mrb, arg)));

    // if (FL_TEST(c, FL_SINGLETON))
    // {
    //   rb_raise(rb_eTypeError, "singleton can't be loaded");
    // }
    v = r_object0(mrb, arg, 0, extmod);
    if (mrb_object_p(v) || mrb_class_p(v)) // rb_special_const_p(v) || TYPE(v) == T_OBJECT || TYPE(v) == T_CLASS)
    {
    format_error:
      mrb_raise(mrb, E_ARGUMENT_ERROR, "dump format error (user class)");
    }
    if (mrb_module_p(v))
      ; // TYPE(v) == T_MODULE || !RTEST(rb_class_inherited_p(c, RBASIC(v)->klass)))
    {
      mrb_value tmp = mrb_obj_value(mrb_obj_alloc(mrb, MRB_INSTANCE_TT(c), c));

      if (mrb_type(v) != mrb_type(tmp))
        goto format_error;
    }
    // RBASIC(v)->klass = c;
  }
  break;

  case TYPE_NIL:
    v = mrb_nil_value();
    v = r_leave(mrb, v, arg);
    break;

  case TYPE_TRUE:
    v = mrb_true_value();
    v = r_leave(mrb, v, arg);
    break;

  case TYPE_FALSE:
    v = mrb_false_value();
    v = r_leave(mrb, v, arg);
    break;

  case TYPE_FIXNUM:
  {
    long i = r_long(mrb, arg);
    v = mrb_fixnum_value(i);
  }
    v = r_leave(mrb, v, arg);
    break;

  case TYPE_FLOAT:
  {
    double d;
    mrb_value str = r_bytes(mrb, arg);
    const char *ptr = RSTRING_PTR(str);

    if (strcmp(ptr, "nan") == 0)
    {
      d = NAN;
    }
    else if (strcmp(ptr, "inf") == 0)
    {
      d = INFINITY;
    }
    else if (strcmp(ptr, "-inf") == 0)
    {
      d = -INFINITY;
    }
    else
    {
      char *e;
      d = strtod(ptr, &e);
      d = load_mantissa(d, e, RSTRING_LEN(str) - (e - ptr));
    }
    v = mrb_float_value(mrb, d);
    v = r_entry(mrb, v, arg);
    v = r_leave(mrb, v, arg);
  }
  break;

    //   case TYPE_BIGNUM:
    //   {
    //     long len;
    //     BDIGIT *digits;
    //     volatile VALUE data;

    //     NEWOBJ(big, struct RBignum);
    //     OBJSETUP(big, rb_cBignum, T_BIGNUM);
    //     RBIGNUM_SET_SIGN(big, (r_byte(arg) == '+'));
    //     len = r_long(arg);
    //     data = r_bytes0(len * 2, arg);
    // #if SIZEOF_BDIGITS == SIZEOF_SHORT
    //     rb_big_resize((VALUE)big, len);
    // #else
    //     rb_big_resize((VALUE)big, (len + 1) * 2 / sizeof(BDIGIT));
    // #endif
    //     digits = RBIGNUM_DIGITS(big);
    //     MEMCPY(digits, RSTRING_PTR(data), char, len * 2);
    // #if SIZEOF_BDIGITS > SIZEOF_SHORT
    //     MEMZERO((char *)digits + len * 2, char,
    //             RBIGNUM_LEN(big) * sizeof(BDIGIT) - len * 2);
    // #endif
    //     len = RBIGNUM_LEN(big);
    //     while (len > 0)
    //     {
    //       unsigned char *p = (unsigned char *)digits;
    //       BDIGIT num = 0;
    // #if SIZEOF_BDIGITS > SIZEOF_SHORT
    //       int shift = 0;
    //       int i;

    //       for (i = 0; i < SIZEOF_BDIGITS; i++)
    //       {
    //         num |= (int)p[i] << shift;
    //         shift += 8;
    //       }
    // #else
    //       num = p[0] | (p[1] << 8);
    // #endif
    //       *digits++ = num;
    //       len--;
    //     }
    //     v = rb_big_norm((VALUE)big);
    //     v = r_entry(v, arg);
    //     v = r_leave(v, arg);
    //   }
    //   break;

  case TYPE_STRING:
    v = r_entry(mrb, r_string(mrb, arg), arg);
    v = r_leave(mrb, v, arg);
    break;

    // case TYPE_REGEXP:
    // {
    //   volatile VALUE str = r_bytes(arg);
    //   int options = r_byte(arg);
    //   int has_encoding = FALSE;
    //   st_index_t idx = r_prepare(arg);

    //   if (ivp)
    //   {
    //     r_ivar(str, &has_encoding, arg);
    //     *ivp = FALSE;
    //   }
    //   if (!has_encoding)
    //   {
    //     /* 1.8 compatibility; remove escapes undefined in 1.8 */
    //     char *ptr = RSTRING_PTR(str), *dst = ptr, *src = ptr;
    //     long len = RSTRING_LEN(str);
    //     long bs = 0;
    //     for (; len-- > 0; *dst++ = *src++)
    //     {
    //       switch (*src)
    //       {
    //       case '\\':
    //         bs++;
    //         break;
    //       case 'g':
    //       case 'h':
    //       case 'i':
    //       case 'j':
    //       case 'k':
    //       case 'l':
    //       case 'm':
    //       case 'o':
    //       case 'p':
    //       case 'q':
    //       case 'u':
    //       case 'y':
    //       case 'E':
    //       case 'F':
    //       case 'H':
    //       case 'I':
    //       case 'J':
    //       case 'K':
    //       case 'L':
    //       case 'N':
    //       case 'O':
    //       case 'P':
    //       case 'Q':
    //       case 'R':
    //       case 'S':
    //       case 'T':
    //       case 'U':
    //       case 'V':
    //       case 'X':
    //       case 'Y':
    //         if (bs & 1)
    //           --dst;
    //       default:
    //         bs = 0;
    //         break;
    //       }
    //     }
    //     rb_str_set_len(str, dst - ptr);
    //   }
    //   v = r_entry0(rb_reg_new_str(str, options), idx, arg);
    //   v = r_leave(v, arg);
    // }
    // break;

  case TYPE_ARRAY:
  {
    volatile long len = r_long(mrb, arg); /* gcc 2.7.2.3 -O2 bug?? */

    v = mrb_ary_new_capa(mrb, len);
    v = r_entry(mrb, v, arg);
    while (len--)
    {
      mrb_ary_push(mrb, v, r_object(mrb, arg));
    }
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_HASH:
  case TYPE_HASH_DEF:
  {
    long len = r_long(mrb, arg);

    v = mrb_hash_new(mrb);
    v = r_entry(mrb, v, arg);
    while (len--)
    {
      mrb_value key = r_object(mrb, arg);
      mrb_value value = r_object(mrb, arg);
      mrb_hash_set(mrb, v, key, value);
    }
    if (type == TYPE_HASH_DEF)
    {
      mrb_raise(mrb, E_TYPE_ERROR, "can't load hash with default");
      // RHASH_IFNONE(v) = r_object(mrb, arg);
    }
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_STRUCT:
  {
    mrb_value mem, values;
    volatile long i; /* gcc 2.7.2.3 -O2 bug?? */
    mrb_sym slot;
    mrb_int idx = r_prepare(mrb, arg);
    struct RClass *klass = mrb_class_get(mrb, RSTRING_CSTR(mrb, r_unique(mrb, arg)));
    long len = r_long(mrb, arg);

    v = mrb_obj_value(mrb_obj_alloc(mrb, MRB_INSTANCE_TT(klass), klass));
    if (mrb_type(v) != MRB_TT_STRUCT)
    {
      mrb_raisef(mrb, E_TYPE_ERROR, "class %s not a struct", mrb_class_name(mrb, klass));
    }
    mem = mrb_funcall_id(mrb, mrb_obj_value(klass), MRB_SYM(members), 0); // rb_struct_s_members(klass);
    if (RARRAY_LEN(mem) != len)
    {
      mrb_raisef(mrb, E_TYPE_ERROR, "struct %s not compatible (struct size differs)", mrb_class_name(mrb, klass));
    }

    v = r_entry0(mrb, v, idx, arg);
    values = mrb_ary_new_capa(mrb, len);
    for (i = 0; i < len; i++)
    {
      slot = r_symbol(mrb, arg);

      if (mrb_symbol(RARRAY_PTR(mem)[i]) != slot)
      {
        mrb_raisef(mrb, E_TYPE_ERROR, "struct %s not compatible (:%s for :%s)", mrb_class_name(mrb, klass), mrb_sym_name(mrb, slot), mrb_sym_name(mrb, mrb_symbol(RARRAY_PTR(mem)[i])));
      }
      mrb_ary_push(mrb, values, r_object(mrb, arg));
    }
    mrb_funcall_argv(mrb, v, MRB_SYM(initialize), len, RARRAY_PTR(values)); // rb_struct_initialize(v, values);
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_USERDEF:
  {
    struct RClass *klass = mrb_class_get(mrb, RSTRING_CSTR(mrb, r_unique(mrb, arg)));
    mrb_value data;

    if (!mrb_respond_to(mrb, mrb_obj_value(klass), s_load))
    {
      mrb_raisef(mrb, E_TYPE_ERROR, "class %s needs to have method `_load'", mrb_class_name(mrb, klass));
    }
    data = r_string(mrb, arg);
    if (ivp)
    {
      r_ivar(mrb, data, NULL, arg);
      *ivp = FALSE;
    }
    v = mrb_funcall_id(mrb, mrb_obj_value(klass), s_load, 1, data);
    check_load_arg(mrb, arg, s_load);
    v = r_entry(mrb, v, arg);
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_USRMARSHAL:
  {
    struct RClass *klass = mrb_class_get(mrb, RSTRING_CSTR(mrb, r_unique(mrb, arg)));
    mrb_value data;

    v = mrb_obj_value(mrb_obj_alloc(mrb, MRB_INSTANCE_TT(klass), klass));
    if (!mrb_nil_p(extmod))
    {
      // TODO: extend
      // while (RARRAY_LEN(extmod) > 0)
      // {
      //   VALUE m = rb_ary_pop(extmod);
      //   rb_extend_object(v, m);
      // }
    }
    if (!mrb_respond_to(mrb, v, s_mload))
    {
      mrb_raisef(mrb, E_TYPE_ERROR, "instance of %s needs to have method `marshal_load'", mrb_class_name(mrb, klass));
    }
    v = r_entry(mrb, v, arg);
    data = r_object(mrb, arg);
    mrb_funcall_id(mrb, v, s_mload, 1, data);
    check_load_arg(mrb, arg, s_mload);
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_OBJECT:
  {
    mrb_int idx = r_prepare(mrb, arg);
    v = obj_alloc_by_path(mrb, r_unique(mrb, arg), arg);
    if (!mrb_object_p(v))
    {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "dump format error");
    }
    v = r_entry0(mrb, v, idx, arg);
    r_ivar(mrb, v, NULL, arg);
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_DATA:
  {
    struct RClass *klass = mrb_class_get(mrb, RSTRING_CSTR(mrb, r_unique(mrb, arg)));
    if (mrb_respond_to(mrb, mrb_obj_value(klass), s_alloc))
    {
      static int warn = TRUE;
      if (warn)
      {
        mrb_warn(mrb, "define `allocate' instead of `_alloc'");
        warn = FALSE;
      }
      v = mrb_funcall_id(mrb, mrb_obj_value(klass), s_alloc, 0);
      check_load_arg(mrb, arg, s_alloc);
    }
    else
    {
      v = mrb_obj_value(mrb_obj_alloc(mrb, MRB_INSTANCE_TT(klass), klass));
    }
    if (!mrb_data_p(v))
    {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "dump format error");
    }
    v = r_entry(mrb, v, arg);
    if (!mrb_respond_to(mrb, v, s_load_data))
    {
      mrb_raisef(mrb, E_TYPE_ERROR, "class %s needs to have instance method `_load_data'", mrb_class_name(mrb, klass));
    }
    mrb_funcall_id(mrb, v, s_load_data, 1, r_object0(mrb, arg, 0, extmod));
    check_load_arg(mrb, arg, s_load_data);
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_MODULE_OLD:
  {
    volatile mrb_value str = r_bytes(mrb, arg);

    v = mrb_obj_value(mrb_class_get(mrb, RSTRING_CSTR(mrb, str)));
    v = r_entry(mrb, v, arg);
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_CLASS:
  {
    volatile mrb_value str = r_bytes(mrb, arg);

    v = mrb_obj_value(mrb_class_get(mrb, RSTRING_CSTR(mrb, str)));
    v = r_entry(mrb, v, arg);
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_MODULE:
  {
    volatile mrb_value str = r_bytes(mrb, arg);

    v = mrb_obj_value(mrb_module_get(mrb, RSTRING_CSTR(mrb, str)));
    v = r_entry(mrb, v, arg);
    v = r_leave(mrb, v, arg);
  }
  break;

  case TYPE_SYMBOL:
    if (ivp)
    {
      v = mrb_symbol_value(r_symreal(mrb, arg, *ivp));
      *ivp = FALSE;
    }
    else
    {
      v = mrb_symbol_value(r_symreal(mrb, arg, 0));
    }
    v = r_leave(mrb, v, arg);
    break;

  case TYPE_SYMLINK:
    v = mrb_symbol_value(r_symlink(mrb, arg));
    break;

  default:
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "dump format error(0x%d)", type);
    break;
  }
  return v;
}

static mrb_value
r_object(mrb_state *mrb, struct load_arg *arg)
{
  return r_object0(mrb, arg, 0, mrb_nil_value());
}

static void
clear_load_arg(mrb_state *mrb, struct load_arg *arg)
{
  kh_destroy(symbol_load_table, mrb, arg->symbols);
  kh_destroy(object_load_table, mrb, arg->data);
}

mrb_value mrb_marshal_load(mrb_state *mrb, mrb_marshal_reader_t reader, mrb_value source)
{
  struct load_arg arg; // TODO: needs gc protection
  arg.src = source;
  arg.position = 0;
  arg.reader = reader;
  arg.symbols = kh_init(symbol_load_table, mrb);
  arg.data = kh_init(object_load_table, mrb);
  arg.proc = NULL;

  mrb_value v;

  int major, minor;

  major = r_byte(mrb, &arg);
  minor = r_byte(mrb, &arg);

  if (major != MARSHAL_MAJOR || minor > MARSHAL_MINOR)
  {
    clear_load_arg(mrb, &arg);
    mrb_raisef(mrb, E_TYPE_ERROR, "incompatible marshal file format (can't be read)\n\
\tformat version %d.%d required; %d.%d given",
               MARSHAL_MAJOR, MARSHAL_MINOR, major, minor);
  }

  v = r_object(mrb, &arg);
  clear_load_arg(mrb, &arg);

  return v;
}
