#include <mruby.h>
#include <mruby/data.h>
#include <mruby/hash.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/value.h>
#include <mruby/string.h>
#include <mruby/numeric.h>
#include <mruby/variable.h>

#include <stdint.h>
#include <string.h>
#include <math.h>

#if (MRUBY_RELEASE_MAJOR >= 2) && (MRUBY_RELEASE_MINOR >= 1)
	#define mrb_sym2name mrb_sym_name
	#define mrb_sym2name_len mrb_sym_name_len
	#define mrb_sym2str mrb_sym_str
#endif

#define MAJOR_VERSION 4
#define MINOR_VERSION 8

#define MARSHAL_SUCCESS 		0
#define MARSHAL_UNKNOWN			1
// =======================================================
// Loader
// =======================================================
struct marshal_loader {
	mrb_value source;
	unsigned source_pos;
	unsigned(*load_func)(mrb_state* mrb, struct marshal_loader* loader, unsigned size, void* dest);
	mrb_value linked_objects;
	mrb_value linked_symbols;
};

static unsigned marshal_loadfunc_for_string(mrb_state* mrb, struct marshal_loader* loader, unsigned size, void* dest) {
	unsigned result_len = size;
	if ( RSTRING_LEN(loader->source) - loader->source_pos < result_len ) {
		result_len = RSTRING_LEN(loader->source) - loader->source_pos;
	}
	if ( result_len ) {
		memcpy(dest, RSTRING_PTR(loader->source) + loader->source_pos, result_len);
		loader->source_pos += result_len;
	}
	return result_len;
}

static unsigned marshal_loadfunc_for_reader(mrb_state* mrb, struct marshal_loader* loader, unsigned size, void* dest) {
	mrb_value string = mrb_funcall(mrb, loader->source, "read", 1, mrb_fixnum_value(size));
	if ( mrb_string_p(string) ) {
		unsigned result_len = RSTRING_LEN(string);
		memcpy(dest, RSTRING_PTR(string), result_len);
		loader->source_pos += result_len;
		return result_len;
	}
	return 0;
}

static uint8_t marshal_loader_byte(mrb_state* mrb, struct marshal_loader* loader) {
	uint8_t byte;
	loader->load_func(mrb, loader, 1, &byte);
	return byte;
}

static unsigned marshal_loader_read(mrb_state* mrb, struct marshal_loader* loader, unsigned size, void* dest) {
	return loader->load_func(mrb, loader, size, dest);
}

static int marshal_loader_initialize(mrb_state* mrb, mrb_value source, struct marshal_loader* loader) {
	loader->source = source;
	loader->source_pos = 0;
	if ( mrb_string_p(source) ) {
		loader->load_func = marshal_loadfunc_for_string;
	} else if ( mrb_respond_to(mrb, source, mrb_intern_lit(mrb, "read")) ) {
		loader->load_func = marshal_loadfunc_for_reader;
	} else {
		return MARSHAL_UNKNOWN;
	}
	loader->linked_objects = mrb_ary_new(mrb);
	loader->linked_symbols = mrb_ary_new(mrb);
	return MARSHAL_SUCCESS;
}

static void marshal_loader_register_link(mrb_state* mrb, struct marshal_loader* loader, mrb_value* value) {
	mrb_ary_push(mrb, loader->linked_objects, *value);
}

static struct RClass* marshal_loader_path2cls(mrb_state* mrb, const char* path_begin, mrb_int len) {
	char* begin = (char*)path_begin;
	char* p = begin;
	char* end = begin + len;
	struct RClass* ret = mrb->object_class;
	for ( ;; ) {
		while ( (p < end && p[0] != ':') ||
				((p + 1) < end && p[1] != ':') ) p++;
		
		mrb_sym const cls = mrb_intern(mrb, begin, p - begin);
		if (!mrb_mod_cv_defined(mrb, ret, cls)) {
			mrb_raisef(mrb, mrb_class_get(mrb, "ArgumentError"), "undefined class/module %S", mrb_str_new(mrb, path_begin, p - path_begin));
		}

		mrb_value const cnst = mrb_mod_cv_get(mrb, ret, cls);
		if (mrb_type(cnst) != MRB_TT_CLASS &&  mrb_type(cnst) != MRB_TT_MODULE) {
			mrb_raisef(mrb, mrb_class_get(mrb, "TypeError"), "%S does not refer to class/module", mrb_str_new(mrb, path_begin, p - path_begin));
		}
		ret = mrb_class_ptr(cnst);

		if(p >= end) break;

		p += 2;
		begin = p;
	}
	return ret;
}

