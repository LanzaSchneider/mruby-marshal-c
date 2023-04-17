#include <mruby.h>
#include <mruby/value.h>
#include <mruby/marshal.h>

#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/class.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/object.h>

#include <mruby/presym.h>

#include <string.h>
#include "common.h"

#include <mruby/khash.h>
KHASH_DECLARE(symbol_dump_table, mrb_sym, mrb_int, 1);
KHASH_DECLARE(object_dump_table, mrb_value, mrb_int, 1);

#define kh_mrb_value_hash_func(mrb, v) mrb_obj_id(v)
KHASH_DEFINE(symbol_dump_table, mrb_sym, mrb_int, 1, kh_int_hash_func, kh_int_hash_equal);
KHASH_DEFINE(object_dump_table, mrb_value, mrb_int, 1, kh_mrb_value_hash_func, mrb_equal);

struct dump_arg
{
  mrb_value dest;
  mrb_uint position;
  mrb_marshal_writer_t writer;

  kh_symbol_dump_table_t *symbols;
  kh_object_dump_table_t *data;
};

struct dump_call_arg
{
  // mrb_value obj;
  struct dump_arg *arg;
  int limit;
};

static void
check_dump_arg(mrb_state *mrb, struct dump_arg *arg, mrb_sym sym)
{
  if (!arg->symbols)
  {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "Marshal.dump reentered at %s", mrb_sym_name(mrb, sym));
  }
}

static void w_long(mrb_state *, long, struct dump_arg *);

static void
w_nbyte(mrb_state *mrb, const char *s, long n, struct dump_arg *arg)
{
  arg->position += arg->writer(mrb, s, n, arg->dest, arg->position);
}

static void
w_byte(mrb_state *mrb, char c, struct dump_arg *arg)
{
  w_nbyte(mrb, &c, 1, arg);
}

static void
w_bytes(mrb_state *mrb, const char *s, long n, struct dump_arg *arg)
{
  w_long(mrb, n, arg);
  w_nbyte(mrb, s, n, arg);
}

#define w_cstr(mrb, s, arg) w_bytes(mrb, (s), strlen(s), (arg))

static void
w_short(mrb_state *mrb, int x, struct dump_arg *arg)
{
  w_byte(mrb, (char)((x >> 0) & 0xff), arg);
  w_byte(mrb, (char)((x >> 8) & 0xff), arg);
}

static void
w_long(mrb_state *mrb, long x, struct dump_arg *arg)
{
  char buf[sizeof(long) + 1];
  int i, len = 0;

#if SIZEOF_LONG > 4
  if (!(RSHIFT(x, 31) == 0 || RSHIFT(x, 31) == -1))
  {
    /* big long does not fit in 4 bytes */
    rb_raise(rb_eTypeError, "long too big to dump");
  }
#endif

  if (x == 0)
  {
    w_byte(mrb, 0, arg);
    return;
  }
  if (0 < x && x < 123)
  {
    w_byte(mrb, (char)(x + 5), arg);
    return;
  }
  if (-124 < x && x < 0)
  {
    w_byte(mrb, (char)((x - 5) & 0xff), arg);
    return;
  }
  for (i = 1; i < (int)sizeof(long) + 1; i++)
  {
    buf[i] = (char)(x & 0xff);
    x = RSHIFT(x, 8);
    if (x == 0)
    {
      buf[0] = i;
      break;
    }
    if (x == -1)
    {
      buf[0] = -i;
      break;
    }
  }
  len = i;
  for (i = 0; i <= len; i++)
  {
    w_byte(mrb, buf[i], arg);
  }
}

static void
w_float(mrb_state *mrb, double d, struct dump_arg *arg)
{
  char buf[FLOAT_DIG + (DECIMAL_MANT + 7) / 8 + 10];

  if (isinf(d))
  {
    if (d < 0)
      w_cstr(mrb, "-inf", arg);
    else
      w_cstr(mrb, "inf", arg);
  }
  else if (isnan(d))
  {
    w_cstr(mrb, "nan", arg);
  }
  else if (d == 0.0)
  {
    if (1.0 / d < 0)
      w_cstr(mrb, "-0", arg);
    else
      w_cstr(mrb, "0", arg);
  }
  else
  {
    sprintf(buf, "%lf", (double)d);
    {
      int len = strlen(buf);
      int bound_left = 0, bound_right = len - 1;
      for (int i = 0; i < len; i++)
        if (buf[i] == '.')
        {
          bound_left = i - 1;
          break;
        }
      for (int i = bound_right; i > bound_left; i--)
      {
        if (i < 0)
          break;
        if (buf[i] == '0' || buf[i] == '.')
        {
          buf[i] = '\0';
        }
        else
          break;
      }
      w_bytes(mrb, buf, len, arg);
    }
  }
}

