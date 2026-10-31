#pragma once

// Baker's types, declared and not defined, for a header that only names them: a pointer, a
// reference, a parameter, a return type. Each of Baker's other headers costs a translation
// unit about half a second to compile, most of it <string_view> and <span>; this one
// includes only <cstdint>. Include the defining header where the type is used.
//
//   #include <baker/fwd.hpp>
//   class Spawner {
//   public:
//       explicit Spawner(const baker::Config& config);
//       void add_functions(baker::Context& ctx);
//   };

#include <cstdint>

namespace baker {

// Data (baker::data)
struct Allocator;           // allocator.hpp
class Symbol;               // symbol.hpp
enum class Type : uint8_t;  // node.hpp
class Node;
class Array;  // array.hpp
class Values;
class ValueList;
template <class T>
struct Convert;
template <class T>
class Ref;  // ref.hpp
template <class T>
class List;  // list.hpp
template <class Sig>
class Callback;                  // callback.hpp
class Path;                      // path.hpp
enum class ErrorCode : uint8_t;  // error.hpp
class Error;
struct ParseLimits;  // parse.hpp
class LoadSession;
class SourceMap;       // source_map.hpp
struct ConfigOptions;  // config.hpp
struct LayerPolicy;
struct LayerOptions;
class ConfigSnapshot;
class Config;
class TweakBase;  // tweak.hpp
template <class T>
class Tweak;
class TweakRegistry;
class Curve;     // curve.hpp
struct Project;  // project.hpp

// Script (baker::script)
class Args;  // script.hpp
class FuncRef;
class Context;
class ObjectBase;  // object.hpp
template <class T, class Base = ObjectBase>
class Object;
class MethodRef;
class Task;  // task.hpp
class Scheduler;

namespace shape {
class Set;  // shape.hpp (baker::shape)
}

}  // namespace baker
