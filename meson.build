project('dpdk-perf', 'cpp',
  version: '0.1.0',
  default_options: ['warning_level=2', 'cpp_std=c++23', 'optimization=3', 'debug=true'])

cmake = import('cmake')
dpdk_dep = dependency('libdpdk', method: 'pkg-config', required: true, static: true, version: '25.11.0')
tlx_proj = cmake.subproject('tlx')
tlx_dep = tlx_proj.dependency('tlx')
hdr_hist = cmake.subproject('hdr_histogram')
hdr_histogram_dep = hdr_hist.dependency('hdr_histogram')

add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')
add_project_arguments('-Wpedantic', language: 'c')
add_project_arguments('-fno-omit-frame-pointer', language: 'cpp')

sources = files('src/client.cc', 'src/connection.cc', 'src/iface.cc',  'src/util.cc', 'src/log.cc',  'src/task.cc')

fastt_lib = static_library('fastt', sources, include_directories: include_directories('include'), 
  dependencies: [dpdk_dep, hdr_histogram_dep], link_args: ['-lcap', '-Wl,--allow-multiple-definition', '-Wl,--whole-archive'])


executable('server', 'server_main.cc', include_directories: include_directories('include'), dependencies: [dpdk_dep, tlx_dep, hdr_histogram_dep], link_with: fastt_lib, link_args: ['-lcap'])
executable('uring', 'uring_peer.cc', include_directories: include_directories('include'), dependencies: [tlx_dep, dpdk_dep, hdr_histogram_dep], link_args: ['-luring', '-lcap'])

# testing
gtest_dep = dependency('gtest', required: true)
gtest_main_dep = dependency('gtest_main', required: true)
test_inc = include_directories('include', 'tests')

test_srcs = files('tests/test.cc',
  'tests/transport_input_test.cc',
  'tests/transport_output_test.cc'
  )
unit_tests = executable('unit_tests',
  test_srcs,
  include_directories: test_inc,
  dependencies: [dpdk_dep, gtest_dep, gtest_main_dep, hdr_histogram_dep],
  link_with: fastt_lib,
  link_args: ['-lcap'])
test('unit_tests', unit_tests, protocol: 'gtest')