static void
w_symbol(mrb_state *mrb, mrb_sym id, struct dump_arg *arg)
{
  mrb_int num;

  for (khint_t i = 0; i < kh_end(arg->symbols); i++)
  {
    if (kh_exist(arg->symbols, i))
    {
      num = kh_value(arg->symbols, i);
      w_byte(mrb, TYPE_SYMLINK, arg);
      w_long(mrb, (long)num, arg);
      return;
    }
  }

  mrb_value sym = mrb_sym_str(mrb, id);

  w_byte(mrb, TYPE_SYMBOL, arg);
  w_bytes(mrb, RSTRING_PTR(sym), RSTRING_LEN(sym), arg);

  kh_value(arg->symbols, kh_put(symbol_dump_table, mrb, arg->symbols, id)) = kh_n_buckets(arg->symbols);
}

static void
w_unique(mrb_state *mrb, mrb_value s, struct dump_arg *arg)
{
  // TODO: must_not_be_anonymous("class", s);
  w_symbol(mrb, mrb_intern_str(mrb, s), arg);
}

static void w_object(mrb_state *, mrb_value, struct dump_arg *, int);

static int
hash_each(mrb_state *mrb, mrb_value key, mrb_value value, void *ud)
{
  struct dump_call_arg *arg = (struct dump_call_arg *)ud;
  w_object(mrb, key, arg->arg, arg->limit);
  w_object(mrb, value, arg->arg, arg->limit);
  return 0; // continue
}

static void
w_class(mrb_state *mrb, char type, mrb_value obj, struct dump_arg *arg, int check)
{
  mrb_value path;
  struct RClass *klass;

  klass = mrb_obj_class(mrb, obj);
  // TODO: w_extended(mrb, klass, arg, check);
  w_byte(mrb, type, arg);
  path = mrb_class_path(mrb, klass);
  w_unique(mrb, path, arg);
}

static void
w_uclass(mrb_state *mrb, mrb_value obj, struct RClass *super, struct dump_arg *arg)
{
  struct RClass *klass = mrb_obj_class(mrb, obj);

  // TODO: w_extended(mrb, klass, arg, TRUE);
  if (klass != super)
  {
    w_byte(mrb, TYPE_UCLASS, arg);
    w_unique(mrb, mrb_class_path(mrb, klass), arg);
  }
}

static int
w_obj_each(mrb_state *mrb, mrb_sym id, mrb_value value, void *ud)
{
  struct dump_call_arg *arg = (struct dump_call_arg *)ud;
  // if (id == mrb_id_encoding()) return;
  if (id == mrb_intern_cstr(mrb, "E"))
    return 0; // continue
  w_symbol(mrb, id, arg->arg);
  w_object(mrb, value, arg->arg, arg->limit);
  return 0; // continue
}

typedef struct iv_tbl
{
  int size, alloc;
  mrb_value *ptr;
} iv_tbl;

#define IV_DELETED (1UL << 31)
#define IV_KEY_P(k) (((k) & ~((uint32_t)IV_DELETED)) != 0)

/* Iterates over the instance variable table. */
static void
iv_foreach(mrb_state *mrb, iv_tbl *t, mrb_iv_foreach_func *func, void *p)
{
  int i;

  if (t == NULL)
    return;
  if (t->alloc == 0)
    return;
  if (t->size == 0)
    return;

  mrb_sym *keys = (mrb_sym *)&t->ptr[t->alloc];
  mrb_value *vals = t->ptr;
  for (i = 0; i < t->alloc; i++)
  {
    if (IV_KEY_P(keys[i]))
    {
      if ((*func)(mrb, keys[i], vals[i], p) != 0)
      {
        return;
      }
    }
  }
  return;
}

#undef IV_DELETED
#undef IV_KEY_P

static void
w_ivar(mrb_state *mrb, mrb_value obj, struct iv_tbl *tbl, struct dump_call_arg *arg)
{
  // long num = tbl ? tbl->size : 0;
  // w_encoding(obj, num, arg);
  iv_foreach(mrb, tbl, w_obj_each, arg);
}

static void
w_objivar(mrb_state *mrb, mrb_value obj, struct dump_call_arg *arg)
{
  mrb_iv_foreach(mrb, obj, w_obj_each, arg);
}

