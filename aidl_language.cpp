#include "aidl_language.h"
#include "aidl_typenames.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <android-base/parsedouble.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include "aidl_language_y.h"
#include "logging.h"

#ifdef _WIN32
int isatty(int  fd)
{
    return (fd == 0);
}
#endif

using android::aidl::IoDelegate;
using android::base::Join;
using android::base::Split;
using std::cerr;
using std::endl;
using std::pair;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {
bool is_java_keyword(const char* str) {
  static const std::vector<std::string> kJavaKeywords{
      "abstract", "assert", "boolean",    "break",     "byte",       "case",      "catch",
      "char",     "class",  "const",      "continue",  "default",    "do",        "double",
      "else",     "enum",   "extends",    "final",     "finally",    "float",     "for",
      "goto",     "if",     "implements", "import",    "instanceof", "int",       "interface",
      "long",     "native", "new",        "package",   "private",    "protected", "public",
      "return",   "short",  "static",     "strictfp",  "super",      "switch",    "synchronized",
      "this",     "throw",  "throws",     "transient", "try",        "void",      "volatile",
      "while",    "true",   "false",      "null",
  };
  return std::find(kJavaKeywords.begin(), kJavaKeywords.end(), str) != kJavaKeywords.end();
}
}  // namespace

void yylex_init(void **);
void yylex_destroy(void *);
void yyset_in(FILE *f, void *);
int yyparse(Parser*);
YY_BUFFER_STATE yy_scan_buffer(char *, size_t, void *);
void yy_delete_buffer(YY_BUFFER_STATE, void *);

AidlToken::AidlToken(const std::string& text, const std::string& comments)
    : text_(text),
      comments_(comments) {}

AidlLocation::AidlLocation(const std::string& file, Point begin, Point end)
    : file_(file), begin_(begin), end_(end) {}

std::ostream& operator<<(std::ostream& os, const AidlLocation& l) {
  os << l.file_ << ":" << l.begin_.line << "." << l.begin_.column << "-";
  if (l.begin_.line != l.end_.line) {
    os << l.end_.line << ".";
  }
  os << l.end_.column;
  return os;
}

AidlNode::AidlNode(const AidlLocation& location) : location_(location) {}

std::string AidlNode::PrintLocation() const {
  std::stringstream ss;
  ss << location_.file_ << ":" << location_.begin_.line;
  return ss.str();
}

AidlError::AidlError(bool fatal) : os_(std::cerr), fatal_(fatal) {
  os_ << "ERROR: ";
}

static const string kNullable("nullable");
static const string kUtf8InCpp("utf8InCpp");
static const string kVintfStability("VintfStability");
static const string kUnsupportedAppUsage("UnsupportedAppUsage");
static const string kSystemApi("SystemApi");
static const string kStableParcelable("JavaOnlyStableParcelable");
static const string kBacking("Backing");

static const std::map<string, std::map<std::string, std::string>> kAnnotationParameters{
    {kNullable, {}},
    {kUtf8InCpp, {}},
    {kVintfStability, {}},
    {kUnsupportedAppUsage,
     {{"expectedSignature", "String"},
      {"implicitMember", "String"},
      {"maxTargetSdk", "int"},
      {"publicAlternatives", "String"},
      {"trackingBug", "long"}}},
    {kSystemApi, {}},
    {kStableParcelable, {}},
    {kBacking, {{"type", "String"}}}};

AidlAnnotation* AidlAnnotation::Parse(
    const AidlLocation& location, const string& name,
    std::map<std::string, std::shared_ptr<AidlConstantValue>>* parameter_list) {
  if (kAnnotationParameters.find(name) == kAnnotationParameters.end()) {
    std::ostringstream stream;
    stream << "'" << name << "' is not a recognized annotation. ";
    stream << "It must be one of:";
    for (const auto& kv : kAnnotationParameters) {
      stream << " " << kv.first;
    }
    stream << ".";
    AIDL_ERROR(location) << stream.str();
    return nullptr;
  }
  if (parameter_list == nullptr) {
    return new AidlAnnotation(location, name);
  }

  return new AidlAnnotation(location, name, std::move(*parameter_list));
}

AidlAnnotation::AidlAnnotation(const AidlLocation& location, const string& name)
    : AidlAnnotation(location, name, {}) {}

AidlAnnotation::AidlAnnotation(
    const AidlLocation& location, const string& name,
    std::map<std::string, std::shared_ptr<AidlConstantValue>>&& parameters)
    : AidlNode(location), name_(name), parameters_(std::move(parameters)) {}