static mrb_value marshal_loader_marshal(mrb_state* mrb, struct marshal_loader* loader);

static void number_too_big(mrb_state* mrb) {
	mrb_raise(mrb, E_NOTIMP_ERROR, "number too big");
}

static mrb_int marshal_loader_fixnum(mrb_state* mrb, struct marshal_loader* loader) {
	mrb_int const c = (signed char)(marshal_loader_byte(mrb, loader));
	if(c == 0) return 0;
	else if(c > 0) {
		if(4 < c && c < 128) { return c - 5; }
		if(c > sizeof(mrb_int)) { number_too_big(mrb); }
		mrb_int ret = 0;
		for(mrb_int i = 0; i < c; ++i) {
			ret |= (mrb_int)(marshal_loader_byte(mrb, loader)) << (8 * i);
		}
		return ret;
	} else {
		if(-129 < c && c < -4) { return c + 5; }
		mrb_int const len = -c;
		if(len > sizeof(mrb_int)) { number_too_big(mrb); }
		mrb_int ret = ~0;
		for(mrb_int i = 0; i < len; ++i) {
			ret &= ~(0xff << (8 * i));
			ret |= (mrb_int)(marshal_loader_byte(mrb, loader)) << (8 * i);
		}
		return ret;
	}
}

static mrb_value marshal_loader_string(mrb_state* mrb, struct marshal_loader* loader) {
	mrb_int len = marshal_loader_fixnum(mrb, loader);
	mrb_value buf = mrb_str_new(mrb, NULL, 0);
	mrb_str_resize(mrb, buf, len);
	marshal_loader_read(mrb, loader, len, RSTRING_PTR(buf));
	return buf;
}

static mrb_sym marshal_loader_symbol(mrb_state* mrb, struct marshal_loader* loader) {
	mrb_value symbol = marshal_loader_marshal(mrb, loader);
	if ( !mrb_symbol_p(symbol) ) {
		mrb_raise(mrb, E_TYPE_ERROR, "request symbol");
	}
	return mrb_symbol(symbol);
}

