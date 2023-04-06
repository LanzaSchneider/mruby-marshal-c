class UserMarshal
  attr_accessor :data

  def initialize
    @data = 'stuff'
  end
  def marshal_dump() :data end
  def marshal_load(data) @data = data end
  def ==(other) self.class === other and @data == other.data end
end

class UserDefined
  class Nested
    def ==(other)
      other.kind_of? self.class
    end
  end

  attr_reader :a, :b

  def initialize
    @a = 'stuff'
    @b = @a
  end

  def _dump(depth)
    Marshal.dump [:stuff, :stuff]
  end

  def self._load(data)
    a, b = Marshal.load data

    obj = allocate
    obj.instance_variable_set :@a, a
    obj.instance_variable_set :@b, b

    obj
  end

  def ==(other)
    self.class === other and
    @a == other.a and
    @b == other.b
  end
end

Regexp = Class.new unless Object.const_defined? :Regexp

assert('Marshal#dump') do
  assert_equal Marshal.dump(nil), "\004\b0"
  assert_equal Marshal.dump(true), "\004\bT"
  assert_equal Marshal.dump(false), "\004\bF"

  # with a Fixnum
  [ [Marshal,  0,       "\004\bi\000"],
    [Marshal,  5,       "\004\bi\n"],
    [Marshal,  8,       "\004\bi\r"],
    [Marshal,  122,     "\004\bi\177"],
    [Marshal,  123,     "\004\bi\001{"],
    [Marshal,  1234,    "\004\bi\002\322\004"],
    [Marshal, -8,       "\004\bi\363"],
    [Marshal, -123,     "\004\bi\200"],
    [Marshal, -124,     "\004\bi\377\204"],
    [Marshal, -1234,    "\004\bi\376.\373"],
    [Marshal, -4516727, "\004\bi\375\211\024\273"],
    [Marshal,  2**8,    "\004\bi\002\000\001"],
    [Marshal,  2**16,   "\004\bi\003\000\000\001"],
    [Marshal,  2**24,   "\004\bi\004\000\000\000\001"],
    [Marshal, -2**8,    "\004\bi\377\000"],
    [Marshal, -2**16,   "\004\bi\376\000\000"],
    [Marshal, -2**24,   "\004\bi\375\000\000\000"],
  ].each do |test_case|
    target, argv, result = *test_case
    assert_equal target.dump(argv), result
  end

  # with a symbol
  assert_equal Marshal.dump(:symbol), "\004\b:\vsymbol"
  assert_equal Marshal.dump(('big' * 100).to_sym), "\004\b:\002,\001#{'big' * 100}"

  # with an object responding to #marshal_dump
  assert_equal Marshal.dump(UserMarshal.new), "\x04\bU:\x10UserMarshal:\tdata"

  # with an object responding to #_dump
  assert_equal Marshal.dump(UserDefined.new), "\004\bu:\020UserDefined\022\004\b[\a:\nstuff;\000"

  # with a Float
  [ [Marshal,  0.0,             "\004\bf\0060"],
    [Marshal, -0.0,             "\004\bf\a-0"],
    [Marshal,  1.0,             "\004\bf\0061"],
    [Marshal,  123.4567,        "\004\bf\r123.4567"],
    [Marshal, -0.841,           "\x04\bf\v-0.841"],
    [Marshal, -9876.345,        "\x04\bf\x0E-9876.345"],
    [Marshal,  Float::INFINITY, "\004\bf\binf"],
    [Marshal, -Float::INFINITY, "\004\bf\t-inf"],
    [Marshal,  Float::NAN,      "\004\bf\bnan"],
  ].each do |test_case|
    target, argv, result = *test_case
    assert_equal target.dump(argv), result
  end

  # with an Array
  assert_equal Marshal.dump([]), "\004\b[\000"
  assert_equal Marshal.dump([:a, 1, 2]), "\004\b[\b:\006ai\006i\a"
  a = []
  a << a
  assert_equal Marshal.dump(a), "\x04\b[\x06@\x00"

  # with a Hash
  assert_equal Marshal.dump({}), "\004\b{\000"
end