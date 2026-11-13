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
#define DECODE_STACK_SLOTS_PER_TABLE 4

#if defined(_MSC_VER)
#define LUA_SIMDJSON_NOINLINE __declspec(noinline)
#else
#define LUA_SIMDJSON_NOINLINE __attribute__((noinline))
#endif

using namespace simdjson;

enum class decode_status {
  ok,
  simdjson_error,
  unknown_json_type,
  depth_exceeded,
  stack_overflow,
  input_too_large,
  allocation_failed,
};

struct decode_outcome {
  decode_status status;
  simdjson::error_code error;
  int detail;
};

struct decode_context {
  int max_depth;
  int current_depth = 0;
  decode_status status = decode_status::ok;
  simdjson::error_code error = simdjson::SUCCESS;
  int detail = 0;
};

static bool fail_status(decode_context &context, decode_status status,
                        int detail = 0) {
  context.status = status;
  context.detail = detail;
  return false;
}

static bool fail_simdjson(decode_context &context, simdjson::error_code error) {
  context.status = decode_status::simdjson_error;
  context.error = error;
  return false;
}

static int raise_decode_error(lua_State *L, const decode_outcome &outcome) {
  switch (outcome.status) {
  case decode_status::ok:
    return 1;
  case decode_status::simdjson_error:
    return luaL_error(L, "%s", simdjson::error_message(outcome.error));
  case decode_status::unknown_json_type:
    return luaL_error(L, "simdjson::ondemand::json_type::unknown or "
                         "unsupported type encountered");
  case decode_status::depth_exceeded:
    return luaL_error(L, "maximum nesting depth exceeded (limit: %d)",
                      outcome.detail);
  case decode_status::stack_overflow:
    return luaL_error(L, "cannot decode: Lua stack overflow");
  case decode_status::input_too_large:
    return luaL_error(L, "JSON input is too large");
  case decode_status::allocation_failed:
    return luaL_error(L, "failed to allocate JSON parse buffer");
  }
  return luaL_error(L, "unknown decoder failure");
}

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
thread_local simdjson::padded_string file_parse_buffer;

static void shrink_decode_buffers(size_t desired_buffer_size,
                                  int max_depth) noexcept {
  file_parse_buffer = simdjson::padded_string();
  size_t max_retained = desired_buffer_size + SIMDJSON_PADDING;
  if (parse_buffer_capacity > max_retained) {
    char *replacement = new (std::nothrow) char[max_retained];
    if (replacement != nullptr) {
      parse_buffer.reset(replacement);
      parse_buffer_capacity = max_retained;
    } else {
      parse_buffer.reset();
      parse_buffer_capacity = 0;
    }
  }
  if (ondemand_parser.capacity() > max_retained) {
    ondemand_parser = ondemand::parser{};
    (void)ondemand_parser.allocate(desired_buffer_size,
                                   static_cast<size_t>(max_depth));
  }
}

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

