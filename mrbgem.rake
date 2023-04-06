MRuby::Gem::Specification.new('mruby-marshal-c') do |spec|
	spec.license = 'Public domain'
	spec.authors = 'Lanza Schneider'
	spec.summary = 'Marshal module for mruby written in C-language with full object-link & symbol link support!'
	spec.add_test_dependency('mruby-struct', :core => 'mruby-struct')
end