static mrb_value marshal_loader_marshal(mrb_state* mrb, struct marshal_loader* loader) {
	char type = (char)marshal_loader_byte(mrb, loader);
	switch ( type ) {
	case '0': return mrb_nil_value();
	case 'T': return mrb_true_value();
	case 'F': return mrb_false_value();
	case 'i': return mrb_fixnum_value(marshal_loader_fixnum(mrb, loader));
	case 'l': // break; // TODO: Bignum
	{
		mrb_value value;
		mrb_float floatia = 0; 
		int8_t sign = (int8_t)marshal_loader_byte(mrb, loader);
		mrb_int len = marshal_loader_fixnum(mrb, loader);
		unsigned exp = 0;
		for ( ; exp < len * 2; exp++ ) {
			floatia += pow(marshal_loader_byte(mrb, loader) * 2, exp * 8);
		}
		floatia *= sign;
		value = mrb_float_value(mrb, floatia);
		marshal_loader_register_link(mrb, loader, &value);
		return value;
	}
	case ':': {
		mrb_value symbol = mrb_symbol_value(mrb_intern_str(mrb, marshal_loader_string(mrb, loader)));
		mrb_ary_push(mrb, loader->linked_symbols, symbol);
		return symbol;
	}
	case '"': {
		mrb_value string = marshal_loader_string(mrb, loader);
		marshal_loader_register_link(mrb, loader, &string);
		return string;
	}
	case 'I': {
		mrb_value object = marshal_loader_marshal(mrb, loader);
		mrb_int iv_len = marshal_loader_fixnum(mrb, loader);
		mrb_int i = 0;
		for ( ; i < iv_len; i++ ) {
			mrb_sym symbol = marshal_loader_symbol(mrb, loader);
			mrb_value value = marshal_loader_marshal(mrb, loader);
			if (!strcmp("E", mrb_sym2name(mrb, symbol))) continue;
			mrb_iv_set(mrb, object, symbol, value);
		}
		return object;
	}
	case '[': {
		mrb_value array = mrb_ary_new(mrb);
		mrb_int len = marshal_loader_fixnum(mrb, loader);
		mrb_int i = 0;
		marshal_loader_register_link(mrb, loader, &array);
		for ( ; i < len; i++ ) {
			mrb_ary_push(mrb, array, marshal_loader_marshal(mrb, loader));
		}
		return array;
	}
	case '{': {
		mrb_value hash = mrb_hash_new(mrb);
		mrb_int len = marshal_loader_fixnum(mrb, loader);
		mrb_int i = 0;
		marshal_loader_register_link(mrb, loader, &hash);
		for ( ; i < len; i++ ) {
			mrb_value key = marshal_loader_marshal(mrb, loader);
			mrb_value value = marshal_loader_marshal(mrb, loader);
			mrb_hash_set(mrb, hash, key, value);
		}
		return hash;
	}
	case 'f': {
		mrb_value string = marshal_loader_string(mrb, loader);
		mrb_float floatia;
		mrb_value value;
		char buffer[128];
		if ( RSTRING_LEN(string) >= 127 ) {
			mrb_raise(mrb, E_NOTIMP_ERROR, "float data too long");
		}
		memcpy(buffer, RSTRING_PTR(string), RSTRING_LEN(string));
		buffer[RSTRING_LEN(string)] = '\0';
		{
			double floatinho;
			sscanf(buffer, "%lf", &floatinho);
			floatia = (mrb_float)floatinho;
		}
		value = mrb_float_value(mrb, floatia);
		marshal_loader_register_link(mrb, loader, &value);
		return value;
	}
	case 'c': 
	case 'm': {
		mrb_value path = marshal_loader_string(mrb, loader);
		struct RClass* cls = marshal_loader_path2cls(mrb, RSTRING_PTR(path), RSTRING_LEN(path));
		mrb_value value = mrb_obj_value(cls);
		marshal_loader_register_link(mrb, loader, &value);
		return value;
	}
	case 'S': {
		mrb_sym path_symbol = marshal_loader_symbol(mrb, loader);
		mrb_int path_len = 0;
		const char* path = mrb_sym2name_len(mrb, path_symbol, &path_len);
		struct RClass* cls = marshal_loader_path2cls(mrb, path, path_len);
		
		mrb_int member_count = marshal_loader_fixnum(mrb, loader);
		mrb_value struct_symbols = mrb_iv_get(mrb, mrb_obj_value(cls), mrb_intern_lit(mrb, "__members__"));
		mrb_check_type(mrb, struct_symbols, MRB_TT_ARRAY);
		if (member_count != RARRAY_LEN(struct_symbols)) {
			mrb_raisef(mrb, E_TYPE_ERROR, "struct %S not compatible (struct size differs)", mrb_symbol_value(path_symbol));
		}
		
		mrb_value object = mrb_obj_new(mrb, cls, 0, NULL);
		marshal_loader_register_link(mrb, loader, &object);
		
		mrb_value symbols = mrb_ary_new_capa(mrb, member_count);
		mrb_value values = mrb_ary_new_capa(mrb, member_count);

		mrb_int i;
		
		for (i = 0; i < member_count; ++i) {
			mrb_ary_push(mrb, symbols, mrb_symbol_value(marshal_loader_symbol(mrb, loader)));
			mrb_ary_push(mrb, values, marshal_loader_marshal(mrb, loader));
		}

		for (i = 0; i < member_count; ++i) {
			mrb_value src_sym = mrb_ary_ref(mrb, symbols, i);
			mrb_value dst_sym = mrb_ary_ref(mrb, struct_symbols, i);
			if (!mrb_obj_eq(mrb, src_sym, dst_sym)) {
				mrb_raisef(mrb, E_TYPE_ERROR, "struct %S not compatible (:%S for :%S)", mrb_symbol_value(path_symbol), src_sym, dst_sym);
			}
		}
		
		for (i = 0; i < member_count; ++i) {
			RARRAY_PTR(object)[i] = RARRAY_PTR(values)[i];
		}
		return object;
	}
	case '/': {
		mrb_value regexp = mrb_funcall(mrb, mrb_obj_value(mrb_class_get(mrb, "Regexp")), "new", 2, marshal_loader_string(mrb, loader), marshal_loader_fixnum(mrb, loader));
		marshal_loader_register_link(mrb, loader, &regexp);
		return regexp;
	}
	case 'o': {
		mrb_int path_len = 0;
		const char* path = mrb_sym2name_len(mrb, marshal_loader_symbol(mrb, loader), &path_len);
		struct RClass* cls = marshal_loader_path2cls(mrb, path, path_len);
		enum mrb_vtype ttype = MRB_INSTANCE_TT(cls);
		if (ttype == 0) ttype = MRB_TT_OBJECT;
		mrb_value object = mrb_obj_value(mrb_obj_alloc(mrb, ttype, cls));
		marshal_loader_register_link(mrb, loader, &object);
		mrb_int iv_len = marshal_loader_fixnum(mrb, loader);
		mrb_int i = 0;
		for ( ; i < iv_len; i++ ) {
			mrb_sym symbol = marshal_loader_symbol(mrb, loader);
			mrb_value value = marshal_loader_marshal(mrb, loader);
			mrb_iv_set(mrb, object, symbol, value);
		}
		return object;
	}
	case 'C': {
		mrb_int path_len = 0;
		const char* path = mrb_sym2name_len(mrb, marshal_loader_symbol(mrb, loader), &path_len);
		struct RClass* cls = marshal_loader_path2cls(mrb, path, path_len);
		mrb_value object = marshal_loader_marshal(mrb, loader);
		mrb_basic_ptr(object)->c = cls;
		return object;
	}
	case 'u': {
		mrb_int path_len = 0;
		const char* path = mrb_sym2name_len(mrb, marshal_loader_symbol(mrb, loader), &path_len);
		struct RClass* cls = marshal_loader_path2cls(mrb, path, path_len);
		mrb_value object = mrb_funcall(mrb, mrb_obj_value(cls), "_load", 1, marshal_loader_string(mrb, loader));
		marshal_loader_register_link(mrb, loader, &object);
		return object;
	}
	case 'U': {
		mrb_int path_len = 0;
		const char* path = mrb_sym2name_len(mrb, marshal_loader_symbol(mrb, loader), &path_len);
		struct RClass* cls = marshal_loader_path2cls(mrb, path, path_len);
		enum mrb_vtype ttype = MRB_INSTANCE_TT(cls);
		if (ttype == 0) ttype = MRB_TT_OBJECT;
		mrb_value object = mrb_obj_value(mrb_obj_alloc(mrb, ttype, cls));
		mrb_funcall(mrb, object, "marshal_load", 1, marshal_loader_marshal(mrb, loader));
		marshal_loader_register_link(mrb, loader, &object);
		return object;
	}
	case 'd': {
		mrb_int path_len = 0;
		const char* path = mrb_sym2name_len(mrb, marshal_loader_symbol(mrb, loader), &path_len);
		struct RClass* cls = marshal_loader_path2cls(mrb, path, path_len);
		enum mrb_vtype ttype = MRB_INSTANCE_TT(cls);
		if (ttype == 0) ttype = MRB_TT_OBJECT;
		mrb_value object = mrb_obj_value(mrb_obj_alloc(mrb, ttype, cls));
		marshal_loader_register_link(mrb, loader, &object);
		mrb_funcall(mrb, object, "_load_data", 1, marshal_loader_marshal(mrb, loader));
		return object;
	}
	case 'e': {
		mrb_int path_len = 0;
		const char* path = mrb_sym2name_len(mrb, marshal_loader_symbol(mrb, loader), &path_len);
		struct RClass* mod = marshal_loader_path2cls(mrb, path, path_len);
		mrb_value object = marshal_loader_marshal(mrb, loader);
		mrb_funcall(mrb, object, "extend", 1, mrb_obj_value(mod));
		return object;
	}
	case ';': {
		mrb_int const id = marshal_loader_fixnum(mrb, loader);
		if ( id >= RARRAY_LEN(loader->linked_symbols) ) {
			char buffer[128];
			sprintf(buffer, "Invalid link id[%d] - Symbols size:%d", id, RARRAY_LEN(loader->linked_symbols));
			mrb_raise(mrb, E_INDEX_ERROR, buffer);
		}
		return RARRAY_PTR(loader->linked_symbols)[id];
	}
	case '@': {
		mrb_int const id = marshal_loader_fixnum(mrb, loader);
		if ( id >= RARRAY_LEN(loader->linked_objects) ) {
			char buffer[128];
			sprintf(buffer, "Invalid link id[%d] - Objects size:%d", id, RARRAY_LEN(loader->linked_objects));
			mrb_raise(mrb, E_INDEX_ERROR, buffer);
		}
		return RARRAY_PTR(loader->linked_objects)[id];
	}
	}
	{
		char buffer[64];
		sprintf(buffer, "Unsupported tag \"%c\"", type);
		mrb_raise(mrb, E_NOTIMP_ERROR, buffer);
	}
	return mrb_nil_value();
}

