# AMBuildScript.py - 定义构建逻辑

CXX = builder.DetectCompilers()

program = builder.AddCommandLineProgram('momsurffix_ext')

program.compiler.includes += [
    builder.sourcePath + '/src',
    # 通过 Actions 设置的 SDK 路径将在运行时注入
]

program.compiler.cxxincludes += [
    builder.sourcePath + '/src',
]

program.sources += [
    'src/momsurffix_ext.cpp',
    'src/extension.h',  # 如果有其他头文件，添加此处
    'src/smsdk_config.h',
]

builder.Build(program)
