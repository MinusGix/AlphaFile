project(
    'AlphaFile',
    'c', 'cpp',
    default_options : ['cpp_std=c++17', 'c_std=c11', 'buildtype=debugoptimized'],
    license : 'MIT',
    version : '1.0.0'
)

test_executable = executable('tests', sources : ['src/test.cpp'])
test('tests', test_executable)

libalphafile_dep = declare_dependency(
    include_directories : include_directories('include')
)