static void
w_object(mrb_state *mrb, mrb_value obj, struct dump_arg *arg, int limit)
{
  struct dump_call_arg c_arg;
  struct iv_tbl *ivtbl = NULL;

  int hasiv = 0;
#define has_ivars(obj, ivtbl) (mrb_object_p(obj) && (ivtbl = mrb_obj_ptr(obj)->iv))

  if (limit == 0)
  {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "exceed depth limit");
  }

  limit--;
  c_arg.limit = limit;
  c_arg.arg = arg;

  for (khint_t i = 0; i < kh_end(arg->data); i++)
  {
    if (kh_exist(arg->data, i))
    {
      w_byte(mrb, TYPE_LINK, arg);
      w_long(mrb, (long)kh_value(arg->data, i), arg);
      return;
    }
  }

  if (mrb_nil_p(obj))
  {
    w_byte(mrb, TYPE_NIL, arg);
  }
  else if (mrb_true_p(obj))
  {
    w_byte(mrb, TYPE_TRUE, arg);
  }
  else if (mrb_false_p(obj))
  {
    w_byte(mrb, TYPE_FALSE, arg);
  }
  else if (mrb_fixnum_p(obj))
  {
#if SIZEOF_LONG <= 4
    w_byte(mrb, TYPE_FIXNUM, arg);
    w_long(mrb, mrb_fixnum(obj), arg);
#else
    if (RSHIFT((long)obj, 31) == 0 || RSHIFT((long)obj, 31) == -1)
    {
      w_byte(mrb, TYPE_FIXNUM, arg);
      w_long(mrb, FIX2LONG(obj), arg);
    }
    else
    {
      w_object(mrb, rb_int2big(FIX2LONG(obj)), arg, limit);
    }
#endif
  }
  else if (mrb_symbol_p(obj))
  {
    w_symbol(mrb, mrb_symbol(obj), arg);
  }
  else
  {
    // arg->infection |= (int)FL_TEST(obj, MARSHAL_INFECTION);

    if (mrb_respond_to(mrb, obj, s_mdump))
    {
      volatile mrb_value v;

      kh_value(arg->data, kh_put(object_dump_table, mrb, arg->data, obj)) = kh_n_buckets(arg->data);

      v = mrb_funcall_id(mrb, obj, s_mdump, 0);
      check_dump_arg(mrb, arg, s_mdump);
      hasiv = has_ivars(obj, ivtbl);
      if (hasiv)
        w_byte(mrb, TYPE_IVAR, arg);
      w_class(mrb, TYPE_USRMARSHAL, obj, arg, FALSE);
      w_object(mrb, v, arg, limit);
      if (hasiv)
        w_ivar(mrb, obj, ivtbl, &c_arg);
      return;
    }
    if (mrb_respond_to(mrb, obj, s_dump))
    {
      mrb_value v;
      struct iv_tbl *ivtbl2 = 0;
      int hasiv2;

      v = mrb_funcall_id(mrb, obj, s_dump, 1, mrb_fixnum_value(limit));
      check_dump_arg(mrb, arg, s_dump);
      if (!mrb_string_p(v))
      {
        mrb_raise(mrb, E_TYPE_ERROR, "_dump() must return string");
      }
      hasiv = has_ivars(obj, ivtbl);
      if (hasiv)
        w_byte(mrb, TYPE_IVAR, arg);
      if ((hasiv2 = has_ivars(v, ivtbl2)) != 0 && !hasiv)
      {
        w_byte(mrb, TYPE_IVAR, arg);
      }
      w_class(mrb, TYPE_USERDEF, obj, arg, FALSE);
      w_bytes(mrb, RSTRING_PTR(v), RSTRING_LEN(v), arg);
      if (hasiv2)
      {
        w_ivar(mrb, v, ivtbl2, &c_arg);
      }
      else if (hasiv)
      {
        w_ivar(mrb, obj, ivtbl, &c_arg);
      }
      kh_value(arg->data, kh_put(object_dump_table, mrb, arg->data, obj)) = kh_n_buckets(arg->data);
      return;
    }

    kh_value(arg->data, kh_put(object_dump_table, mrb, arg->data, obj)) = kh_n_buckets(arg->data);

    hasiv = has_ivars(obj, ivtbl);
    if (hasiv)
      w_byte(mrb, TYPE_IVAR, arg);

    switch (mrb_type(obj))
    {
    case MRB_TT_CLASS:
      // singleton check ?!
      // if (FL_)
      // {
      //   rb_raise(rb_eTypeError, "singleton class can't be dumped");
      // }
      w_byte(mrb, TYPE_CLASS, arg);
      {
        volatile mrb_value path = mrb_class_path(mrb, (struct RClass *)mrb_obj_ptr(obj));
        w_bytes(mrb, RSTRING_PTR(path), RSTRING_LEN(path), arg);
      }
      break;

    case MRB_TT_MODULE:
      w_byte(mrb, TYPE_MODULE, arg);
      {
        volatile mrb_value path = mrb_class_path(mrb, (struct RClass *)mrb_obj_ptr(obj));
        w_bytes(mrb, RSTRING_PTR(path), RSTRING_LEN(path), arg);
      }
      break;

    case MRB_TT_FLOAT:
      w_byte(mrb, TYPE_FLOAT, arg);
      w_float(mrb, mrb_float(obj), arg);
      break;

    case MRB_TT_STRING:
      w_uclass(mrb, obj, mrb->string_class, arg);
      w_byte(mrb, TYPE_STRING, arg);
      w_bytes(mrb, RSTRING_PTR(obj), RSTRING_LEN(obj), arg);
      break;

      // case T_REGEXP:
      //   w_uclass(mrb, obj, rb_cRegexp, arg);
      //   w_byte(mrb, TYPE_REGEXP, arg);
      //   {
      //     int opts = rb_reg_options(obj);
      //     w_bytes(mrb, RREGEXP_SRC_PTR(obj), RREGEXP_SRC_LEN(obj), arg);
      //     w_byte(mrb, (char)opts, arg);
      //   }
      //   break;

    case MRB_TT_ARRAY:
      w_uclass(mrb, obj, mrb->array_class, arg);
      w_byte(mrb, TYPE_ARRAY, arg);
      {
        long i, len = RARRAY_LEN(obj);

        w_long(mrb, len, arg);
        for (i = 0; i < RARRAY_LEN(obj); i++)
        {
          w_object(mrb, RARRAY_PTR(obj)[i], arg, limit);
          if (len != RARRAY_LEN(obj))
          {
            mrb_raise(mrb, E_RUNTIME_ERROR, "array modified during dump");
          }
        }
      }
      break;

    case MRB_TT_HASH:
      w_uclass(mrb, obj, mrb->hash_class, arg);
      if (!MRB_RHASH_DEFAULT_P(obj))
      {
        w_byte(mrb, TYPE_HASH, arg);
      }
      else if (MRB_RHASH_PROCDEFAULT_P(obj))
      {
        mrb_raise(mrb, E_TYPE_ERROR, "can't dump hash with default proc");
      }
      else
      {
        w_byte(mrb, TYPE_HASH_DEF, arg);
      }
      w_long(mrb, mrb_hash_size(mrb, obj), arg);
      mrb_hash_foreach(mrb, mrb_hash_ptr(obj), hash_each, &c_arg);
      if (MRB_RHASH_DEFAULT_P(obj))
      {
        mrb_raise(mrb, E_TYPE_ERROR, "can't dump hash with default");
        // w_object(mrb, RHASH_IFNONE(obj), arg, limit);
      }
      break;

    case MRB_TT_STRUCT:
      w_class(mrb, TYPE_STRUCT, obj, arg, TRUE);
      {
#define RSTRUCT_LEN(st) RARRAY_LEN(st)
#define RSTRUCT_PTR(st) RARRAY_PTR(st)
        long len = RSTRUCT_LEN(obj);
        mrb_value mem;
        long i;
        w_long(mrb, len, arg);
        mem = mrb_funcall_id(mrb, obj, MRB_SYM(members), 0); // rb_struct_members(obj);
        for (i = 0; i < len; i++)
        {
          w_symbol(mrb, mrb_symbol(RARRAY_PTR(mem)[i]), arg);
          w_object(mrb, RSTRUCT_PTR(obj)[i], arg, limit);
        }
#undef RSTRUCT_LEN
#undef RSTRUCT_PTR
      }
      break;

    case MRB_TT_OBJECT:
      w_class(mrb, TYPE_OBJECT, obj, arg, TRUE);
      w_objivar(mrb, obj, &c_arg);
      break;

    case MRB_TT_DATA:
    {
      mrb_value v;

      if (!mrb_respond_to(mrb, obj, s_dump_data))
      {
        mrb_raisef(mrb, E_TYPE_ERROR, "no _dump_data is defined for class %s", mrb_obj_classname(mrb, obj));
      }
      v = mrb_funcall_id(mrb, obj, s_dump_data, 0);
      check_dump_arg(mrb, arg, s_dump_data);
      w_class(mrb, TYPE_DATA, obj, arg, TRUE);
      w_object(mrb, v, arg, limit);
    }
    break;

    default:
      mrb_raisef(mrb, E_TYPE_ERROR, "can't dump %s", mrb_obj_classname(mrb, obj));
      break;
    }
  }
  if (hasiv)
  {
    w_ivar(mrb, obj, ivtbl, &c_arg);
  }
}

void mrb_marshal_dump(mrb_state *mrb, mrb_value obj, mrb_marshal_writer_t writer, mrb_value target, int limit)
{
  struct dump_arg arg; // TODO: needs gc protection
  arg.dest = target;
  arg.position = 0;
  arg.writer = writer;
  arg.symbols = kh_init(symbol_dump_table, mrb);
  arg.data = kh_init(object_dump_table, mrb);

  w_byte(mrb, MARSHAL_MAJOR, &arg);
  w_byte(mrb, MARSHAL_MINOR, &arg);
  w_object(mrb, obj, &arg, limit);

  kh_destroy(symbol_dump_table, mrb, arg.symbols);
  kh_destroy(object_dump_table, mrb, arg.data);
}