template <typename T>
static LUA_SIMDJSON_NOINLINE bool
convert_ondemand_scalar_to_table(lua_State *L, T &element,
                                 ondemand::json_type type,
                                 decode_context &context) {
  switch (type) {
  case ondemand::json_type::number: {
    ondemand::number number;
    if (auto error = element.get_number().get(number); error) {
      return fail_simdjson(context, error);
    }
    ondemand::number_type number_type = number.get_number_type();
    switch (number_type) {
    case SIMDJSON_BUILTIN_IMPLEMENTATION::number_type::floating_point_number: {
      double value = 0;
      if (auto error = element.get_double().get(value); error) {
        return fail_simdjson(context, error);
      }
      lua_pushnumber(L, value);
      break;
    }

    case SIMDJSON_BUILTIN_IMPLEMENTATION::number_type::signed_integer: {
      int64_t value = 0;
      if (auto error = element.get_int64().get(value); error) {
        return fail_simdjson(context, error);
      }
      lua_pushinteger(L, value);
      break;
    }

    case SIMDJSON_BUILTIN_IMPLEMENTATION::number_type::unsigned_integer: {
// a uint64 can be greater than an int64, so we must check how large and pass as
// a number if larger but LUA_MAXINTEGER (which is only defined in 5.3+)
#if defined(LUA_MAXINTEGER)
      uint64_t actual_value = 0;
      if (auto error = element.get_uint64().get(actual_value); error) {
        return fail_simdjson(context, error);
      }
      if (actual_value > LUA_MAXINTEGER) {
        lua_pushnumber(L, actual_value);
      } else {
        lua_pushinteger(L, actual_value);
      }
#else
      double value = 0;
      if (auto error = element.get_double().get(value); error) {
        return fail_simdjson(context, error);
      }
      lua_pushnumber(L, value);
#endif
      break;
    }

    case SIMDJSON_BUILTIN_IMPLEMENTATION::number_type::big_integer: {
      double value = 0;
      if (auto error = element.get_double().get(value); error) {
        return fail_simdjson(context, error);
      }
      lua_pushnumber(L, value);
      break;
    }
    }
    return true;
  }

  case ondemand::json_type::string: {
    std::string_view s;
    if (auto error = element.get_string().get(s); error) {
      return fail_simdjson(context, error);
    }
    lua_pushlstring(L, s.data(), s.size());
    return true;
  }

  case ondemand::json_type::boolean: {
    bool value = false;
    if (auto error = element.get_bool().get(value); error) {
      return fail_simdjson(context, error);
    }
    lua_pushboolean(L, value);
    return true;
  }

  case ondemand::json_type::null: {
    bool is_null = false;
    if (auto error = element.is_null().get(is_null); error) {
      return fail_simdjson(context, error);
    }
    if (!is_null) {
      return fail_simdjson(context, simdjson::INCORRECT_TYPE);
    }
    lua_pushlightuserdata(L, NULL);
    return true;
  }

  case ondemand::json_type::array:
  case ondemand::json_type::object:
  case ondemand::json_type::unknown:
  default:
    return fail_status(context, decode_status::unknown_json_type);
  }
}

static LUA_SIMDJSON_NOINLINE bool
push_object_field_key(lua_State *L,
                      simdjson_result<ondemand::field> &field_result,
                      ondemand::value &child, decode_context &context) {
  ondemand::field field;
  if (auto error = std::move(field_result).get(field); error) {
    return fail_simdjson(context, error);
  }
  std::string_view s;
  if (auto error = field.unescaped_key().get(s); error) {
    return fail_simdjson(context, error);
  }
  lua_pushlstring(L, s.data(), s.size());
  child = std::move(field).value();
  return true;
}

template <typename T>
static LUA_SIMDJSON_NOINLINE bool
prepare_element_type(T &element, ondemand::json_type &type,
                     decode_context &context) {
  if (auto error = element.type().get(type); error) {
    return fail_simdjson(context, error);
  }
  return true;
}

template <typename T>
static LUA_SIMDJSON_NOINLINE bool prepare_array(lua_State *L, T &element,
                                                ondemand::array &array,
                                                decode_context &context) {
  if (context.current_depth >= context.max_depth) {
    return fail_status(context, decode_status::depth_exceeded,
                       context.max_depth);
  }
  if (!lua_checkstack(L, DECODE_STACK_SLOTS_PER_TABLE)) {
    return fail_status(context, decode_status::stack_overflow);
  }
  if (auto error = element.get_array().get(array); error) {
    return fail_simdjson(context, error);
  }
  context.current_depth++;
  lua_newtable(L);
  return true;
}

template <typename T>
static LUA_SIMDJSON_NOINLINE bool prepare_object(lua_State *L, T &element,
                                                 ondemand::object &object,
                                                 decode_context &context) {
  if (context.current_depth >= context.max_depth) {
    return fail_status(context, decode_status::depth_exceeded,
                       context.max_depth);
  }
  if (!lua_checkstack(L, DECODE_STACK_SLOTS_PER_TABLE)) {
    return fail_status(context, decode_status::stack_overflow);
  }
  if (auto error = element.get_object().get(object); error) {
    return fail_simdjson(context, error);
  }
  context.current_depth++;
  lua_newtable(L);
  return true;
}

