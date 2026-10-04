project('billiards', 'cpp', version: '0.1.0', meson_version: '>=1.2.0')

krisp = dependency('krisp', required: true)
krisp_runtime_directory = get_option('krisp_runtime_directory')
python = find_program('python3', required: true)

runtime_deploy = custom_target(
  'deploy-krisp-runtime',
  output: 'krisp-runtime.stamp',
  command: [
    python,
    files('scripts/deploy_runtime.py'),
    krisp_runtime_directory,
    meson.current_build_dir(),
    '@OUTPUT@',
  ],
  build_by_default: true,
  build_always_stale: true,
)

executable(
  'billiards',
  'billiards.cpp',
  'billiards_ui.cpp',
  dependencies: krisp,
  link_depends: runtime_deploy[0],
  install: false,
)
