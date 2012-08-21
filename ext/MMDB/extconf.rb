require 'mkmf'
$LDFLAGS = CONFIG['LDFLAGS'] = CONFIG['LDFLAGS'].gsub('--no-undefined', '--allow-shlib-undefined')

create_makefile('RecordModelMMDBExt') 
