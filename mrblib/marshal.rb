module Marshal
	class << self
		# def dump(obj, port = nil, limit = nil)
			# if port.is_a?(::IO)
				# Dumper.new(port).dump(obj, limit)
				# return
			# else
				# buf = (port.is_a?(::String) ? port : '')
				# Dumper.new(buf).dump(obj, limit)
				# return buf
			# end
		# end
		# def load(*args)
			# port = args[0]
			# result = Reader.new(port).read
			# return result
		# end
	end
end

class Marshal::Dumper
	def initialize(dest)
		@dest = dest
		if @dest.is_a?(::String)
			@dumper = Proc.new do |data|
				@dest.concat data
			end
		else
			@dumper = Proc.new do |data|
				@dest.write data
			end
		end
		append "\x04\b" # Version
		@link = []
		@link_symbol = []
	end
	
	def append(data)
		@dumper.call(data)
	end
	
	def dump(object, limit = nil)
		limit = 100 if limit.nil?
		raise RuntimeError, 'exceed depth limit' if limit == 0
		limit -= 1
		# Dump basic object
		case object
		when nil 
			append '0'
			return
		when true 
			append 'T'
			return
		when false
			append 'F'
			return
		when ::Fixnum
			append 'i'
			dump_fixnum object
			return
		when ::Symbol
			dump_symbol object
			return
		when ::Float
			@link << object
			append 'f'
			dump_float object
			return
		end
		# Linked object
		link_index = nil
		@link.each_index do |index|
			if @link[index].object_id == object.object_id
				link_index = index
				break
			end
		end
		if link_index
			append '@'
			dump_fixnum link_index
			return
		end
		# Dump object
		if object.respond_to?(:marshal_dump)
			@link << object
			dump_class 'U', object
			dump object.marshal_dump, limit
			return
		end
		if object.respond_to?(:_dump)
			dump_data = object._dump(limit)
			raise TypeError, '_dump() must return string' unless dump_data.is_a?(String)
			hasiv = !object.instance_variables.empty?
			append 'I' if hasiv
			dump_class 'u', object
			dump_bytes dump_data
			dump_iv(object, limit) if hasiv
			@link << object
			return
		end
		@link << object
		hasiv = !object.instance_variables.empty?
		hasencoding = nil
		case object
		when ::String, ::Regexp
			# hasencoding = (object.encoding.name == 'UTF-8') rescue nil
			# hasencoding = nil if !hasencoding
		when ::Object, ::Class, ::Module
			hasiv = false
		end
		append 'I' if hasiv || (hasencoding != nil)
		case object
		when ::Class
			append 'c'
			dump_bytes object.name
		when ::Module
			append 'm'
			dump_bytes object.name
		when ::Float
			append 'f'
			dump_float object
		when ::Bignum
			append 'l'
			dump_bignum object
		when ::String
			dump_uclass object, ::String
			append '"'
			dump_bytes object
		when ::Regexp
			dump_uclass object, ::Regexp
			append '/'
			dump_bytes object.source
			append object.options.chr
		when ::Array
			dump_uclass object, ::Array
			append '['
			dump_fixnum object.size
			object.each do |unit|
				dump unit, limit
			end
		when ::Hash
			dump_uclass object, ::Hash
			append '{'
			dump_fixnum object.size
			object.each do |k, v|
				dump k, limit
				dump v, limit
			end
		when ::Struct
			dump_class 'S', object
			dump_fixnum object.size
			object.members.each do |k|
				dump_symbol k
				dump object[k], limit
			end
		when ::Object
			dump_class 'o', object
			dump_iv(object, limit)
		else
			raise TypeError, "no _dump_data is defined for class #{Object.instance_method(:class).bind(object).call}"
		end
		dump_iv(object, limit, hasencoding) if hasiv || (hasencoding != nil)
	end
	
	def dump_extended(klass)
		
	end
	
	def dump_class(type, object)
		klass = Object.instance_method(:class).bind(object).call
		dump_extended klass
		append type
		dump_symbol klass.name.to_sym
	end
	
	def dump_uclass(object, supa)
		klass = Object.instance_method(:class).bind(object).call
		dump_extended klass
		if klass != supa
			append 'C'
			dump_symbol klass.name.to_sym
		end
	end
	
	def dump_fixnum(n)
		if n == 0
			append n.chr
		elsif n > 0 && n < 123
			append (n + 5).chr
		elsif n < 0 && n > -124
			append ((n - 5) & 0xFF).chr
		else
			
		end
	end
	
	def dump_symbol(symbol)
		link_index = @link_symbol.index(symbol)
		if link_index
			append ';'
			dump_fixnum link_index
		else
			@link_symbol << symbol
			append ':'
			dump_bytes symbol.to_s
		end
	end
	
	def dump_bytes(bytes)
		dump_fixnum bytes.bytesize
		append bytes
	end
	
	def dump_iv(object, limit, hasencoding = nil)
		size = object.instance_variables.size
		size += 1 if hasencoding != nil
		dump_fixnum size
		object.instance_variables.each do |symbol|
			dump_symbol symbol
			dump object.instance_variable_get(symbol), limit
		end
		if hasencoding != nil
			dump_symbol :E
			dump hasencoding, limit
		end
	end
	
	def dump_float(float)
		dump_bytes float.to_s
	end
	
	def dump_bignum(bignum)
		raise 'cannot dump bignum'
	end
	
end