template <typename T>
static bool convert_ondemand_element_to_table(lua_State *L, T &element,
                                              decode_context &context) {
  static_assert(std::is_base_of<ondemand::document, T>::value ||
                    std::is_base_of<ondemand::value, T>::value,
                "type parameter must be document or value");

  ondemand::json_type type;
  if (!prepare_element_type(element, type, context)) {
    return false;
  }

  switch (type) {
  case ondemand::json_type::array: {
    ondemand::array array;
    if (!prepare_array(L, element, array, context)) {
      return false;
    }

    int count = 1;
    for (simdjson_result<ondemand::value> child_result : array) {
      ondemand::value child;
      if (auto error = child_result.get(child); error) {
        return fail_simdjson(context, error);
      }
      if (!convert_ondemand_element_to_table(L, child, context)) {
        return false;
      }
      lua_rawseti(L, -2, count++);
    }
    context.current_depth--;
    return true;
  }

  case ondemand::json_type::object: {
    ondemand::object object;
    if (!prepare_object(L, element, object, context)) {
      return false;
    }

    for (simdjson_result<ondemand::field> field_result : object) {
      ondemand::value child;
      if (!push_object_field_key(L, field_result, child, context)) {
        return false;
      }
      if (!convert_ondemand_element_to_table(L, child, context)) {
        return false;
      }
      lua_rawset(L, -3);
    }
    context.current_depth--;
    return true;
  }

  default:
    return convert_ondemand_scalar_to_table(L, element, type, context);
  }
}

static LUA_SIMDJSON_NOINLINE decode_outcome
parse_value(lua_State *L, const char *json_str, size_t json_str_len,
            int max_depth, size_t desired_buffer_size) {
  shrink_decode_buffers(desired_buffer_size, max_depth);

  if (json_str_len > std::numeric_limits<size_t>::max() - SIMDJSON_PADDING) {
    return {decode_status::input_too_large, simdjson::SUCCESS, 0};
  }

  size_t required_capacity = json_str_len + SIMDJSON_PADDING;
  if (parse_buffer_capacity < required_capacity) {
    char *replacement = new (std::nothrow) char[required_capacity];
    if (replacement == nullptr) {
      return {decode_status::allocation_failed, simdjson::SUCCESS, 0};
    }
    parse_buffer.reset(replacement);
    parse_buffer_capacity = required_capacity;
  }

  std::memcpy(parse_buffer.get(), json_str, json_str_len);
  simdjson::padded_string_view padded_view(parse_buffer.get(), json_str_len,
                                           parse_buffer_capacity);

  ondemand::document doc;
  if (auto error = ondemand_parser.iterate(padded_view).get(doc); error) {
    shrink_decode_buffers(desired_buffer_size, max_depth);
    return {decode_status::simdjson_error, error, 0};
  }

  decode_context context{max_depth};
  if (!convert_ondemand_element_to_table(L, doc, context)) {
    shrink_decode_buffers(desired_buffer_size, max_depth);
    return {context.status, context.error, context.detail};
  }

  if (!doc.at_end()) {
    shrink_decode_buffers(desired_buffer_size, max_depth);
    return {decode_status::simdjson_error, simdjson::TRAILING_CONTENT, 0};
  }

  shrink_decode_buffers(desired_buffer_size, max_depth);
  return {decode_status::ok, simdjson::SUCCESS, 0};
}

static LUA_SIMDJSON_NOINLINE decode_outcome
parse_file_value(lua_State *L, const char *json_file, size_t json_file_len,
                 int max_depth, size_t desired_buffer_size) {
  shrink_decode_buffers(desired_buffer_size, max_depth);

  if (std::memchr(json_file, '\0', json_file_len) != nullptr) {
    return {decode_status::simdjson_error, simdjson::IO_ERROR, 0};
  }

  if (auto error =
          simdjson::padded_string::load(json_file).get(file_parse_buffer);
      error) {
    shrink_decode_buffers(desired_buffer_size, max_depth);
    return {decode_status::simdjson_error, error, 0};
  }

  ondemand::document doc;
  if (auto error = ondemand_parser.iterate(file_parse_buffer).get(doc); error) {
    shrink_decode_buffers(desired_buffer_size, max_depth);
    return {decode_status::simdjson_error, error, 0};
  }

  decode_context context{max_depth};
  if (!convert_ondemand_element_to_table(L, doc, context)) {
    shrink_decode_buffers(desired_buffer_size, max_depth);
    return {context.status, context.error, context.detail};
  }

  if (!doc.at_end()) {
    shrink_decode_buffers(desired_buffer_size, max_depth);
    return {decode_status::simdjson_error, simdjson::TRAILING_CONTENT, 0};
  }

  shrink_decode_buffers(desired_buffer_size, max_depth);
  return {decode_status::ok, simdjson::SUCCESS, 0};
}