bool AidlAnnotation::CheckValid() const {
  auto supported_params_iterator = kAnnotationParameters.find(GetName());
  if (supported_params_iterator == kAnnotationParameters.end()) {
    AIDL_ERROR(this) << GetName() << " annotation does not have any supported parameters.";
    return false;
  }
  const auto& supported_params = supported_params_iterator->second;
  for (const auto& name_and_param : parameters_) {
    const std::string& param_name = name_and_param.first;
    const std::shared_ptr<AidlConstantValue>& param = name_and_param.second;
    auto parameter_mapping_it = supported_params.find(param_name);
    if (parameter_mapping_it == supported_params.end()) {
      std::ostringstream stream;
      stream << "Parameter " << param_name << " not supported ";
      stream << "for annotation " << GetName() << ".";
      stream << "It must be one of:";
      for (const auto& kv : supported_params) {
        stream << " " << kv.first;
      }
      AIDL_ERROR(this) << stream.str();
      return false;
    }
    AidlTypeSpecifier type{AIDL_LOCATION_HERE, parameter_mapping_it->second, false, nullptr, ""};
    const std::string param_value = param->As(type, AidlConstantValueDecorator);
    // Assume error on empty string.
    if (param_value == "") {
      AIDL_ERROR(this) << "Invalid value for parameter " << param_name << " on annotation "
                       << GetName() << ".";
      return false;
    }
  }
  return true;
}

std::map<std::string, std::string> AidlAnnotation::AnnotationParams(
    const ConstantValueDecorator& decorator) const {
  std::map<std::string, std::string> raw_params;
  const auto& supported_params = kAnnotationParameters.at(GetName());
  for (const auto& name_and_param : parameters_) {
    const std::string& param_name = name_and_param.first;
    const std::shared_ptr<AidlConstantValue>& param = name_and_param.second;
    AidlTypeSpecifier type{AIDL_LOCATION_HERE, supported_params.at(param_name), false, nullptr, ""};
    raw_params.emplace(param_name, param->As(type, decorator));
  }
  return raw_params;
}

std::string AidlAnnotation::ToString(const ConstantValueDecorator& decorator) const {
  if (parameters_.empty()) {
    return "@" + GetName();
  } else {
    vector<string> param_strings;
    for (const auto& [name, value] : AnnotationParams(decorator)) {
      param_strings.emplace_back(name + "=" + value);
    }
    return "@" + GetName() + "(" + Join(param_strings, ", ") + ")";
  }
}

static bool HasAnnotation(const vector<AidlAnnotation>& annotations, const string& name) {
  for (const auto& a : annotations) {
    if (a.GetName() == name) {
      return true;
    }
  }
  return false;
}

static const AidlAnnotation* GetAnnotation(const vector<AidlAnnotation>& annotations,
                                           const string& name) {
  for (const auto& a : annotations) {
    if (a.GetName() == name) {
      return &a;
    }
  }
  return nullptr;
}

AidlAnnotatable::AidlAnnotatable(const AidlLocation& location) : AidlNode(location) {}

bool AidlAnnotatable::IsNullable() const {
  return HasAnnotation(annotations_, kNullable);
}

bool AidlAnnotatable::IsUtf8InCpp() const {
  return HasAnnotation(annotations_, kUtf8InCpp);
}

bool AidlAnnotatable::IsVintfStability() const {
  return HasAnnotation(annotations_, kVintfStability);
}

const AidlAnnotation* AidlAnnotatable::UnsupportedAppUsage() const {
  return GetAnnotation(annotations_, kUnsupportedAppUsage);
}

const AidlTypeSpecifier* AidlAnnotatable::BackingType(const AidlTypenames& typenames) const {
  auto annotation = GetAnnotation(annotations_, kBacking);
  if (annotation != nullptr) {
    auto annotation_params = annotation->AnnotationParams(AidlConstantValueDecorator);
    if (auto it = annotation_params.find("type"); it != annotation_params.end()) {
      const string& type = it->second;
      AidlTypeSpecifier* type_specifier =
          new AidlTypeSpecifier(AIDL_LOCATION_HERE,
                                // Strip the quotes off the type String.
                                type.substr(1, type.length() - 2), false, nullptr, "");
      type_specifier->Resolve(typenames);
      return type_specifier;
    }
  }
  return nullptr;
}

bool AidlAnnotatable::IsSystemApi() const {
  return HasAnnotation(annotations_, kSystemApi);
}

bool AidlAnnotatable::IsStableParcelable() const {
  return HasAnnotation(annotations_, kStableParcelable);
}

bool AidlAnnotatable::CheckValidAnnotations() const {
  for (const auto& annotation : GetAnnotations()) {
    if (!annotation.CheckValid()) {
      return false;
    }
  }

  return true;
}

string AidlAnnotatable::ToString() const {
  vector<string> ret;
  for (const auto& a : annotations_) {
    ret.emplace_back(a.ToString(AidlConstantValueDecorator));
  }
  std::sort(ret.begin(), ret.end());
  return Join(ret, " ");
}

AidlTypeSpecifier::AidlTypeSpecifier(const AidlLocation& location, const string& unresolved_name,
                                     bool is_array,
                                     vector<unique_ptr<AidlTypeSpecifier>>* type_params,
                                     const string& comments)
    : AidlAnnotatable(location),
      unresolved_name_(unresolved_name),
      is_array_(is_array),
      type_params_(type_params),
      comments_(comments),
      split_name_(Split(unresolved_name, ".")) {}

AidlTypeSpecifier AidlTypeSpecifier::ArrayBase() const {
  AIDL_FATAL_IF(!is_array_, this);

  AidlTypeSpecifier arrayBase = *this;
  arrayBase.is_array_ = false;
  return arrayBase;
}

