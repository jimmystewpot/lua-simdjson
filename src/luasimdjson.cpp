#include <cmath>
#include <cstring>
#include <limits>
#include <lua.hpp>
#include <memory>
#include <new>

#define NDEBUG
#define __OPTIMIZE__ 1

#include "luasimdjson.h"
#include "simdjson.h"

#define LUA_SIMDJSON_NAME "simdjson"
#define LUA_SIMDJSON_VERSION "1.0.1"
#define LUA_SIMDJSON_MAX_DECODE_DEPTH_KEY "simdjson.maxDecodeDepth"
#define LUA_SIMDJSON_DECODE_BUFFER_SIZE_KEY "simdjson.decodeBufferSize"
#define DEFAULT_MAX_DECODE_DEPTH static_cast<int>(simdjson::DEFAULT_MAX_DEPTH)
#define MAX_DECODE_DEPTH static_cast<int>(simdjson::DEFAULT_MAX_DEPTH)
#define DEFAULT_DECODE_BUFFER_SIZE (16 * 1024)
#define MAX_DECODE_BUFFER_SIZE (64 * 1024 * 1024)

using namespace simdjson;

#if !defined(luaL_newlibtable) &&                                              \
    (!defined LUA_VERSION_NUM || LUA_VERSION_NUM <= 501)
/*
** set_funcs compat for 5.1
** Stolen from: http://lua-users.org/wiki/CompatibilityWithLuaFive
** Adapted from Lua 5.2.0
*/
static void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup) {
  luaL_checkstack(L, nup + 1, "too many upvalues");
  for (; l->name != NULL; l++) { /* fill the table with given functions */
    int i;
    lua_pushstring(L, l->name);
    for (i = 0; i < nup; i++) /* copy upvalues to the top */
      lua_pushvalue(L, -(nup + 1));
    lua_pushcclosure(L, l->func, nup); /* closure with those upvalues */
    lua_settable(L, -(nup + 3));
  }
  lua_pop(L, nup); /* remove upvalues */
}
#endif

thread_local ondemand::parser ondemand_parser;
thread_local std::unique_ptr<char[]> parse_buffer;
thread_local size_t parse_buffer_capacity = 0;

static int absolute_index(lua_State *L, int index) {
  if (index > 0 || index <= LUA_REGISTRYINDEX) {
    return index;
  }
  return lua_gettop(L) + index + 1;
}

static lua_Integer check_integer(lua_State *L, int index, const char *name,
                                 lua_Integer maximum) {
  if (lua_type(L, index) != LUA_TNUMBER) {
    luaL_error(L, "%s must be an integer", name);
  }

  lua_Number number = lua_tonumber(L, index);
  if (!std::isfinite(number) || std::floor(number) != number || number < 1 ||
      number > static_cast<lua_Number>(maximum)) {
    luaL_error(L, "%s must be an integer between 1 and %lld", name,
               static_cast<long long>(maximum));
  }
  return static_cast<lua_Integer>(number);
}

static int check_decode_depth(lua_State *L, int index, const char *name) {
  return static_cast<int>(check_integer(L, index, name, MAX_DECODE_DEPTH));
}

static size_t check_decode_buffer_size(lua_State *L, int index,
                                       const char *name) {
  return static_cast<size_t>(
      check_integer(L, index, name, MAX_DECODE_BUFFER_SIZE));
}

static bool is_known_decode_option(lua_State *L, int index) {
  size_t length = 0;
  const char *key = lua_tolstring(L, index, &length);
  return (length == sizeof("maxDepth") - 1 &&
          std::memcmp(key, "maxDepth", length) == 0) ||
         (length == sizeof("bufferSize") - 1 &&
          std::memcmp(key, "bufferSize", length) == 0);
}

static void validate_decode_options(lua_State *L, int table_index) {
  table_index = absolute_index(L, table_index);
  lua_pushnil(L);
  while (lua_next(L, table_index) != 0) {
    if (lua_type(L, -2) != LUA_TSTRING || !is_known_decode_option(L, -2)) {
      luaL_error(L, "unknown decode option");
    }
    lua_pop(L, 1);
  }
}

static void raw_get_field(lua_State *L, int table_index, const char *key) {
  lua_pushstring(L, key);
  lua_rawget(L, absolute_index(L, table_index));
}

