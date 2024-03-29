#include <mruby.h>
#include <mruby/value.h>
#include <mruby/marshal.h>
#include <mruby/class.h>
#include <mruby/string.h>
#include <mruby/presym.h>

#include "common.h"
#include <string.h>

static int
_writer_string(mrb_state *mrb, const void *src, int size, mrb_value dest, mrb_uint position)
{
  int ai = mrb_gc_arena_save(mrb);
  mrb_str_buf_cat(mrb, dest, (const char *)src, (size_t)size);
  mrb_gc_arena_restore(mrb, ai);
  return size;
}

static int
_writer_io(mrb_state *mrb, const void *src, int size, mrb_value dest, mrb_uint position)
{
  int ai = mrb_gc_arena_save(mrb);
  int written = mrb_as_int(mrb, mrb_funcall_id(mrb, dest, s_write, 1, mrb_str_new(mrb, (const char *)src, size)));
  mrb_gc_arena_restore(mrb, ai);
  return written;
}

static mrb_value
mrb_mruby_marshal_dump(mrb_state *mrb, mrb_value self)
{
  mrb_value obj, io = mrb_nil_value();
  mrb_int limit = -1;
  const mrb_int arg_count = mrb_get_args(mrb, "o|oi", &obj, &io, &limit);
  if (arg_count == 2 && mrb_fixnum_p(io))
  {
    limit = mrb_fixnum(io);
    io = mrb_nil_value();
  }
  if (mrb_nil_p(io))
  {
    mrb_value str = mrb_str_new(mrb, NULL, 0);
    mrb_marshal_dump(mrb, obj, _writer_string, str, limit);
    return str;
  }
  else
  {
    mrb_marshal_dump(mrb, obj, _writer_io, io, limit);
    return io;
  }
}

static int
_reader_string(mrb_state *mrb, mrb_value src, void *dest, int size, mrb_uint position)
{
  int remain = RSTRING_LEN(src) - position;
  if (size < 0)
  {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "negative length %d given", size);
  }
  if (remain > 0)
  {
    int len = remain < size ? remain : size;
    memcpy(dest, RSTRING_PTR(src) + position, len);
    return len;
  }
  return 0;
}

static int
_reader_io(mrb_state *mrb, mrb_value src, void *dest, int size, mrb_uint position)
{
  int ai = mrb_gc_arena_save(mrb);
  mrb_value buf = mrb_funcall_id(mrb, src, s_read, 1, mrb_fixnum_value(size));
  int buf_len = 0;
  if (mrb_string_p(buf))
  {
    memcpy(dest, RSTRING_PTR(buf), RSTRING_LEN(buf));
    buf_len = RSTRING_LEN(buf);
  }
  mrb_gc_arena_restore(mrb, ai);
  return buf_len;
}

static mrb_value
mrb_mruby_marshal_load(mrb_state *mrb, mrb_value self)
{
  mrb_value obj;
  mrb_get_args(mrb, "o", &obj);
  return mrb_string_p(obj)
             ? mrb_marshal_load(mrb, _reader_string, obj)
             : mrb_marshal_load(mrb, _reader_io, obj);
}

void mrb_mruby_marshal_c_gem_init(mrb_state *mrb)
{
  struct RClass *mrb_marshal;
  mrb_marshal = mrb_define_module_id(mrb, MRB_SYM(Marshal));

  mrb_define_module_function_id(mrb, mrb_marshal, MRB_SYM(dump), mrb_mruby_marshal_dump, MRB_ARGS_REQ(1));
  mrb_define_module_function_id(mrb, mrb_marshal, MRB_SYM(load), mrb_mruby_marshal_load, MRB_ARGS_REQ(1));
  mrb_define_module_function_id(mrb, mrb_marshal, MRB_SYM(restore), mrb_mruby_marshal_load, MRB_ARGS_REQ(1));

  mrb_define_const_id(mrb, mrb_marshal, MRB_SYM(MAJOR_VERSION), mrb_fixnum_value(MARSHAL_MAJOR));
  mrb_define_const_id(mrb, mrb_marshal, MRB_SYM(MINOR_VERSION), mrb_fixnum_value(MARSHAL_MINOR));
}

void mrb_mruby_marshal_c_gem_final(mrb_state *mrb)
{
}