static mrb_value marshal_load(mrb_state* mrb, mrb_value self) {
	mrb_int argc = 0;
	mrb_value* args = NULL;
	if ( mrb_get_args(mrb, "*!", &args, &argc) && argc > 0 ) {
		struct marshal_loader loader;
		int ret = marshal_loader_initialize(mrb, args[0], &loader);
		switch ( ret ) {
		case MARSHAL_UNKNOWN:
			mrb_raise(mrb, E_TYPE_ERROR, "Marshal failed.\n");
		break;
		}
		{
			uint8_t const major_version = marshal_loader_byte(mrb, &loader);
			uint8_t const minor_version = marshal_loader_byte(mrb, &loader);
			if ( major_version != MAJOR_VERSION || minor_version != MINOR_VERSION ) {
				mrb_raisef(mrb, E_TYPE_ERROR, "invalid marshal version: %S.%S (expected: %S.%S)",
					mrb_fixnum_value(major_version), mrb_fixnum_value(minor_version),
					mrb_fixnum_value(MAJOR_VERSION), mrb_fixnum_value(MINOR_VERSION));
			}
		}
		return marshal_loader_marshal(mrb, &loader);
	}
	return mrb_nil_value();
}

// =======================================================
// Dumper
// =======================================================
struct marshal_dumper {
	mrb_value dest;
	unsigned dest_pos;
	unsigned (*dump_func)(mrb_state* mrb, struct marshal_dumper* dumper, unsigned size, void* source);
	mrb_value linked_objects;
	mrb_value linked_symbols;
};

