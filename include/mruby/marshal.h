/*
** mruby/marshal.h - Marshal class
*/

#ifndef MRUBY_MARSHAL_H
#define MRUBY_MARSHAL_H

#include "mruby/common.h"

MRB_BEGIN_DECL

/**
 * Function pointer type for mruby-marshal-c writer.
 *
 * @param mrb mrb_state
 * @param src source data
 * @param size size of data to write
 * @param dest the target to write, maybe an IO or a String, etc.
 * @param position the write position of dest
 * @return bytes written
 */
typedef mrb_uint (*mrb_marshal_writer_t)(mrb_state *mrb, const void *src, mrb_uint size, mrb_value dest, mrb_uint position);

/**
 * Function pointer type for mruby-marshal-c reader.
 *
 * @param mrb mrb_state
 * @param src the source to read, maybe an IO or a String, etc.
 * @param dest the target to write
 * @param size size of data to read
 * @param position the read position of src
 * @return bytes read
 */
typedef mrb_uint (*mrb_marshal_reader_t)(mrb_state *mrb, mrb_value src, void *dest, mrb_uint size, mrb_uint position);

MRB_API void mrb_marshal_dump(mrb_state *mrb, mrb_value obj, mrb_marshal_writer_t writer, mrb_value target, int limit);
MRB_API mrb_value mrb_marshal_load(mrb_state *mrb, mrb_marshal_reader_t reader, mrb_value source);

MRB_END_DECL

#endif /* MRUBY_MARSHAL_H */