string AidlTypeSpecifier::ToString() const {
  string ret = GetName();
  if (IsGeneric()) {
    vector<string> arg_names;
    for (const auto& ta : GetTypeParameters()) {
      arg_names.emplace_back(ta->ToString());
    }
    ret += "<" + Join(arg_names, ",") + ">";
  }
  if (IsArray()) {
    ret += "[]";
  }
  return ret;
}

string AidlTypeSpecifier::Signature() const {
  string ret = ToString();
  string annotations = AidlAnnotatable::ToString();
  if (annotations != "") {
    ret = annotations + " " + ret;
  }
  return ret;
}

bool AidlTypeSpecifier::Resolve(const AidlTypenames& typenames) {
  CHECK(!IsResolved());
  pair<string, bool> result = typenames.ResolveTypename(unresolved_name_);
  if (result.second) {
    fully_qualified_name_ = result.first;
    split_name_ = Split(fully_qualified_name_, ".");
  }
  return result.second;
}

bool AidlTypeSpecifier::CheckValid(const AidlTypenames& typenames) const {
  if (!CheckValidAnnotations()) {
    return false;
  }
  if (IsGeneric()) {
    const string& type_name = GetName();
    const int num = GetTypeParameters().size();
    if (type_name == "List") {
      if (num > 1) {
        AIDL_ERROR(this) << " List cannot have type parameters more than one, but got "
                         << "'" << ToString() << "'";
        return false;
      }
    } else if (type_name == "Map") {
      if (num != 0 && num != 2) {
        AIDL_ERROR(this) << "Map must have 0 or 2 type parameters, but got "
                         << "'" << ToString() << "'";
        return false;
      }
    }
  }

  if (GetName() == "void") {
    if (IsArray() || IsNullable() || IsUtf8InCpp()) {
      AIDL_ERROR(this) << "void type cannot be an array or nullable or utf8 string";
      return false;
    }
  }

  if (IsArray()) {
    const auto definedType = typenames.TryGetDefinedType(GetName());
    if (definedType != nullptr && definedType->AsInterface() != nullptr) {
      AIDL_ERROR(this) << "Binder type cannot be an array";
      return false;
    }
    if (definedType != nullptr && definedType->AsEnumDeclaration() != nullptr) {
      // TODO(b/123321528): Support arrays of enums.
      AIDL_ERROR(this) << "Enum type cannot be an array";
      return false;
    }
  }

  if (IsNullable()) {
    if (AidlTypenames::IsPrimitiveTypename(GetName()) && !IsArray()) {
      AIDL_ERROR(this) << "Primitive type cannot get nullable annotation";
      return false;
    }
  }
  return true;
}

std::string AidlConstantValueDecorator(const AidlTypeSpecifier& /*type*/,
                                       const std::string& raw_value) {
  return raw_value;
}

AidlVariableDeclaration::AidlVariableDeclaration(const AidlLocation& location,
                                                 AidlTypeSpecifier* type, const std::string& name)
    : AidlVariableDeclaration(location, type, name, nullptr /*default_value*/) {}

AidlVariableDeclaration::AidlVariableDeclaration(const AidlLocation& location,
                                                 AidlTypeSpecifier* type, const std::string& name,
                                                 AidlConstantValue* default_value)
    : AidlNode(location), type_(type), name_(name), default_value_(default_value) {}

bool AidlVariableDeclaration::CheckValid(const AidlTypenames& typenames) const {
  bool valid = true;
  valid &= type_->CheckValid(typenames);

  if (default_value_ == nullptr) return valid;
  valid &= default_value_->CheckValid();

  if (!valid) return false;

  return !ValueString(AidlConstantValueDecorator).empty();
}

string AidlVariableDeclaration::ToString() const {
  string ret = type_->Signature() + " " + name_;
  if (default_value_ != nullptr) {
    ret += " = " + ValueString(AidlConstantValueDecorator);
  }
  return ret;
}

string AidlVariableDeclaration::Signature() const {
  return type_->Signature() + " " + name_;
}

std::string AidlVariableDeclaration::ValueString(const ConstantValueDecorator& decorator) const {
  if (default_value_ != nullptr) {
    return GetDefaultValue()->As(GetType(), decorator);
  } else {
    return "";
  }
}

AidlArgument::AidlArgument(const AidlLocation& location, AidlArgument::Direction direction,
                           AidlTypeSpecifier* type, const std::string& name)
    : AidlVariableDeclaration(location, type, name),
      direction_(direction),
      direction_specified_(true) {}

AidlArgument::AidlArgument(const AidlLocation& location, AidlTypeSpecifier* type,
                           const std::string& name)
    : AidlVariableDeclaration(location, type, name),
      direction_(AidlArgument::IN_DIR),
      direction_specified_(false) {}

string AidlArgument::GetDirectionSpecifier() const {
  string ret;
  if (direction_specified_) {
    switch(direction_) {
    case AidlArgument::IN_DIR:
      ret += "in ";
      break;
    case AidlArgument::OUT_DIR:
      ret += "out ";
      break;
    case AidlArgument::INOUT_DIR:
      ret += "inout ";
      break;
    }
  }
  return ret;
}

string AidlArgument::ToString() const {
  return GetDirectionSpecifier() + AidlVariableDeclaration::ToString();
}

