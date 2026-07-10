#pragma once
#include "common.h"
#include <winrt/Windows.Data.Json.h>

namespace hp::json {
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

inline JsonObject Object(const JsonObject& parent, const wchar_t* name) {
  try {
    if (parent.HasKey(name) && parent.GetNamedValue(name).ValueType() == JsonValueType::Object) {
      return parent.GetNamedObject(name);
    }
  } catch (...) {
  }
  return JsonObject{};
}

inline JsonArray Array(const JsonObject& parent, const wchar_t* name) {
  try {
    if (parent.HasKey(name) && parent.GetNamedValue(name).ValueType() == JsonValueType::Array) {
      return parent.GetNamedArray(name);
    }
  } catch (...) {
  }
  return JsonArray{};
}

inline std::wstring Text(const JsonObject& object, const wchar_t* name, const std::wstring& fallback = {}) {
  try {
    if (object.HasKey(name) && object.GetNamedValue(name).ValueType() == JsonValueType::String) {
      return object.GetNamedString(name).c_str();
    }
  } catch (...) {
  }
  return fallback;
}

inline double Number(const JsonObject& object, const wchar_t* name, double fallback = 0) {
  try {
    if (object.HasKey(name) && object.GetNamedValue(name).ValueType() == JsonValueType::Number) {
      return object.GetNamedNumber(name);
    }
  } catch (...) {
  }
  return fallback;
}

inline bool Boolean(const JsonObject& object, const wchar_t* name, bool fallback = false) {
  try {
    if (object.HasKey(name) && object.GetNamedValue(name).ValueType() == JsonValueType::Boolean) {
      return object.GetNamedBoolean(name);
    }
  } catch (...) {
  }
  return fallback;
}

inline std::wstring Stringify(const JsonObject& object, const wchar_t* name) {
  try {
    if (object.HasKey(name)) return object.GetNamedValue(name).Stringify().c_str();
  } catch (...) {
  }
  return {};
}
}
