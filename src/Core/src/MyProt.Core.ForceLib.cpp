// src/Core/src/MyProt.Core.ForceLib.cpp
// 占位 .cpp — Core 是 header-only, 但 vcxproj 配为 StaticLibrary
// MSBuild 不会为空 StaticLibrary 产 .lib, 下游 link 会报 LNK1104
// 此文件提供一个空翻译单元, 让 cl 产 .obj + lib 工具产 .lib
//
// 不可删: 删了就断下游 link

namespace MyProt { namespace Core {
// 故意为空; 仅作为编译单元锚点
}}