std::string AidlArgument::Signature() const {
  class AidlInterface;
  class AidlInterface;
  class AidlParcelable;
  class AidlStructuredParcelable;
  class AidlParcelable;
  class AidlStructuredParcelable;
  return GetDirectionSpecifier() + AidlVariableDeclaration::Signature();
}

AidlMember::AidlMember(const AidlLocation& location) : AidlNode(location) {}

AidlConstantValue::AidlConstantValue(const AidlLocation& location, Type type,
                                     const std::string& checked_value)
    : AidlNode(location), type_(type), value_(checked_value) {
  CHECK(!value_.empty() || type_ == Type::ERROR);
  CHECK(type_ != Type::ARRAY);
}

AidlConstantValue::AidlConstantValue(const AidlLocation& location, Type type,
                                     std::vector<std::unique_ptr<AidlConstantValue>>* values)
    : AidlNode(location), type_(type), values_(std::move(*values)) {}

static bool isValidLiteralChar(char c) {
  return !(c <= 0x1f ||  // control characters are < 0x20
           c >= 0x7f ||  // DEL is 0x7f
           c == '\\');   // Disallow backslashes for future proofing.
}

AidlConstantValue* AidlConstantValue::Boolean(const AidlLocation& location, bool value) {
  return new AidlConstantValue(location, Type::BOOLEAN, value ? "true" : "false");
}

AidlConstantValue* AidlConstantValue::Character(const AidlLocation& location, char value) {
  if (!isValidLiteralChar(value)) {
    AIDL_ERROR(location) << "Invalid character literal " << value;
    return new AidlConstantValue(location, Type::ERROR, "");
  }
  return new AidlConstantValue(location, Type::CHARACTER, std::string("'") + value + "'");
}

AidlConstantValue* AidlConstantValue::Floating(const AidlLocation& location,
                                               const std::string& value) {
  return new AidlConstantValue(location, Type::FLOATING, value);
}

AidlConstantValue* AidlConstantValue::Hex(const AidlLocation& location, const std::string& value) {
  return new AidlConstantValue(location, Type::HEXIDECIMAL, value);
}

AidlConstantValue* AidlConstantValue::Integral(const AidlLocation& location,
                                               const std::string& value) {
  return new AidlConstantValue(location, Type::INTEGRAL, value);
}

AidlConstantValue* AidlConstantValue::Array(
    const AidlLocation& location, std::vector<std::unique_ptr<AidlConstantValue>>* values) {
  return new AidlConstantValue(location, Type::ARRAY, values);
}

AidlConstantValue* AidlConstantValue::String(const AidlLocation& location,
                                             const std::string& value) {
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isValidLiteralChar(value[i])) {
      AIDL_ERROR(location) << "Found invalid character at index " << i << " in string constant '"
                           << value << "'";
      return new AidlConstantValue(location, Type::ERROR, "");
    }
  }

  return new AidlConstantValue(location, Type::STRING, value);
}

bool AidlConstantValue::CheckValid() const {
  // error always logged during creation
  return type_ != AidlConstantValue::Type::ERROR;
}

static string TrimIfSuffix(const string& str, const string& suffix) {
  if (str.size() > suffix.size() &&
      0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix)) {
    return str.substr(0, str.size() - suffix.size());
  }
  return str;
}