static unsigned marshal_dumpfunc_for_string(mrb_state* mrb, struct marshal_dumper* dumper, unsigned size, void* source) {
	mrb_str_concat(mrb, dumper->dest, mrb_str_new(mrb, source, size));
	dumper->dest_pos += size;
	return size;
}

static unsigned marshal_dumpfunc_for_writer(mrb_state* mrb, struct marshal_dumper* dumper, unsigned size, void* source) {
	mrb_value write_len = mrb_funcall(mrb, dumper->dest, "write", 1, mrb_str_new(mrb, source, size));
	if ( mrb_fixnum_p(write_len) ) {
		dumper->dest_pos += mrb_fixnum(write_len);
		return mrb_fixnum(write_len);
	}
	return 0;
}

static void marshal_dumper_byte(mrb_state* mrb, struct marshal_dumper* dumper, char byte) {
	dumper->dump_func(mrb, dumper, 1, &byte);
}

static unsigned marshal_dumper_dump(mrb_state* mrb, struct marshal_dumper* dumper, unsigned size, void* source) {
	return dumper->dump_func(mrb, dumper, size, source);
}

static int marshal_dumper_initialize(mrb_state* mrb, mrb_value dest, struct marshal_dumper* dumper) {
	dumper->dest = dest;
	dumper->dest_pos = 0;
	if ( mrb_string_p(dest) ) {
		dumper->dump_func = marshal_dumpfunc_for_string;
	} else if ( mrb_respond_to(mrb, dest, mrb_intern_lit(mrb, "write")) ) {
		dumper->dump_func = marshal_dumpfunc_for_writer;
	} else {
		return MARSHAL_UNKNOWN;
	}
	dumper->linked_objects = mrb_ary_new(mrb);
	dumper->linked_symbols = mrb_ary_new(mrb);
	return MARSHAL_SUCCESS;
}

static void marshal_dumper_fixnum(mrb_state* mrb, struct marshal_dumper* dumper, mrb_int v) {
	if(v == 0) { 
		marshal_dumper_byte(mrb, dumper, 0);
		return; 
	} else if(0 < v && v < 123) {
		marshal_dumper_byte(mrb, dumper, v + 5);
		return; 
	} else if(-124 < v && v < 0) {
		marshal_dumper_byte(mrb, dumper, (v - 5) & 0xFF);
		return; 
	} else {
		char buf[sizeof(mrb_int) + 1];
		mrb_int x = v;
		size_t i = 1;
		for(; i <= sizeof(mrb_int); ++i) {
			buf[i] = x & 0xFF;
			x = x < 0 ? ~((~x) >> 8) : (x >> 8);
			if(x ==  0) { buf[0] =  i; break; }
			if(x == -1) { buf[0] = -i; break; }
		}
		marshal_dumper_dump(mrb, dumper, i + 1, buf);
	}
}

