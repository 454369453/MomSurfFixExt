import ambuild2

builder = ambuild2.Builder('momsurffix_ext')
builder.options.cxx = 'clang++'
builder.options.arch = 'x86'
builder.options.sdks = ['csgo']  # 或 'css' 根据您的游戏
builder.options.mms_path = 'mmsource-1.12'  # Metamod 版本
builder.options.sm_path = 'sourcemod-1.12'  # SourceMod 版本
builder.AddSource('src/momsurffix_ext.cpp')
builder.AddSource('src/extension.h')  # 添加头文件
builder.Build()