string AidlConstantValue::As(const AidlTypeSpecifier& type,
                             const ConstantValueDecorator& decorator) const {
  if (type.IsGeneric()) {
    AIDL_ERROR(type) << "Generic type cannot be specified with a constant literal.";
    return "";
  }

  const std::string& type_string = type.GetName();

  if ((type_ == Type::ARRAY) != type.IsArray()) {
    goto mismatch_error;
  }

  switch (type_) {
    case AidlConstantValue::Type::ARRAY: {
      vector<string> raw_values;
      raw_values.reserve(values_.size());

      bool success = true;
      for (const auto& value : values_) {
        const AidlTypeSpecifier& array_base = type.ArrayBase();
        const std::string raw_value = value->As(array_base, decorator);

        success &= !raw_value.empty();
        raw_values.push_back(decorator(array_base, raw_value));
      }
      if (!success) {
        AIDL_ERROR(this) << "Default value must be a literal array of " << type_string << ".";
        return "";
      }
      return decorator(type, "{" + Join(raw_values, ", ") + "}");
    }
    case AidlConstantValue::Type::BOOLEAN:
      if (type_string == "boolean") return decorator(type, value_);
      goto mismatch_error;
    case AidlConstantValue::Type::CHARACTER:
      if (type_string == "char") return decorator(type, value_);
      goto mismatch_error;
    case AidlConstantValue::Type::FLOATING: {
      bool is_float_literal = value_.back() == 'f';
      const std::string raw_value = TrimIfSuffix(value_, "f");

      if (type_string == "double") {
        double parsed_value;
        if (!android::base::ParseDouble(raw_value, &parsed_value)) goto parse_error;
        return decorator(type, std::to_string(parsed_value));
      }
      if (is_float_literal && type_string == "float") {
        float parsed_value;
        if (!android::base::ParseFloat(raw_value, &parsed_value)) goto parse_error;
        return decorator(type, std::to_string(parsed_value) + "f");
      }
      goto mismatch_error;
    }
    case AidlConstantValue::Type::HEXIDECIMAL:
      // For historical reasons, a hexidecimal int needs to have the specified bits interpreted
      // as the signed type, so the other types are made consistent with it.
      if (type_string == "byte") {
        uint8_t unsigned_value;
        if (!android::base::ParseUint<uint8_t>(value_, &unsigned_value)) goto parse_error;
        return decorator(type, std::to_string((int8_t)unsigned_value));
      }
      if (type_string == "int") {
        uint32_t unsigned_value;
        if (!android::base::ParseUint<uint32_t>(value_, &unsigned_value)) goto parse_error;
        return decorator(type, std::to_string((int32_t)unsigned_value));
      }
      if (type_string == "long") {
        uint64_t unsigned_value;
        if (!android::base::ParseUint<uint64_t>(value_, &unsigned_value)) goto parse_error;
        return decorator(type, std::to_string((int64_t)unsigned_value));
      }
      goto mismatch_error;
    case AidlConstantValue::Type::INTEGRAL:
      if (type_string == "byte") {
        if (!android::base::ParseInt<int8_t>(value_, nullptr)) goto parse_error;
        return decorator(type, value_);
      }
      if (type_string == "int") {
        if (!android::base::ParseInt<int32_t>(value_, nullptr)) goto parse_error;
        return decorator(type, value_);
      }
      if (type_string == "long") {
        if (!android::base::ParseInt<int64_t>(value_, nullptr)) goto parse_error;
        return decorator(type, value_);
      }
      goto mismatch_error;
    case AidlConstantValue::Type::STRING:
      if (type_string == "String") return decorator(type, value_);
      goto mismatch_error;
    default:
      AIDL_FATAL(this) << "Unrecognized constant value type";
  }

mismatch_error:
  AIDL_ERROR(this) << "Expecting type " << type_string << " but constant is " << ToString(type_);
  return "";
parse_error:
  AIDL_ERROR(this) << "Could not parse " << value_ << " as " << type_string;
  return "";
}

string AidlConstantValue::ToString(Type type) {
  switch (type) {
    case Type::ARRAY:
      return "a literal array";
    case Type::BOOLEAN:
      return "a literal boolean";
    case Type::CHARACTER:
      return "a literal char";
    case Type::FLOATING:
      return "a floating-point literal";
    case Type::HEXIDECIMAL:
      return "a hexidecimal literal";
    case Type::INTEGRAL:
      return "an integral literal";
    case Type::STRING:
      return "a literal string";
    case Type::ERROR:
      LOG(FATAL) << "aidl internal error: error type failed to halt program";
      return "";
    default:
      LOG(FATAL) << "aidl internal error: unknown constant type: " << static_cast<int>(type);
      return "";  // not reached
  }
}

AidlConstantDeclaration::AidlConstantDeclaration(const AidlLocation& location,
                                                 AidlTypeSpecifier* type, const std::string& name,
                                                 AidlConstantValue* value)
    : AidlMember(location), type_(type), name_(name), value_(value) {}

bool AidlConstantDeclaration::CheckValid(const AidlTypenames& typenames) const {
  bool valid = true;
  valid &= type_->CheckValid(typenames);
  valid &= value_->CheckValid();
  if (!valid) return false;

  const static set<string> kSupportedConstTypes = {"String", "int"};
  if (kSupportedConstTypes.find(type_->ToString()) == kSupportedConstTypes.end()) {
    AIDL_ERROR(this) << "Constant of type " << type_->ToString() << " is not supported.";
    return false;
  }

  return !ValueString(AidlConstantValueDecorator).empty();
}

string AidlConstantDeclaration::ToString() const {
  return "const " + type_->ToString() + " " + name_ + " = " +
         ValueString(AidlConstantValueDecorator);
}

string AidlConstantDeclaration::Signature() const {
  return type_->Signature() + " " + name_;
}

AidlMethod::AidlMethod(const AidlLocation& location, bool oneway, AidlTypeSpecifier* type,
                       const std::string& name, std::vector<std::unique_ptr<AidlArgument>>* args,
                       const std::string& comments)
    : AidlMethod(location, oneway, type, name, args, comments, 0, true) {
  has_id_ = false;
}

AidlMethod::AidlMethod(const AidlLocation& location, bool oneway, AidlTypeSpecifier* type,
                       const std::string& name, std::vector<std::unique_ptr<AidlArgument>>* args,
                       const std::string& comments, int id, bool is_user_defined)
    : AidlMember(location),
      oneway_(oneway),
      comments_(comments),
      type_(type),
      name_(name),
      arguments_(std::move(*args)),
      id_(id),
      is_user_defined_(is_user_defined) {
  has_id_ = true;
  delete args;
  for (const unique_ptr<AidlArgument>& a : arguments_) {
    if (a->IsIn()) { in_arguments_.push_back(a.get()); }
    if (a->IsOut()) { out_arguments_.push_back(a.get()); }
  }
}