static void parse_decode_options(lua_State *L, int table_index, int &max_depth,
                                 size_t &desired_buffer_size) {
  table_index = absolute_index(L, table_index);
  validate_decode_options(L, table_index);

  raw_get_field(L, table_index, "maxDepth");
  if (!lua_isnil(L, -1)) {
    max_depth = check_decode_depth(L, -1, "maxDepth");
  }
  lua_pop(L, 1);

  raw_get_field(L, table_index, "bufferSize");
  if (!lua_isnil(L, -1)) {
    desired_buffer_size = check_decode_buffer_size(L, -1, "bufferSize");
  }
  lua_pop(L, 1);
}

static int read_max_decode_depth(lua_State *L) {
  lua_pushstring(L, LUA_SIMDJSON_MAX_DECODE_DEPTH_KEY);
  lua_rawget(L, LUA_REGISTRYINDEX);
  int max_depth = DEFAULT_MAX_DECODE_DEPTH;
  if (lua_type(L, -1) == LUA_TNUMBER) {
    lua_Number value = lua_tonumber(L, -1);
    if (std::isfinite(value) && std::floor(value) == value && value >= 1 &&
        value <= MAX_DECODE_DEPTH) {
      max_depth = static_cast<int>(value);
    }
  }
  lua_pop(L, 1);
  return max_depth;
}