static void marshal_dumper_string(mrb_state* mrb, struct marshal_dumper* dumper, mrb_value* string) {
	if ( mrb_string_p(*string) ) {
		marshal_dumper_fixnum(mrb, dumper, RSTRING_LEN(*string));
		marshal_dumper_dump(mrb, dumper, RSTRING_LEN(*string), RSTRING_PTR(*string));
	} else {
		mrb_raise(mrb, E_TYPE_ERROR, "marshal requesting string value");
	}
}

static void marshal_dumper_symbol(mrb_state* mrb, struct marshal_dumper* dumper, mrb_sym symbol) {
	size_t len = RARRAY_LEN(dumper->linked_symbols);
	mrb_value* begin = RARRAY_PTR(dumper->linked_symbols);
	mrb_value* ptr = begin;
	for (; ptr < (begin + len) && (mrb_symbol(*ptr) != symbol); ++ptr);

	if(ptr == begin + len) {
		mrb_ary_push(mrb, dumper->linked_symbols, mrb_symbol_value(symbol));
		marshal_dumper_byte(mrb, dumper, ':');
		mrb_value string = mrb_sym2str(mrb, symbol);
		marshal_dumper_string(mrb, dumper, &string);
	} else { 
		marshal_dumper_byte(mrb, dumper, ';');
		marshal_dumper_fixnum(mrb, dumper, ptr - begin);
	}
}

static void marshal_dumper_class_symbol(mrb_state* mrb, struct marshal_dumper* dumper, struct RClass* cls) {
	marshal_dumper_symbol(mrb, dumper, mrb_intern_str(mrb, mrb_class_path(mrb, cls)));
}

static void marshal_dumper_extended(mrb_state* mrb, struct marshal_dumper* dumper, mrb_value* obj, int check) {
	if ( check ) {}
	struct RClass* cls = mrb_class(mrb, *obj);
	while(cls->tt == MRB_TT_ICLASS) {
		marshal_dumper_byte(mrb, dumper, 'e');
		marshal_dumper_symbol(mrb, dumper, mrb_intern_cstr(mrb, mrb_class_name(mrb, cls->c)));
    }
}

static void marshal_dumper_uclass(mrb_state* mrb, struct marshal_dumper* dumper, mrb_value* obj, struct RClass* super) {
	marshal_dumper_extended(mrb, dumper, obj, 1);
	struct RClass* real_class = mrb_class_real(mrb_class(mrb, *obj));
    if(real_class != super) {
		marshal_dumper_byte(mrb, dumper, 'C');
		marshal_dumper_class_symbol(mrb, dumper, real_class);
	}
}

static void marshal_dumper_class(mrb_state* mrb, struct marshal_dumper* dumper, mrb_value* obj, char tag, int check) {
	marshal_dumper_extended(mrb, dumper, obj, check);
	marshal_dumper_byte(mrb, dumper, tag);
	marshal_dumper_class_symbol(mrb, dumper, mrb_class_real(mrb_class(mrb, *obj)));
}

static struct RClass* regexp_class = NULL;