string AidlMethod::Signature() const {
  vector<string> arg_signatures;
  for (const auto& arg : GetArguments()) {
    arg_signatures.emplace_back(arg->GetType().ToString());
  }
  return GetName() + "(" + Join(arg_signatures, ", ") + ")";
}

string AidlMethod::ToString() const {
  vector<string> arg_strings;
  for (const auto& arg : GetArguments()) {
    arg_strings.emplace_back(arg->Signature());
  }
  string ret = (IsOneway() ? "oneway " : "") + GetType().Signature() + " " + GetName() + "(" +
               Join(arg_strings, ", ") + ")";
  if (HasId()) {
    ret += " = " + std::to_string(GetId());
  }
  return ret;
}

AidlDefinedType::AidlDefinedType(const AidlLocation& location, const std::string& name,
                                 const std::string& comments,
                                 const std::vector<std::string>& package)
    : AidlAnnotatable(location), name_(name), comments_(comments), package_(package) {}

std::string AidlDefinedType::GetPackage() const {
  return Join(package_, '.');
}

std::string AidlDefinedType::GetCanonicalName() const {
  if (package_.empty()) {
    return GetName();
  }
  return GetPackage() + "." + GetName();
}

AidlParcelable::AidlParcelable(const AidlLocation& location, AidlQualifiedName* name,
                               const std::vector<std::string>& package, const std::string& comments,
                               const std::string& cpp_header)
    : AidlDefinedType(location, name->GetDotName(), comments, package),
      name_(name),
      cpp_header_(cpp_header) {
  // Strip off quotation marks if we actually have a cpp header.
  if (cpp_header_.length() >= 2) {
    cpp_header_ = cpp_header_.substr(1, cpp_header_.length() - 2);
  }
}

bool AidlParcelable::CheckValid(const AidlTypenames&) const {
  static const std::set<string> allowed{kStableParcelable};
  if (!CheckValidAnnotations()) {
    return false;
  }
  for (const auto& v : GetAnnotations()) {
    if (allowed.find(v.GetName()) == allowed.end()) {
      std::ostringstream stream;
      stream << "Unstructured parcelable can contain only";
      for (const string& kv : allowed) {
        stream << " " << kv;
      }
      stream << ".";
      AIDL_ERROR(this) << stream.str();
      return false;
    }
  }

  return true;
}

void AidlParcelable::Write(CodeWriter* writer) const {
  writer->Write("parcelable %s ;\n", GetName().c_str());
}

AidlStructuredParcelable::AidlStructuredParcelable(
    const AidlLocation& location, AidlQualifiedName* name, const std::vector<std::string>& package,
    const std::string& comments, std::vector<std::unique_ptr<AidlVariableDeclaration>>* variables)
    : AidlParcelable(location, name, package, comments, "" /*cpp_header*/),
      variables_(std::move(*variables)) {}

void AidlStructuredParcelable::Write(CodeWriter* writer) const {
  writer->Write("parcelable %s {\n", GetName().c_str());
  writer->Indent();
  for (const auto& field : GetFields()) {
    writer->Write("%s;\n", field->ToString().c_str());
  }
  writer->Dedent();
  writer->Write("}\n");
}

bool AidlStructuredParcelable::CheckValid(const AidlTypenames& typenames) const {
  bool success = true;
  for (const auto& v : GetFields()) {
    success = success && v->CheckValid(typenames);
  }
  return success;
}

// TODO: we should treat every backend all the same in future.
bool AidlTypeSpecifier::LanguageSpecificCheckValid(Options::Language lang) const {
  if (lang == Options::Language::CPP) {
    if (this->GetName() == "List" && !this->IsGeneric()) {
      AIDL_ERROR(this) << "List without type isn't supported in cpp.";
      return false;
    }
  }
  if (this->IsGeneric()) {
    if (this->GetName() == "List") {
      if (this->GetTypeParameters().size() != 1) {
        AIDL_ERROR(this) << "List must have only one type parameter.";
        return false;
      }
      if (lang == Options::Language::CPP) {
        auto& name = this->GetTypeParameters()[0]->GetName();
        if (!(name == "String" || name == "IBinder")) {
          AIDL_ERROR(this) << "List in cpp supports only string and IBinder for now.";
          return false;
        }
      } else if (lang == Options::Language::NDK) {
        AIDL_ERROR(this) << "NDK backend does not support List yet.";
        return false;
      }

    } else if (this->GetName() == "Map") {
      if (lang != Options::Language::JAVA) {
        AIDL_ERROR(this) << "Currently, only Java backend supports Map.";
        return false;
      }
    }
  }
  return true;
}

// TODO: we should treat every backend all the same in future.
bool AidlParcelable::LanguageSpecificCheckValid(Options::Language lang) const {
  if (lang != Options::Language::JAVA) {
    if (this->IsStableParcelable()) {
      AIDL_ERROR(this) << "@JavaOnlyStableParcelable supports only Java target.";
      return false;
    }
    const AidlParcelable* unstructuredParcelable = this->AsUnstructuredParcelable();
    if (unstructuredParcelable != nullptr) {
      if (unstructuredParcelable->GetCppHeader().empty()) {
        AIDL_ERROR(unstructuredParcelable)
            << "Unstructured parcelable must have C++ header defined.";
        return false;
      }
    }
  }
  return true;
}