static void write_max_decode_depth(lua_State *L, int max_depth) {
  lua_pushstring(L, LUA_SIMDJSON_MAX_DECODE_DEPTH_KEY);
  lua_pushinteger(L, max_depth);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

static size_t read_decode_buffer_size(lua_State *L) {
  lua_pushstring(L, LUA_SIMDJSON_DECODE_BUFFER_SIZE_KEY);
  lua_rawget(L, LUA_REGISTRYINDEX);
  size_t buffer_size = DEFAULT_DECODE_BUFFER_SIZE;
  if (lua_type(L, -1) == LUA_TNUMBER) {
    lua_Number value = lua_tonumber(L, -1);
    if (std::isfinite(value) && std::floor(value) == value && value >= 1 &&
        value <= MAX_DECODE_BUFFER_SIZE) {
      buffer_size = static_cast<size_t>(value);
    }
  }
  lua_pop(L, 1);
  return buffer_size;
}

static void write_decode_buffer_size(lua_State *L, size_t buffer_size) {
  lua_pushstring(L, LUA_SIMDJSON_DECODE_BUFFER_SIZE_KEY);
  lua_pushinteger(L, static_cast<lua_Integer>(buffer_size));
  lua_rawset(L, LUA_REGISTRYINDEX);
}

static simdjson::padded_string_view
copy_to_padded_buffer(lua_State *L, const char *data, size_t length) {
  if (length > std::numeric_limits<size_t>::max() - SIMDJSON_PADDING) {
    luaL_error(L, "JSON input is too large");
    return simdjson::padded_string_view();
  }

  size_t required_capacity = length + SIMDJSON_PADDING;
  if (parse_buffer_capacity < required_capacity) {
    char *replacement = new (std::nothrow) char[required_capacity];
    if (replacement == nullptr) {
      luaL_error(L, "failed to allocate JSON parse buffer");
      return simdjson::padded_string_view();
    }
    parse_buffer.reset(replacement);
    parse_buffer_capacity = required_capacity;
  }

  std::memcpy(parse_buffer.get(), data, length);
  return simdjson::padded_string_view(parse_buffer.get(), length,
                                      parse_buffer_capacity);
}

template <typename T>
void convert_ondemand_element_to_table(lua_State *L, T &element) {
  static_assert(std::is_base_of<ondemand::document, T>::value ||
                    std::is_base_of<ondemand::value, T>::value,
                "type parameter must be document or value");

  switch (element.type()) {
  case ondemand::json_type::array: {
    int count = 1;
    lua_newtable(L);

    for (ondemand::value child : element.get_array()) {
      lua_pushinteger(L, count);
      convert_ondemand_element_to_table(L, child);
      lua_settable(L, -3);
      count = count + 1;
    }
    break;
  }

  case ondemand::json_type::object:
    lua_newtable(L);
    for (ondemand::field field : element.get_object()) {
      std::string_view s = field.unescaped_key();
      lua_pushlstring(L, s.data(), s.size());
      convert_ondemand_element_to_table(L, field.value());
      lua_settable(L, -3);
    }
    break;

  case ondemand::json_type::number: {
    ondemand::number number = element.get_number();
    ondemand::number_type number_type = number.get_number_type();
    switch (number_type) {
    case SIMDJSON_BUILTIN_IMPLEMENTATION::number_type::floating_point_number:
      lua_pushnumber(L, element.get_double());
      break;

    case SIMDJSON_BUILTIN_IMPLEMENTATION::number_type::signed_integer:
      lua_pushinteger(L, element.get_int64());
      break;

    case SIMDJSON_BUILTIN_IMPLEMENTATION::number_type::unsigned_integer: {
// a uint64 can be greater than an int64, so we must check how large and pass as
// a number if larger but LUA_MAXINTEGER (which is only defined in 5.3+)
#if defined(LUA_MAXINTEGER)
      uint64_t actual_value = element.get_uint64();
      if (actual_value > LUA_MAXINTEGER) {
        lua_pushnumber(L, actual_value);
      } else {
        lua_pushinteger(L, actual_value);
      }
#else
      lua_pushnumber(L, element.get_double());
#endif
      break;
    }

    case SIMDJSON_BUILTIN_IMPLEMENTATION::number_type::big_integer:
      lua_pushnumber(L, element.get_double());
      break;
    }
    break;
  }

  case ondemand::json_type::string: {
    std::string_view s = element.get_string();
    lua_pushlstring(L, s.data(), s.size());
    break;
  }

  case ondemand::json_type::boolean:
    lua_pushboolean(L, element.get_bool());
    break;

  case ondemand::json_type::null:
    // calling is_null().value() will trigger an exception if the value is
    // invalid
    if (element.is_null().value()) {
      lua_pushlightuserdata(L, NULL);
    }
    break;

  case ondemand::json_type::unknown:
  default:
    luaL_error(L, "simdjson::ondemand::json_type::unknown or unsupported type "
                  "encountered");
    break;
  }
}

static int parse(lua_State *L) {
  int argument_count = lua_gettop(L);
  luaL_argcheck(L, argument_count >= 1 && argument_count <= 2, 1,
                "expected 1 or 2 arguments");
  size_t json_str_len;
  const char *json_str = luaL_checklstring(L, 1, &json_str_len);

  int max_depth = read_max_decode_depth(L);
  size_t desired_buffer_size = read_decode_buffer_size(L);
  if (argument_count == 2) {
    luaL_checktype(L, 2, LUA_TTABLE);
    parse_decode_options(L, 2, max_depth, desired_buffer_size);
  }
  (void)max_depth;
  (void)desired_buffer_size;

  ondemand::document doc;

  try {
    // Lua owns json_str and does not guarantee simdjson's required trailing
    // padding. Copy it into reusable padded storage before parsing.
    doc = ondemand_parser.iterate(
        copy_to_padded_buffer(L, json_str, json_str_len));
    convert_ondemand_element_to_table(L, doc);
  } catch (simdjson::simdjson_error &error) {
    luaL_error(L, error.what());
  }

  return 1;
}

static int parse_file(lua_State *L) {
  int argument_count = lua_gettop(L);
  luaL_argcheck(L, argument_count >= 1 && argument_count <= 2, 1,
                "expected 1 or 2 arguments");
  const char *json_file = luaL_checkstring(L, 1);

  int max_depth = read_max_decode_depth(L);
  size_t desired_buffer_size = read_decode_buffer_size(L);
  if (argument_count == 2) {
    luaL_checktype(L, 2, LUA_TTABLE);
    parse_decode_options(L, 2, max_depth, desired_buffer_size);
  }
  (void)max_depth;
  (void)desired_buffer_size;

  padded_string json_string;
  ondemand::document doc;

  try {
    json_string = padded_string::load(json_file);
    doc = ondemand_parser.iterate(json_string);
    convert_ondemand_element_to_table(L, doc);
  } catch (simdjson::simdjson_error &error) {
    luaL_error(L, error.what());
  }

  return 1;
}

static int set_max_decode_depth(lua_State *L) {
  int max_depth = check_decode_depth(L, 1, "maximum decode depth");
  write_max_decode_depth(L, max_depth);
  return 0;
}

static int get_max_decode_depth(lua_State *L) {
  lua_pushinteger(L, read_max_decode_depth(L));
  return 1;
}

static int set_decode_buffer_size(lua_State *L) {
  size_t buffer_size = check_decode_buffer_size(L, 1, "decode buffer size");
  write_decode_buffer_size(L, buffer_size);
  return 0;
}

static int get_decode_buffer_size(lua_State *L) {
  lua_pushinteger(L, static_cast<lua_Integer>(read_decode_buffer_size(L)));
  return 1;
}

static int active_implementation(lua_State *L) {
  const auto &implementation = simdjson::get_active_implementation();
  std::string name = implementation->name();
  const std::string description = implementation->description();
  const std::string implementation_name = name + " (" + description + ")";

  lua_pushlstring(L, implementation_name.data(), implementation_name.size());

  return 1;
}

// ParsedObject as C++ class
#define LUA_MYOBJECT "ParsedObject"
class ParsedObject {
private:
  simdjson::padded_string json_string;
  ondemand::document doc;
  std::unique_ptr<ondemand::parser> parser;

public:
  ParsedObject(const char *json_file)
      : json_string(padded_string::load(json_file)),
        parser(new ondemand::parser{}) {
    this->doc = this->parser.get()->iterate(json_string);
  }
  ParsedObject(const char *json_str, size_t json_str_len)
      : json_string(json_str, json_str_len), parser(new ondemand::parser{}) {
    this->doc = this->parser.get()->iterate(json_string);
  }
  ~ParsedObject() {}
  ondemand::document *get_doc() { return &(this->doc); }
};

static int ParsedObject_delete(lua_State *L) {
  delete *reinterpret_cast<ParsedObject **>(lua_touserdata(L, 1));
  return 0;
}

static int ParsedObject_open(lua_State *L) {
  size_t json_str_len;
  const char *json_str = luaL_checklstring(L, 1, &json_str_len);

  try {
    ParsedObject **parsedObject =
        (ParsedObject **)(lua_newuserdata(L, sizeof(ParsedObject *)));
    *parsedObject = new ParsedObject(json_str, json_str_len);
    luaL_getmetatable(L, LUA_MYOBJECT);
    lua_setmetatable(L, -2);
  } catch (simdjson::simdjson_error &error) {
    luaL_error(L, error.what());
  }
  return 1;
}

static int ParsedObject_open_file(lua_State *L) {
  const char *json_file = luaL_checkstring(L, 1);

  try {
    ParsedObject **parsedObject =
        (ParsedObject **)(lua_newuserdata(L, sizeof(ParsedObject *)));
    *parsedObject = new ParsedObject(json_file);
    luaL_getmetatable(L, LUA_MYOBJECT);
    lua_setmetatable(L, -2);
  } catch (simdjson::simdjson_error &error) {
    luaL_error(L, error.what());
  }

  return 1;
}

static int ParsedObject_atPointer(lua_State *L) {
  int argument_count = lua_gettop(L);
  luaL_argcheck(L, argument_count >= 2 && argument_count <= 3, 2,
                "expected 2 or 3 arguments");
  ondemand::document *document =
      (*reinterpret_cast<ParsedObject **>(luaL_checkudata(L, 1, LUA_MYOBJECT)))
          ->get_doc();
  const char *pointer = luaL_checkstring(L, 2);

  int max_depth = read_max_decode_depth(L);
  size_t desired_buffer_size = read_decode_buffer_size(L);
  if (argument_count == 3) {
    luaL_checktype(L, 3, LUA_TTABLE);
    parse_decode_options(L, 3, max_depth, desired_buffer_size);
  }
  (void)max_depth;
  (void)desired_buffer_size;

  try {
    ondemand::value returned_element = document->at_pointer(pointer);
    convert_ondemand_element_to_table(L, returned_element);
  } catch (simdjson::simdjson_error &error) {
    luaL_error(L, error.what());
  }

  return 1;
}

static int ParsedObject_newindex(lua_State *L) {
  luaL_error(
      L,
      "This should be treated as a read-only table. We may one day add array "
      "access for the elements, and it'll likely not be modifiable.");
  return 1;
}

static const struct luaL_Reg arraylib_m[] = {
    {"atPointer", ParsedObject_atPointer},
    {"__newindex", ParsedObject_newindex},
    {"__gc", ParsedObject_delete},
    {NULL, NULL}};

int luaopen_simdjson(lua_State *L) {
  luaL_newmetatable(L, LUA_MYOBJECT);
  lua_pushvalue(L, -1); /* duplicates the metatable */
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, arraylib_m, 0);

  // luaL_newlib(L, luasimdjson);

  lua_newtable(L);
  luaL_setfuncs(L, luasimdjson, 0);

  lua_pushlightuserdata(L, NULL);
  lua_setfield(L, -2, "null");

  lua_pushliteral(L, LUA_SIMDJSON_NAME);
  lua_setfield(L, -2, "_NAME");
  lua_pushliteral(L, LUA_SIMDJSON_VERSION);
  lua_setfield(L, -2, "_VERSION");

  return 1;
}