class Marshal::Reader
	def initialize(data)
		@data = data
		@data_ptr = 0
		if @data.is_a?(::String)
			@reader = Proc.new do |size|
				result = ''
				size.times do
					result += @data.getbyte(@data_ptr).chr
					@data_ptr += 1
				end
				result
			end
		else
			@data_ptr = @data.pos
			@reader = Proc.new do |size|
				result = @data.read(size)
				@data_ptr += result.length
				result
			end
		end
		@major_version = read_byte
		@minor_version = read_byte
		@objects = []
		@symbols = []
	end
	
	def register(&blk)
		object = blk.call
		@objects << object
		object
	end
	
	def marshal_const_get(name)
		begin
			eval name.to_s
		rescue NameError
			raise ArgumentError, "undefined class/module #{name}"
		end
    end
	
	def read
		char = read_char
		case char
		when '0' then nil
		when 'T' then true
		when 'F' then false
		when 'i' then read_integer
		when 'l' then read_bignum
		when ':' then read_symbol
		when '"' then read_string
		when 'I' then read_with_iv
		when '[' then read_array
		when '{' then read_hash
		when 'f' then read_float
		when 'c' then read_class
		when 'm' then read_module
		when 'S' then read_struct
		when '/' then read_regexp
		when 'o' then read_object
		when 'C' then read_userclass
		when 'u' then read_userdefined
		when 'U' then read_usermarshal
		when 'd' then read_data
		when 'e' then read_extended_object
		when ';' then read_symbol_link
		when '@' then read_object_link
		else
			raise NotImplementedError, "Unsupported tag #{char}[0x#{char.ord.to_s(16)}] on [#{@data_ptr - 1}]."
		end
	end
		
	def read_byte
		read_char.ord
	end
	
	def read_char
		@reader.call 1
	end
	
	def read_integer
      c = (read_byte ^ 128) - 128
      case c
      when 0
        0
      when (5..127)
        c - 5
      when (1..4)
        c
          .times
          .map { |i| [i, read_byte] }
          .inject(0) { |result, (i, byte)| result | (byte << (8 * i)) }
      when (-128..-6)
        c + 5
      when (-5..-1)
        (-c)
          .times
          .map { |i| [i, read_byte] }
          .inject(-1) do |result, (i, byte)|
          a = ~(0xff << (8 * i))
          b = byte << (8 * i)
          (result & a) | b
        end
      end
    end
	
	def read_bignum
		register do
			sign = read_char.eql?('+') ? 1 : -1
			length = read_integer * 2
			data = @reader.call(length)
			bignum = 0
			for i in 0...length
				bignum += (data.getbyte(i) * 2 ** (i * 8))
			end
			bignum *= sign
			bignum
		end
	end
	
	def read_symbol
		symbol = read_integer.times.map { read_char }.join.to_sym
		@symbols << symbol
		symbol
    end
	
	def read_string(cache = true)
		len = read_integer
		string = @reader.call(len)
		@objects << string if cache
		string
    end
	
	def read_with_iv
		obj = read
		iv_len = read_integer
		iv_len.times do
			symbol = read
			value = read
			if symbol == :E #encoding
				# obj.force_encoding('UTF-8') rescue nil
				next
			end
			obj.instance_variable_set(symbol, value) 
		end
		obj
	end
	
	def read_array
		len = read_integer
		array = []
		@objects << array
		len.times { array << read }
		array
    end
	
	def read_hash(cache = true)
		hash = {}
		@objects << hash if cache
		read_integer.times do
			k, v = read, read
			hash[k] = v
		end
		hash
    end
		
	def read_float
		float = read_string(false).to_f
		@objects << float
		float
    end

	def read_class
		register do
			const_name = read_string(false)
			klass = marshal_const_get(const_name)
			unless klass.instance_of?(Class)
				raise ArgumentError, "#{const_name} does not refer to a Class"
			end
			klass
		end
    end
	
	def read_module
		register do
			const_name = read_string(false)
			klass = marshal_const_get(const_name)
			unless klass.instance_of?(Module)
				raise ArgumentError, "#{const_name} does not refer to a Module"
			end
			klass
		end
    end
	
	def read_struct
		register do
			klass = marshal_const_get(read)
			attributes = read_hash(false)
			values = attributes.values_at(*klass.members)
			klass.new(*values)
		end
	end
	
	def read_regexp
		register do
			regexp = Regexp.new(read_string(false), read_byte)
			regexp
		end
	end
	
	def read_object
		klass = marshal_const_get(read)
        object = klass.allocate
		@objects << object
		read_integer.times do
			ivar_name, value = read, read
			object.instance_variable_set(ivar_name, value)
		end
        object
    end
	
	def read_userclass
		klass = marshal_const_get(read)
        object = read
		#object.class = klass
        object
	end
		
	def read_userdefined
		register do
			klass = marshal_const_get(read)
			data = read_string(false)
			klass._load(data)
		end
	end
	
	def read_usermarshal
		klass = marshal_const_get(read)
		object = klass.allocate
		object.marshal_load(read)
		@objects << object
		object
	end
	
	def read_data
		klass = marshal_const_get(read)
		obj = klass.allocate
		@objects << obj
		obj._load_data(read)
		obj
	end
	
	def read_extended_object
		mod = marshal_const_get(read)
		object = read
		object.extend(mod)
		object
	end
	
    def read_symbol_link
		index = read_integer
		raise RuntimeError, "Invalid link id[#{index}] - Symbols size:#{@symbols.size}" if index < 0 || index >= @symbols.size
		@symbols[index]
    end

    def read_object_link
		index = read_integer
		raise RuntimeError, "Invalid link id[#{index}] - Objects size:#{@objects.size}" if index < 0 || index >= @objects.size
		@objects[index]
    end
end