// TODO: we should treat every backend all the same in future.
bool AidlStructuredParcelable::LanguageSpecificCheckValid(Options::Language lang) const {
  if (!AidlParcelable::LanguageSpecificCheckValid(lang)) {
    return false;
  }
  for (const auto& v : this->GetFields()) {
    if (!v->GetType().LanguageSpecificCheckValid(lang)) {
      return false;
    }
  }
  return true;
}

AidlEnumerator::AidlEnumerator(const AidlLocation& location, const std::string& name,
                               AidlConstantValue* value)
    : AidlNode(location), name_(name), value_(value) {}

bool AidlEnumerator::CheckValid(const AidlTypeSpecifier& enum_backing_type) const {
  if (GetValue() == nullptr) {
    return false;
  }
  if (!GetValue()->CheckValid()) {
    return false;
  }
  if (GetValue()->As(enum_backing_type, AidlConstantValueDecorator).empty()) {
    AIDL_ERROR(this) << "Enumerator type differs from enum backing type.";
    return false;
  }
  return true;
}

string AidlEnumerator::ValueString(const AidlTypeSpecifier& backing_type,
                                   const ConstantValueDecorator& decorator) const {
  return GetValue()->As(backing_type, decorator);
}

AidlEnumDeclaration::AidlEnumDeclaration(const AidlLocation& location, const std::string& name,
                                         std::vector<std::unique_ptr<AidlEnumerator>>* enumerators,
                                         const std::vector<std::string>& package)
    : AidlDefinedType(location, name, "", package), enumerators_(std::move(*enumerators)) {}

void AidlEnumDeclaration::SetBackingType(std::unique_ptr<const AidlTypeSpecifier> type) {
  backing_type_ = std::move(type);
}

bool AidlEnumDeclaration::CheckValid(const AidlTypenames&) const {
  if (backing_type_ == nullptr) {
    AIDL_ERROR(this) << "Enum declaration missing backing type.";
    return false;
  }
  bool success = true;
  for (const auto& enumerator : enumerators_) {
    success = success && enumerator->CheckValid(GetBackingType());
  }
  return success;
}

void AidlEnumDeclaration::Write(CodeWriter* writer) const {
  writer->Write("%s\n", AidlAnnotatable::ToString().c_str());
  writer->Write("enum %s {\n", GetName().c_str());
  writer->Indent();
  for (const auto& enumerator : GetEnumerators()) {
    // TODO(b/123321528): After autofilling is supported, determine if we want
    // to leave out the assigned value for enumerators that were autofilled.
    writer->Write("%s = %s,\n", enumerator->GetName().c_str(),
                  enumerator->ValueString(GetBackingType(), AidlConstantValueDecorator).c_str());
  }
  writer->Dedent();
  writer->Write("}\n");
}

// TODO: we should treat every backend all the same in future.
bool AidlInterface::LanguageSpecificCheckValid(Options::Language lang) const {
  for (const auto& m : this->GetMethods()) {
    if (!m->GetType().LanguageSpecificCheckValid(lang)) {
      return false;
    }
    for (const auto& arg : m->GetArguments()) {
      if (!arg->GetType().LanguageSpecificCheckValid(lang)) {
        return false;
      }
    }
  }
  return true;
}

AidlInterface::AidlInterface(const AidlLocation& location, const std::string& name,
                             const std::string& comments, bool oneway,
                             std::vector<std::unique_ptr<AidlMember>>* members,
                             const std::vector<std::string>& package)
    : AidlDefinedType(location, name, comments, package) {
  for (auto& member : *members) {
    AidlMember* local = member.release();
    AidlMethod* method = local->AsMethod();
    AidlConstantDeclaration* constant = local->AsConstantDeclaration();

    CHECK(method == nullptr || constant == nullptr);

    if (method) {
      method->ApplyInterfaceOneway(oneway);
      methods_.emplace_back(method);
    } else if (constant) {
      constants_.emplace_back(constant);
    } else {
      AIDL_FATAL(this) << "Member is neither method nor constant!";
    }
  }

  delete members;
}

void AidlInterface::Write(CodeWriter* writer) const {
  writer->Write("interface %s {\n", GetName().c_str());
  writer->Indent();
  for (const auto& method : GetMethods()) {
    writer->Write("%s;\n", method->ToString().c_str());
  }
  for (const auto& constdecl : GetConstantDeclarations()) {
    writer->Write("%s;\n", constdecl->ToString().c_str());
  }
  writer->Dedent();
  writer->Write("}\n");
}

