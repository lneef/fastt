project('dpdk-perf', 'cpp',
  version: '0.1.0',
  default_options: ['warning_level=2', 'cpp_std=c++23', 'buildtype=debug'])

dpdk_dep = dependency('libdpdk', method: 'pkg-config', required: true, static: true)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')
add_project_arguments('-Wpedantic', language: 'c')

sources = files('src/client.cc', 'src/connection.cc', 'src/iface.cc', 'src/message.cc', 'src/packet_scheduler.cc', 'src/protocol.cc', 'src/util.cc', 'src/log.cc', 'src/kv.cc')

fastt_lib = static_library('fastt', sources, include_directories: include_directories('include'), 
  dependencies: [dpdk_dep], link_args: ['-lcap', '-Wl,--allow-multiple-definition', '-Wl,--whole-archive'])


executable('client', 'client_main.cc', include_directories: include_directories('include'), dependencies: [dpdk_dep], link_with: fastt_lib, link_args: ['-lcap'])
executable('server', 'server_main.cc', include_directories: include_directories('include'), dependencies: [dpdk_dep], link_with: fastt_lib, link_args: ['-lcap'])