static void marshal_dumper_marshal(mrb_state* mrb, struct marshal_dumper* dumper, mrb_value* obj, mrb_int limit) {
	if (limit <= 0) mrb_raise(mrb, E_RUNTIME_ERROR, "exceed depth limit");
	limit -= 1;
	// Nil
	if ( mrb_nil_p(*obj) ) {
		marshal_dumper_byte(mrb, dumper, '0');
		return;
	}
	// Basic type
	switch( mrb_type(*obj) ) {
	case MRB_TT_FALSE:
		marshal_dumper_byte(mrb, dumper, 'F');
		return;
	case MRB_TT_TRUE:
		marshal_dumper_byte(mrb, dumper, 'T');
		return;
	case MRB_TT_FIXNUM: 
		marshal_dumper_byte(mrb, dumper, 'i');
		marshal_dumper_fixnum(mrb, dumper, mrb_fixnum(*obj));
		return;
	case MRB_TT_SYMBOL:
		marshal_dumper_symbol(mrb, dumper, mrb_symbol(*obj));
		return;
	default: break;
	}
	// Checking linked object
	{
		mrb_value *b = RARRAY_PTR(dumper->linked_objects);
		mrb_value *l = b;
		mrb_value *e = b + RARRAY_LEN(dumper->linked_objects);
		for (; (l < e) && (!mrb_obj_eq(mrb, *l, *obj)); ++l);
		if (l != e) {
			marshal_dumper_byte(mrb, dumper, '@');
			marshal_dumper_fixnum(mrb, dumper, l - b);
			return;
		}
	}
	// Link object
	mrb_ary_push(mrb, dumper->linked_objects, *obj);
	// User marshal
	if(mrb_respond_to(mrb, *obj, mrb_intern_lit(mrb, "marshal_dump"))) {
		marshal_dumper_class(mrb, dumper, obj, 'U', 0);
		mrb_value object = mrb_funcall(mrb, *obj, "marshal_dump", 0);
		marshal_dumper_marshal(mrb, dumper, &object, limit);
		return;
	}
	// User defined
	if(mrb_respond_to(mrb, *obj, mrb_intern_lit(mrb, "_dump"))) {
		marshal_dumper_class(mrb, dumper, obj, 'u', 0);
		mrb_value data = mrb_funcall(mrb, *obj, "_dump", 1, mrb_fixnum_value(limit));
		marshal_dumper_string(mrb, dumper, &data);
		return;
	}
	// Class
	struct RClass* cls = mrb_obj_class(mrb, *obj);
	// Normal object
	mrb_value iv_keys = mrb_obj_instance_variables(mrb, *obj);
	if( mrb_type(*obj) != MRB_TT_OBJECT && cls != regexp_class && RARRAY_LEN(iv_keys) > 0 ) marshal_dumper_byte(mrb, dumper, 'I');
	if ( regexp_class == cls ) {
		marshal_dumper_uclass(mrb, dumper, obj, regexp_class);
		marshal_dumper_byte(mrb, dumper, '/');
		mrb_value string = mrb_funcall(mrb, *obj, "source", 0);
		marshal_dumper_string(mrb, dumper, &string);
		mrb_value options = mrb_funcall(mrb, *obj, "options", 0);
		marshal_dumper_fixnum(mrb, dumper, mrb_fixnum(options));
		return;
	} else if ( mrb_class_defined(mrb, "Struct") && mrb_obj_is_kind_of(mrb, *obj, mrb_class_get(mrb, "Struct")) ) {
		mrb_value members = mrb_iv_get(mrb, mrb_obj_value(mrb_class(mrb, *obj)), mrb_intern_lit(mrb, "__members__"));
		marshal_dumper_class(mrb, dumper, obj, 'S', 1);
		marshal_dumper_fixnum(mrb, dumper, RARRAY_LEN(members));
		mrb_int i;
		for (i = 0; i < RARRAY_LEN(members); i++) {
			mrb_check_type(mrb, RARRAY_PTR(members)[i], MRB_TT_SYMBOL);
			marshal_dumper_symbol(mrb, dumper, mrb_symbol(RARRAY_PTR(members)[i]));
			marshal_dumper_marshal(mrb, dumper, RARRAY_PTR(*obj) + i, limit);
		}
	} else switch ( mrb_type(*obj) ) {
		case MRB_TT_OBJECT: {
			marshal_dumper_class(mrb, dumper, obj, 'o', 1);
			marshal_dumper_fixnum(mrb, dumper, RARRAY_LEN(iv_keys));
			mrb_int i;
			for (i = 0; i < RARRAY_LEN(iv_keys); i++) {
				mrb_sym symbol = mrb_symbol(RARRAY_PTR(iv_keys)[i]);
				mrb_value value = mrb_iv_get(mrb, *obj, mrb_symbol(RARRAY_PTR(iv_keys)[i]));
				marshal_dumper_symbol(mrb, dumper, symbol);
				marshal_dumper_marshal(mrb, dumper, &value, limit);
			}
		} return;
		case MRB_TT_CLASS: {
			mrb_value path = mrb_class_path(mrb, mrb_class_ptr(*obj));
			marshal_dumper_byte(mrb, dumper, 'c');
			marshal_dumper_string(mrb, dumper, &path);
		} return;
		case MRB_TT_MODULE: {
			mrb_value path = mrb_class_path(mrb, mrb_class_ptr(*obj));
			marshal_dumper_byte(mrb, dumper, 'm');
			marshal_dumper_string(mrb, dumper, &path);
		} return;
		case MRB_TT_STRING: {
			marshal_dumper_uclass(mrb, dumper, obj, mrb->string_class);
			marshal_dumper_byte(mrb, dumper, '"');
			marshal_dumper_string(mrb, dumper, obj);
		} break;
		case MRB_TT_FLOAT: {
			char buf[256];
			sprintf(buf, "%.16f", mrb_float(*obj));
			marshal_dumper_byte(mrb, dumper, 'f');
			marshal_dumper_fixnum(mrb, dumper, strlen(buf));
			marshal_dumper_dump(mrb, dumper, strlen(buf), buf);
		} break;
		case MRB_TT_ARRAY: {
			marshal_dumper_uclass(mrb, dumper, obj, mrb->array_class);
			marshal_dumper_byte(mrb, dumper, '[');
			marshal_dumper_fixnum(mrb, dumper, RARRAY_LEN(*obj));
			mrb_int i = 0;
			for(; i < RARRAY_LEN(*obj); i++) { marshal_dumper_marshal(mrb, dumper, RARRAY_PTR(*obj) + i, limit); }
		} break;
		case MRB_TT_HASH: {
			marshal_dumper_uclass(mrb, dumper, obj, mrb->hash_class);
			marshal_dumper_byte(mrb, dumper, '{');
			mrb_value keys = mrb_hash_keys(mrb, *obj), key;
			marshal_dumper_fixnum(mrb, dumper, RARRAY_LEN(keys));
			mrb_int i = 0;
			for (; i < RARRAY_LEN(keys); i++) {
				key = mrb_ary_entry(keys, i);
				mrb_value value = mrb_hash_get(mrb, *obj, key);
				marshal_dumper_marshal(mrb, dumper, &key, limit);
				marshal_dumper_marshal(mrb, dumper, &value, limit);
			}
		} break;
		default : {
			char buffer[256];
			sprintf(buffer, "unsupported marshal class %s", mrb_obj_classname(mrb, *obj));
			mrb_raise(mrb, E_TYPE_ERROR, buffer);
		}
	}
	if(RARRAY_LEN(iv_keys) > 0) {
		marshal_dumper_fixnum(mrb, dumper, RARRAY_LEN(iv_keys));
		struct RObject* obj_ptr = mrb_obj_ptr(*obj);
		mrb_int i = 0;
		for(; i < RARRAY_LEN(iv_keys); i++) {
			mrb_sym key = mrb_symbol(RARRAY_PTR(iv_keys)[i]);
			mrb_value value = mrb_obj_iv_get(mrb, obj_ptr, key);
			marshal_dumper_symbol(mrb, dumper, key);
			marshal_dumper_marshal(mrb, dumper, &value, limit);
		}
	}
}