static int parse(lua_State *L) {
  int argument_count = lua_gettop(L);
  luaL_argcheck(L, argument_count >= 1 && argument_count <= 2, 1,
                "expected 1 or 2 arguments");
  size_t json_str_len = 0;
  const char *json_str = luaL_checklstring(L, 1, &json_str_len);

  int max_depth = read_max_decode_depth(L);
  size_t desired_buffer_size = read_decode_buffer_size(L);
  if (argument_count == 2) {
    luaL_checktype(L, 2, LUA_TTABLE);
    parse_decode_options(L, 2, max_depth, desired_buffer_size);
  }

  const int initial_top = lua_gettop(L);
  decode_outcome outcome =
      parse_value(L, json_str, json_str_len, max_depth, desired_buffer_size);
  if (outcome.status != decode_status::ok) {
    lua_settop(L, initial_top);
    return raise_decode_error(L, outcome);
  }

  return 1;
}

static int parse_file(lua_State *L) {
  int argument_count = lua_gettop(L);
  luaL_argcheck(L, argument_count >= 1 && argument_count <= 2, 1,
                "expected 1 or 2 arguments");
  size_t json_file_len = 0;
  const char *json_file = luaL_checklstring(L, 1, &json_file_len);

  int max_depth = read_max_decode_depth(L);
  size_t desired_buffer_size = read_decode_buffer_size(L);
  if (argument_count == 2) {
    luaL_checktype(L, 2, LUA_TTABLE);
    parse_decode_options(L, 2, max_depth, desired_buffer_size);
  }

  const int initial_top = lua_gettop(L);
  decode_outcome outcome = parse_file_value(L, json_file, json_file_len,
                                            max_depth, desired_buffer_size);
  if (outcome.status != decode_status::ok) {
    lua_settop(L, initial_top);
    return raise_decode_error(L, outcome);
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
  lua_pushfstring(L, "%s (%s)", implementation->name().c_str(),
                  implementation->description().c_str());
  return 1;
}

// ParsedObject as C++ class
#define LUA_MYOBJECT "ParsedObject"
class ParsedObject {
private:
  simdjson::padded_string json_string;
  ondemand::document doc;
  std::unique_ptr<ondemand::parser> parser;

  ParsedObject() = default;

public:
  ~ParsedObject() = default;
  ondemand::document *get_doc() { return &(this->doc); }

  static LUA_SIMDJSON_NOINLINE decode_outcome create_from_string(
      const char *json_str, size_t json_str_len, ParsedObject **out) noexcept {
    *out = nullptr;
    if (json_str_len > std::numeric_limits<size_t>::max() - SIMDJSON_PADDING) {
      return {decode_status::input_too_large, simdjson::SUCCESS, 0};
    }

    std::unique_ptr<ParsedObject> obj(new (std::nothrow) ParsedObject());
    if (!obj) {
      return {decode_status::allocation_failed, simdjson::SUCCESS, 0};
    }

    obj->parser.reset(new (std::nothrow) ondemand::parser{});
    if (!obj->parser) {
      return {decode_status::allocation_failed, simdjson::SUCCESS, 0};
    }

    obj->json_string = simdjson::padded_string(json_str, json_str_len);
    if (obj->json_string.data() == nullptr) {
      return {decode_status::allocation_failed, simdjson::SUCCESS, 0};
    }

    if (auto error = obj->parser->iterate(obj->json_string).get(obj->doc);
        error) {
      return {decode_status::simdjson_error, error, 0};
    }

    *out = obj.release();
    return {decode_status::ok, simdjson::SUCCESS, 0};
  }

  static LUA_SIMDJSON_NOINLINE decode_outcome
  create_from_file(const char *json_file, size_t json_file_len,
                   ParsedObject **out) noexcept {
    *out = nullptr;
    if (std::memchr(json_file, '\0', json_file_len) != nullptr) {
      return {decode_status::simdjson_error, simdjson::IO_ERROR, 0};
    }

    std::unique_ptr<ParsedObject> obj(new (std::nothrow) ParsedObject());
    if (!obj) {
      return {decode_status::allocation_failed, simdjson::SUCCESS, 0};
    }

    obj->parser.reset(new (std::nothrow) ondemand::parser{});
    if (!obj->parser) {
      return {decode_status::allocation_failed, simdjson::SUCCESS, 0};
    }

    if (auto error =
            simdjson::padded_string::load(json_file).get(obj->json_string);
        error) {
      return {decode_status::simdjson_error, error, 0};
    }

    if (auto error = obj->parser->iterate(obj->json_string).get(obj->doc);
        error) {
      return {decode_status::simdjson_error, error, 0};
    }

    *out = obj.release();
    return {decode_status::ok, simdjson::SUCCESS, 0};
  }
};

static int ParsedObject_delete(lua_State *L) {
  ParsedObject **slot = reinterpret_cast<ParsedObject **>(lua_touserdata(L, 1));
  if (slot != nullptr && *slot != nullptr) {
    delete *slot;
    *slot = nullptr;
  }
  return 0;
}

static int ParsedObject_open(lua_State *L) {
  size_t json_str_len = 0;
  const char *json_str = luaL_checklstring(L, 1, &json_str_len);

  ParsedObject **slot = reinterpret_cast<ParsedObject **>(
      lua_newuserdata(L, sizeof(ParsedObject *)));
  *slot = nullptr;
  luaL_getmetatable(L, LUA_MYOBJECT);
  lua_setmetatable(L, -2);

  decode_outcome outcome =
      ParsedObject::create_from_string(json_str, json_str_len, slot);
  if (outcome.status != decode_status::ok) {
    return raise_decode_error(L, outcome);
  }
  return 1;
}

static int ParsedObject_open_file(lua_State *L) {
  size_t json_file_len = 0;
  const char *json_file = luaL_checklstring(L, 1, &json_file_len);

  ParsedObject **slot = reinterpret_cast<ParsedObject **>(
      lua_newuserdata(L, sizeof(ParsedObject *)));
  *slot = nullptr;
  luaL_getmetatable(L, LUA_MYOBJECT);
  lua_setmetatable(L, -2);

  decode_outcome outcome =
      ParsedObject::create_from_file(json_file, json_file_len, slot);
  if (outcome.status != decode_status::ok) {
    return raise_decode_error(L, outcome);
  }
  return 1;
}

static LUA_SIMDJSON_NOINLINE decode_outcome
at_pointer_value(lua_State *L, ondemand::document *document,
                 const char *pointer, size_t pointer_len, int max_depth) {
  decode_context context{max_depth};
  if (pointer_len == 0) {
    document->rewind();
    if (!convert_ondemand_element_to_table(L, *document, context)) {
      return {context.status, context.error, context.detail};
    }
    return {decode_status::ok, simdjson::SUCCESS, 0};
  }

  ondemand::value returned_element;
  if (auto error = document->at_pointer(std::string_view(pointer, pointer_len))
                       .get(returned_element);
      error) {
    return {decode_status::simdjson_error, error, 0};
  }
  if (!convert_ondemand_element_to_table(L, returned_element, context)) {
    return {context.status, context.error, context.detail};
  }
  return {decode_status::ok, simdjson::SUCCESS, 0};
}

static int ParsedObject_atPointer(lua_State *L) {
  int argument_count = lua_gettop(L);
  luaL_argcheck(L, argument_count >= 2 && argument_count <= 3, 2,
                "expected 2 or 3 arguments");
  ParsedObject **slot =
      reinterpret_cast<ParsedObject **>(luaL_checkudata(L, 1, LUA_MYOBJECT));
  if (slot == nullptr || *slot == nullptr) {
    return luaL_error(L, "invalid ParsedObject");
  }
  ondemand::document *document = (*slot)->get_doc();
  size_t pointer_len = 0;
  const char *pointer = luaL_checklstring(L, 2, &pointer_len);

  int max_depth = read_max_decode_depth(L);
  size_t desired_buffer_size = read_decode_buffer_size(L);
  if (argument_count == 3) {
    luaL_checktype(L, 3, LUA_TTABLE);
    parse_decode_options(L, 3, max_depth, desired_buffer_size);
  }
  (void)desired_buffer_size;

  const int initial_top = lua_gettop(L);
  decode_outcome outcome =
      at_pointer_value(L, document, pointer, pointer_len, max_depth);
  if (outcome.status != decode_status::ok) {
    lua_settop(L, initial_top);
    return raise_decode_error(L, outcome);
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
