#define MARSHAL_MAJOR 4
#define MARSHAL_MINOR 8

#define TYPE_NIL '0'
#define TYPE_TRUE 'T'
#define TYPE_FALSE 'F'
#define TYPE_FIXNUM 'i'

#define TYPE_EXTENDED 'e'
#define TYPE_UCLASS 'C'
#define TYPE_OBJECT 'o'
#define TYPE_DATA 'd'
#define TYPE_USERDEF 'u'
#define TYPE_USRMARSHAL 'U'
#define TYPE_FLOAT 'f'
#define TYPE_BIGNUM 'l'
#define TYPE_STRING '"'
#define TYPE_REGEXP '/'
#define TYPE_ARRAY '['
#define TYPE_HASH '{'
#define TYPE_HASH_DEF '}'
#define TYPE_STRUCT 'S'
#define TYPE_MODULE_OLD 'M'
#define TYPE_CLASS 'c'
#define TYPE_MODULE 'm'

#define TYPE_SYMBOL ':'
#define TYPE_SYMLINK ';'

#define TYPE_IVAR 'I'
#define TYPE_LINK '@'

#define s_dump MRB_SYM(_dump)
#define s_load MRB_SYM(_load)
#define s_mdump MRB_SYM(marshal_dump)
#define s_mload MRB_SYM(marshal_load)
#define s_dump_data MRB_SYM(_dump_data)
#define s_load_data MRB_SYM(_load_data)
#define s_alloc MRB_SYM(_alloc)
#define s_call MRB_SYM(call)
#define s_getbyte MRB_SYM(getbyte)
#define s_read MRB_SYM(read)
#define s_write MRB_SYM(write)
#define s_binmode MRB_SYM(binmode)

#define RSHIFT(x, y) ((x) >> (int)y)
#define FLOAT_DIG 17
#define DECIMAL_MANT (53 - 16) /* from IEEE754 double precision */
#define SIZEOF_LONG 4