static mrb_value marshal_dump(mrb_state* mrb, mrb_value self) {
	mrb_int argc = 0;
	mrb_value* args = NULL;
	static const char marshal_version[2] = { MAJOR_VERSION, MINOR_VERSION };
	if ( mrb_get_args(mrb, "*!", &args, &argc) && argc > 0 ) {
		mrb_value obj = args[0];
		mrb_value port = (argc >= 2 ? args[1] : mrb_nil_value());
		mrb_int limit = (argc >= 3 ? mrb_fixnum(mrb_convert_to_integer(mrb, args[2], 10)) : 0xFF);
		struct marshal_dumper dumper;
		regexp_class = mrb_class_get(mrb, "Regexp");
		if ( !mrb_string_p(port) && mrb_respond_to(mrb, port, mrb_intern_lit(mrb, "write")) ) {
			marshal_dumper_initialize(mrb, port, &dumper);
			marshal_dumper_dump(mrb, &dumper, 2, marshal_version);
			marshal_dumper_marshal(mrb, &dumper, &obj, limit);
			return mrb_nil_value();
		} else {
			mrb_value buf = (mrb_string_p(port) ? port : mrb_str_new(mrb, NULL, 0));
			marshal_dumper_initialize(mrb, buf, &dumper);
			marshal_dumper_dump(mrb, &dumper, 2, marshal_version);
			marshal_dumper_marshal(mrb, &dumper, &obj, limit);
			return buf;
		}
	}
	return mrb_nil_value();
}

void mrb_mruby_marshal_c_gem_init(mrb_state* mrb) {
	struct RClass* marshal_module = mrb_define_module(mrb, "Marshal");

	mrb_define_module_function(mrb, marshal_module, "load",		marshal_load,	MRB_ARGS_ANY());
	mrb_define_module_function(mrb, marshal_module, "restore",	marshal_load,	MRB_ARGS_ANY());
	mrb_define_module_function(mrb, marshal_module, "dump",		marshal_dump,	MRB_ARGS_ANY());

	mrb_define_const(mrb, marshal_module, "MAJOR_VERSION", mrb_fixnum_value(MAJOR_VERSION));
	mrb_define_const(mrb, marshal_module, "MINOR_VERSION", mrb_fixnum_value(MINOR_VERSION));
}

void mrb_mruby_marshal_c_gem_final(mrb_state* mrb) {}