bool AidlInterface::CheckValid(const AidlTypenames& typenames) const {
  if (!CheckValidAnnotations()) {
    return false;
  }
  // Has to be a pointer due to deleting copy constructor. No idea why.
  map<string, const AidlMethod*> method_names;
  for (const auto& m : GetMethods()) {
    if (!m->GetType().CheckValid(typenames)) {
      return false;
    }

    if (m->IsOneway() && m->GetType().GetName() != "void") {
      AIDL_ERROR(m) << "oneway method '" << m->GetName() << "' cannot return a value";
      return false;
    }

    set<string> argument_names;
    for (const auto& arg : m->GetArguments()) {
      auto it = argument_names.find(arg->GetName());
      if (it != argument_names.end()) {
        AIDL_ERROR(m) << "method '" << m->GetName() << "' has duplicate argument name '"
                      << arg->GetName() << "'";
        return false;
      }
      argument_names.insert(arg->GetName());

      if (!arg->GetType().CheckValid(typenames)) {
        return false;
      }

      if (m->IsOneway() && arg->IsOut()) {
        AIDL_ERROR(m) << "oneway method '" << m->GetName() << "' cannot have out parameters";
        return false;
      }
      const bool can_be_out = typenames.CanBeOutParameter(arg->GetType());
      if (!arg->DirectionWasSpecified() && can_be_out) {
        AIDL_ERROR(arg) << "'" << arg->GetType().ToString()
                        << "' can be an out type, so you must declare it as in, out, or inout.";
        return false;
      }

      if (arg->GetDirection() != AidlArgument::IN_DIR && !can_be_out) {
        AIDL_ERROR(arg) << "'" << arg->ToString() << "' can only be an in parameter.";
        return false;
      }

      // check that the name doesn't match a keyword
      if (is_java_keyword(arg->GetName().c_str())) {
        AIDL_ERROR(arg) << "Argument name is a Java or aidl keyword";
        return false;
      }

      // Reserve a namespace for internal use
      if (android::base::StartsWith(arg->GetName(), "_aidl")) {
        AIDL_ERROR(arg) << "Argument name cannot begin with '_aidl'";
        return false;
      }
    }

    auto it = method_names.find(m->GetName());
    // prevent duplicate methods
    if (it == method_names.end()) {
      method_names[m->GetName()] = m.get();
    } else {
      AIDL_ERROR(m) << "attempt to redefine method " << m->GetName() << ":";
      AIDL_ERROR(it->second) << "previously defined here.";
      return false;
    }

    static set<string> reserved_methods{"asBinder()", "getInterfaceVersion()",
                                        "getTransactionName(int)"};

    if (reserved_methods.find(m->Signature()) != reserved_methods.end()) {
      AIDL_ERROR(m) << " method " << m->Signature() << " is reserved for internal use." << endl;
      return false;
    }
  }

  bool success = true;
  set<string> constant_names;
  for (const std::unique_ptr<AidlConstantDeclaration>& constant : GetConstantDeclarations()) {
    if (constant_names.count(constant->GetName()) > 0) {
      LOG(ERROR) << "Found duplicate constant name '" << constant->GetName() << "'";
      success = false;
    }
    constant_names.insert(constant->GetName());
    success = success && constant->CheckValid(typenames);
  }

  return success;
}

AidlQualifiedName::AidlQualifiedName(const AidlLocation& location, const std::string& term,
                                     const std::string& comments)
    : AidlNode(location), terms_({term}), comments_(comments) {
  if (term.find('.') != string::npos) {
    terms_ = Split(term, ".");
    for (const auto& subterm : terms_) {
      if (subterm.empty()) {
        AIDL_FATAL(this) << "Malformed qualified identifier: '" << term << "'";
      }
    }
  }
}

void AidlQualifiedName::AddTerm(const std::string& term) {
  terms_.push_back(term);
}

AidlImport::AidlImport(const AidlLocation& location, const std::string& needed_class)
    : AidlNode(location), needed_class_(needed_class) {}

std::unique_ptr<Parser> Parser::Parse(const std::string& filename,
                                      const android::aidl::IoDelegate& io_delegate,
                                      AidlTypenames& typenames) {
  // Make sure we can read the file first, before trashing previous state.
  unique_ptr<string> raw_buffer = io_delegate.GetFileContents(filename);
  if (raw_buffer == nullptr) {
    AIDL_ERROR(filename) << "Error while opening file for parsing";
    return nullptr;
  }

  // We're going to scan this buffer in place, and yacc demands we put two
  // nulls at the end.
  raw_buffer->append(2u, '\0');

  std::unique_ptr<Parser> parser(new Parser(filename, *raw_buffer, typenames));

  if (yy::parser(parser.get()).parse() != 0 || parser->HasError()) return nullptr;

  return parser;
}

std::vector<std::string> Parser::Package() const {
  if (!package_) {
    return {};
  }
  return package_->GetTerms();
}

void Parser::AddImport(AidlImport* import) {
  imports_.emplace_back(import);
}

bool Parser::Resolve() {
  bool success = true;
  for (AidlTypeSpecifier* typespec : unresolved_typespecs_) {
    if (!typespec->Resolve(typenames_)) {
      AIDL_ERROR(typespec) << "Failed to resolve '" << typespec->GetUnresolvedName() << "'";
      success = false;
      // don't stop to show more errors if any
    }
  }
  return success;
}

Parser::Parser(const std::string& filename, std::string& raw_buffer,
               android::aidl::AidlTypenames& typenames)
    : filename_(filename), typenames_(typenames) {
  yylex_init(&scanner_);
  buffer_ = yy_scan_buffer(&raw_buffer[0], raw_buffer.length(), scanner_);
}

Parser::~Parser() {
  yy_delete_buffer(buffer_, scanner_);
  yylex_destroy(scanner_);
}
