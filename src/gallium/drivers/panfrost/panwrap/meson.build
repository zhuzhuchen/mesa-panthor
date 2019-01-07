srcs = [
    'panwrap-syscall.c',
    'panwrap-util.c',
    'panwrap-mmap.c',
    'panwrap-decoder.c',
    '../pan_pretty_print.c'
]

shared_library(
    'panwrap',
    srcs,
    include_directories: [inc_common, inc_include, inc_src, include_directories('../include')],
    dependencies: [
        cc.find_library('m', require: true),
        cc.find_library('dl', require: true),
	dependency('threads')
    ],
    install: true,
)
