class UserMarshalWithIvar
  attr_reader :data

  def initialize
    @data = 'my data'
  end

  def marshal_dump
    [:data]
  end

  def marshal_load(o)
    @data = o.first
  end

  def ==(other)
    self.class === other and
    @data = other.data
  end
end

class UserMarshal
  attr_accessor :data

  def initialize
    @data = 'stuff'
  end
  def marshal_dump() :data end
  def marshal_load(data) @data = data end
  def ==(other) self.class === other and @data == other.data end
end

Regexp = Class.new unless Object.const_defined? :Regexp
Struct::Pyramid = Struct.new

assert('Marshal.load') do
  assert_equal Marshal.load("\x04\b0"), nil
  assert_equal Marshal.load("\x04\bT"), true
  assert_equal Marshal.load("\x04\bF"), false
end

# assert('Marshal.load for an array containing the same objects') do
#   s = 'oh'
#   b = 'hi'
#   r = Regexp.new
#   d = [b, :no, s, :go]
#   c = String
#   f = 1.0

#   o1 = UserMarshalWithIvar.new; o2 = UserMarshal.new

#   obj = [:so, 'hello', 100, :so, :so, d, :so, o2, :so, :no, o2,
#           :go, c, nil, Struct::Pyramid.new, f, :go, :no, s, b, r,
#           :so, 'huh', o1, true, b, b, 99, r, b, s, :so, f, c, :no, o1, d]

#   assert_equal Marshal.load("\004\b[*:\aso\"\nhelloii;\000;\000[\t\"\ahi:\ano\"\aoh:\ago;\000U:\020UserMarshal\"\nstuff;\000;\006@\n;\ac\vString0S:\024Struct::Pyramid\000f\0061;\a;\006@\t@\b/\000\000;\000\"\bhuhU:\030UserMarshalWithIvar[\006\"\fmy dataT@\b@\bih@\017@\b@\t;\000@\016@\f;\006@\021@\a"), obj
# end

assert('Marshal.load for a Symbol') do
  sym = Marshal.load("\004\b:\vsymbol")
  assert_equal sym, :symbol
end

assert('Marshal.load for an Object') do
  assert_equal Marshal.load("\004\bo:\vObject\000").class, Object
end

assert('Marshal.load for a Float') do
  assert_equal Marshal.load("\004\bf\bnan").to_s, (0.0 / 0.0).to_s
  assert_equal Marshal.load("\004\bf\v1.3\000\314\315"), 1.3
  assert_equal Marshal.load("\004\bf\0361.1867344999999999e+22\000\344@"), 1.1867345e+22
end

assert('Marshal.load for an Integer') do
  assert_equal Marshal.load("\004\bi\000"), 0
  assert_equal Marshal.load("\004\bi\005"), 0
  assert_equal Marshal.load("\004\bi\r"), 8
  assert_equal Marshal.load("\004\bi\363"), -8
  assert_equal Marshal.load("\004\bi\376.\373"), -1234
end
