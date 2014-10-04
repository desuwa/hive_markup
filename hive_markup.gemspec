Gem::Specification.new do |s|
  s.name = 'hive_markup'
  s.version = '0.0.1'
  s.summary = 'Markup parser for HiveBBS'
  s.author = 'Maxime Youdine'
  s.license = 'MIT'
  s.homepage = 'https://github.com/desuwa/hive_markup'
  s.required_ruby_version = '>= 1.9.3'
  s.files = %w[
    hive_markup.gemspec
    lib/hive_markup/hive_markup.rb
    ext/hive_markup/extconf.rb
    ext/hive_markup/houdini.h
    ext/hive_markup/hive_markup.c
  ]
  s.extensions = ['ext/hive_markup/extconf.rb']
  s.require_paths = ['lib']
end
