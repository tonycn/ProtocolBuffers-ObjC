// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// http://code.google.com/p/protobuf/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Author: robinson@google.com (Will Robinson)
//
// This module outputs pure-Python protocol message classes that will
// largely be constructed at runtime via the metaclass in reflection.py.
// In other words, our job is basically to output a Python equivalent
// of the C++ *Descriptor objects, and fix up all circular references
// within these objects.
//
// Note that the runtime performance of protocol message classes created in
// this way is expected to be lousy.  The plan is to create an alternate
// generator that outputs a Python/C extension module that lets
// performance-minded Python code leverage the fast C++ implementation
// directly.

#include <utility>
#include <map>
#include <string>
#include <vector>

#include <google/protobuf/compiler/python/python_generator.h>
#include <google/protobuf/descriptor.pb.h>

#include <google/protobuf/stubs/common.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/stubs/strutil.h>
#include <google/protobuf/stubs/substitute.h>

namespace google {
namespace protobuf {
namespace compiler {
namespace python {

namespace {

// Returns a copy of |filename| with any trailing ".protodevel" or ".proto
// suffix stripped.
// TODO(robinson): Unify with copy in compiler/cpp/internal/helpers.cc.
string StripProto(const string& filename) {
  const char* suffix = HasSuffixString(filename, ".protodevel")
      ? ".protodevel" : ".proto";
  return StripSuffixString(filename, suffix);
}


// Returns the Python module name expected for a given .proto filename.
string ModuleName(const string& filename) {
  string basename = StripProto(filename);
  StripString(&basename, "-", '_');
  StripString(&basename, "/", '.');
  return basename + "_pb2";
}


// Returns the name of all containing types for descriptor,
// in order from outermost to innermost, followed by descriptor's
// own name.  Each name is separated by |separator|.
template <typename DescriptorT>
string NamePrefixedWithNestedTypes(const DescriptorT& descriptor,
                                   const string& separator) {
  string name = descriptor.name();
  for (const Descriptor* current = descriptor.containing_type();
       current != NULL; current = current->containing_type()) {
    name = current->name() + separator + name;
  }
  return name;
}


// Name of the class attribute where we store the Python
// descriptor.Descriptor instance for the generated class.
// Must stay consistent with the _DESCRIPTOR_KEY constant
// in proto2/public/reflection.py.
const char kDescriptorKey[] = "DESCRIPTOR";


// Prints the common boilerplate needed at the top of every .py
// file output by this generator.
void PrintTopBoilerplate(
    io::Printer* printer, const FileDescriptor* file, bool descriptor_proto) {
  // TODO(robinson): Allow parameterization of Python version?
  printer->Print(
      "# Generated by the protocol buffer compiler.  DO NOT EDIT!\n"
      "\n"
      "from google.protobuf import descriptor\n"
      "from google.protobuf import message\n"
      "from google.protobuf import reflection\n"
      "from google.protobuf import service\n"
      "from google.protobuf import service_reflection\n");
  // Avoid circular imports if this module is descriptor_pb2.
  if (!descriptor_proto) {
    printer->Print(
        "from google.protobuf import descriptor_pb2\n");
  }
}


// Returns a Python literal giving the default value for a field.
// If the field specifies no explicit default value, we'll return
// the default default value for the field type (zero for numbers,
// empty string for strings, empty list for repeated fields, and
// None for non-repeated, composite fields).
//
// TODO(robinson): Unify with code from
// //compiler/cpp/internal/primitive_field.cc
// //compiler/cpp/internal/enum_field.cc
// //compiler/cpp/internal/string_field.cc
string StringifyDefaultValue(const FieldDescriptor& field) {
  if (field.is_repeated()) {
    return "[]";
  }

  switch (field.cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      return SimpleItoa(field.default_value_int32());
    case FieldDescriptor::CPPTYPE_UINT32:
      return SimpleItoa(field.default_value_uint32());
    case FieldDescriptor::CPPTYPE_INT64:
      return SimpleItoa(field.default_value_int64());
    case FieldDescriptor::CPPTYPE_UINT64:
      return SimpleItoa(field.default_value_uint64());
    case FieldDescriptor::CPPTYPE_DOUBLE:
      return SimpleDtoa(field.default_value_double());
    case FieldDescriptor::CPPTYPE_FLOAT:
      return SimpleFtoa(field.default_value_float());
    case FieldDescriptor::CPPTYPE_BOOL:
      return field.default_value_bool() ? "True" : "False";
    case FieldDescriptor::CPPTYPE_ENUM:
      return SimpleItoa(field.default_value_enum()->number());
    case FieldDescriptor::CPPTYPE_STRING:
      if (field.type() == FieldDescriptor::TYPE_STRING) {
        return "unicode(\"" + CEscape(field.default_value_string()) +
            "\", \"utf-8\")";
      } else {
        return "\"" + CEscape(field.default_value_string()) + "\"";
      }
    case FieldDescriptor::CPPTYPE_MESSAGE:
      return "None";
  }
  // (We could add a default case above but then we wouldn't get the nice
  // compiler warning when a new type is added.)
  GOOGLE_LOG(FATAL) << "Not reached.";
  return "";
}



}  // namespace


Generator::Generator() : file_(NULL) {
}

Generator::~Generator() {
}

bool Generator::Generate(const FileDescriptor* file,
                         const string& parameter,
                         OutputDirectory* output_directory,
                         string* error) const {

  // Completely serialize all Generate() calls on this instance.  The
  // thread-safety constraints of the CodeGenerator interface aren't clear so
  // just be as conservative as possible.  It's easier to relax this later if
  // we need to, but I doubt it will be an issue.
  // TODO(kenton):  The proper thing to do would be to allocate any state on
  //   the stack and use that, so that the Generator class itself does not need
  //   to have any mutable members.  Then it is implicitly thread-safe.
  MutexLock lock(&mutex_);
  file_ = file;
  string module_name = ModuleName(file->name());
  string filename = module_name;
  StripString(&filename, ".", '/');
  filename += ".py";


  scoped_ptr<io::ZeroCopyOutputStream> output(output_directory->Open(filename));
  GOOGLE_CHECK(output.get());
  io::Printer printer(output.get(), '$');
  printer_ = &printer;

  PrintTopBoilerplate(printer_, file_, GeneratingDescriptorProto());
  PrintTopLevelEnums();
  PrintTopLevelExtensions();
  PrintAllNestedEnumsInFile();
  PrintMessageDescriptors();
  // We have to print the imports after the descriptors, so that mutually
  // recursive protos in separate files can successfully reference each other.
  PrintImports();
  FixForeignFieldsInDescriptors();
  PrintMessages();
  // We have to fix up the extensions after the message classes themselves,
  // since they need to call static RegisterExtension() methods on these
  // classes.
  FixForeignFieldsInExtensions();
  PrintServices();
  return !printer.failed();
}

// Prints Python imports for all modules imported by |file|.
void Generator::PrintImports() const {
  for (int i = 0; i < file_->dependency_count(); ++i) {
    string module_name = ModuleName(file_->dependency(i)->name());
    printer_->Print("import $module$\n", "module",
                    module_name);
  }
  printer_->Print("\n");
}

// Prints descriptors and module-level constants for all top-level
// enums defined in |file|.
void Generator::PrintTopLevelEnums() const {
  vector<pair<string, int> > top_level_enum_values;
  for (int i = 0; i < file_->enum_type_count(); ++i) {
    const EnumDescriptor& enum_descriptor = *file_->enum_type(i);
    PrintEnum(enum_descriptor);
    printer_->Print("\n");

    for (int j = 0; j < enum_descriptor.value_count(); ++j) {
      const EnumValueDescriptor& value_descriptor = *enum_descriptor.value(j);
      top_level_enum_values.push_back(
          make_pair(value_descriptor.name(), value_descriptor.number()));
    }
  }

  for (int i = 0; i < top_level_enum_values.size(); ++i) {
    printer_->Print("$name$ = $value$\n",
                    "name", top_level_enum_values[i].first,
                    "value", SimpleItoa(top_level_enum_values[i].second));
  }
  printer_->Print("\n");
}

// Prints all enums contained in all message types in |file|.
void Generator::PrintAllNestedEnumsInFile() const {
  for (int i = 0; i < file_->message_type_count(); ++i) {
    PrintNestedEnums(*file_->message_type(i));
  }
}

// Prints a Python statement assigning the appropriate module-level
// enum name to a Python EnumDescriptor object equivalent to
// enum_descriptor.
void Generator::PrintEnum(const EnumDescriptor& enum_descriptor) const {
  map<string, string> m;
  m["descriptor_name"] = ModuleLevelDescriptorName(enum_descriptor);
  m["name"] = enum_descriptor.name();
  m["full_name"] = enum_descriptor.full_name();
  m["filename"] = enum_descriptor.name();
  const char enum_descriptor_template[] =
      "$descriptor_name$ = descriptor.EnumDescriptor(\n"
      "  name='$name$',\n"
      "  full_name='$full_name$',\n"
      "  filename='$filename$',\n"
      "  values=[\n";
  string options_string;
  enum_descriptor.options().SerializeToString(&options_string);
  printer_->Print(m, enum_descriptor_template);
  printer_->Indent();
  printer_->Indent();
  for (int i = 0; i < enum_descriptor.value_count(); ++i) {
    PrintEnumValueDescriptor(*enum_descriptor.value(i));
    printer_->Print(",\n");
  }
  printer_->Outdent();
  printer_->Print("],\n");
  printer_->Print("options=$options_value$,\n",
                  "options_value",
                  OptionsValue("EnumOptions", CEscape(options_string)));
  printer_->Outdent();
  printer_->Print(")\n");
  printer_->Print("\n");
}

// Recursively prints enums in nested types within descriptor, then
// prints enums contained at the top level in descriptor.
void Generator::PrintNestedEnums(const Descriptor& descriptor) const {
  for (int i = 0; i < descriptor.nested_type_count(); ++i) {
    PrintNestedEnums(*descriptor.nested_type(i));
  }

  for (int i = 0; i < descriptor.enum_type_count(); ++i) {
    PrintEnum(*descriptor.enum_type(i));
  }
}

void Generator::PrintTopLevelExtensions() const {
  const bool is_extension = true;
  for (int i = 0; i < file_->extension_count(); ++i) {
    const FieldDescriptor& extension_field = *file_->extension(i);
    string constant_name = extension_field.name() + "_FIELD_NUMBER";
    UpperString(&constant_name);
    printer_->Print("$constant_name$ = $number$\n",
      "constant_name", constant_name,
      "number", SimpleItoa(extension_field.number()));
    printer_->Print("$name$ = ", "name", extension_field.name());
    PrintFieldDescriptor(extension_field, is_extension);
    printer_->Print("\n");
  }
  printer_->Print("\n");
}

// Prints Python equivalents of all Descriptors in |file|.
void Generator::PrintMessageDescriptors() const {
  for (int i = 0; i < file_->message_type_count(); ++i) {
    PrintDescriptor(*file_->message_type(i));
    printer_->Print("\n");
  }
}

void Generator::PrintServices() const {
  for (int i = 0; i < file_->service_count(); ++i) {
    PrintServiceDescriptor(*file_->service(i));
    PrintServiceClass(*file_->service(i));
    PrintServiceStub(*file_->service(i));
    printer_->Print("\n");
  }
}

void Generator::PrintServiceDescriptor(
    const ServiceDescriptor& descriptor) const {
  printer_->Print("\n");
  string service_name = ModuleLevelServiceDescriptorName(descriptor);
  string options_string;
  descriptor.options().SerializeToString(&options_string);

  printer_->Print(
      "$service_name$ = descriptor.ServiceDescriptor(\n",
      "service_name", service_name);
  printer_->Indent();
  map<string, string> m;
  m["name"] = descriptor.name();
  m["full_name"] = descriptor.full_name();
  m["index"] = SimpleItoa(descriptor.index());
  m["options_value"] = OptionsValue("ServiceOptions", options_string);
  const char required_function_arguments[] =
      "name='$name$',\n"
      "full_name='$full_name$',\n"
      "index=$index$,\n"
      "options=$options_value$,\n"
      "methods=[\n";
  printer_->Print(m, required_function_arguments);
  for (int i = 0; i < descriptor.method_count(); ++i) {
    const MethodDescriptor* method = descriptor.method(i);
    string options_string;
    method->options().SerializeToString(&options_string);

    m.clear();
    m["name"] = method->name();
    m["full_name"] = method->full_name();
    m["index"] = SimpleItoa(method->index());
    m["serialized_options"] = CEscape(options_string);
    m["input_type"] = ModuleLevelDescriptorName(*(method->input_type()));
    m["output_type"] = ModuleLevelDescriptorName(*(method->output_type()));
    m["options_value"] = OptionsValue("MethodOptions", options_string);
    printer_->Print("descriptor.MethodDescriptor(\n");
    printer_->Indent();
    printer_->Print(
        m,
        "name='$name$',\n"
        "full_name='$full_name$',\n"
        "index=$index$,\n"
        "containing_service=None,\n"
        "input_type=$input_type$,\n"
        "output_type=$output_type$,\n"
        "options=$options_value$,\n");
    printer_->Outdent();
    printer_->Print("),\n");
  }

  printer_->Outdent();
  printer_->Print("])\n\n");
}

void Generator::PrintServiceClass(const ServiceDescriptor& descriptor) const {
  // Print the service.
  printer_->Print("class $class_name$(service.Service):\n",
                  "class_name", descriptor.name());
  printer_->Indent();
  printer_->Print(
      "__metaclass__ = service_reflection.GeneratedServiceType\n"
      "$descriptor_key$ = $descriptor_name$\n",
      "descriptor_key", kDescriptorKey,
      "descriptor_name", ModuleLevelServiceDescriptorName(descriptor));
  printer_->Outdent();
}

void Generator::PrintServiceStub(const ServiceDescriptor& descriptor) const {
  // Print the service stub.
  printer_->Print("class $class_name$_Stub($class_name$):\n",
                  "class_name", descriptor.name());
  printer_->Indent();
  printer_->Print(
      "__metaclass__ = service_reflection.GeneratedServiceStubType\n"
      "$descriptor_key$ = $descriptor_name$\n",
      "descriptor_key", kDescriptorKey,
      "descriptor_name", ModuleLevelServiceDescriptorName(descriptor));
  printer_->Outdent();
}

// Prints statement assigning ModuleLevelDescriptorName(message_descriptor)
// to a Python Descriptor object for message_descriptor.
//
// Mutually recursive with PrintNestedDescriptors().
void Generator::PrintDescriptor(const Descriptor& message_descriptor) const {
  PrintNestedDescriptors(message_descriptor);

  printer_->Print("\n");
  printer_->Print("$descriptor_name$ = descriptor.Descriptor(\n",
                  "descriptor_name",
                  ModuleLevelDescriptorName(message_descriptor));
  printer_->Indent();
  map<string, string> m;
  m["name"] = message_descriptor.name();
  m["full_name"] = message_descriptor.full_name();
  m["filename"] = message_descriptor.file()->name();
  const char required_function_arguments[] =
      "name='$name$',\n"
      "full_name='$full_name$',\n"
      "filename='$filename$',\n"
      "containing_type=None,\n";  // TODO(robinson): Implement containing_type.
  printer_->Print(m, required_function_arguments);
  PrintFieldsInDescriptor(message_descriptor);
  PrintExtensionsInDescriptor(message_descriptor);
  // TODO(robinson): implement printing of nested_types.
  printer_->Print("nested_types=[],  # TODO(robinson): Implement.\n");
  printer_->Print("enum_types=[\n");
  printer_->Indent();
  for (int i = 0; i < message_descriptor.enum_type_count(); ++i) {
    const string descriptor_name = ModuleLevelDescriptorName(
        *message_descriptor.enum_type(i));
    printer_->Print(descriptor_name.c_str());
    printer_->Print(",\n");
  }
  printer_->Outdent();
  printer_->Print("],\n");
  string options_string;
  message_descriptor.options().SerializeToString(&options_string);
  printer_->Print(
      "options=$options_value$",
      "options_value", OptionsValue("MessageOptions", options_string));
  printer_->Outdent();
  printer_->Print(")\n");
}

// Prints Python Descriptor objects for all nested types contained in
// message_descriptor.
//
// Mutually recursive with PrintDescriptor().
void Generator::PrintNestedDescriptors(
    const Descriptor& containing_descriptor) const {
  for (int i = 0; i < containing_descriptor.nested_type_count(); ++i) {
    PrintDescriptor(*containing_descriptor.nested_type(i));
  }
}

// Prints all messages in |file|.
void Generator::PrintMessages() const {
  for (int i = 0; i < file_->message_type_count(); ++i) {
    PrintMessage(*file_->message_type(i));
    printer_->Print("\n");
  }
}

// Prints a Python class for the given message descriptor.  We defer to the
// metaclass to do almost all of the work of actually creating a useful class.
// The purpose of this function and its many helper functions above is merely
// to output a Python version of the descriptors, which the metaclass in
// reflection.py will use to construct the meat of the class itself.
//
// Mutually recursive with PrintNestedMessages().
void Generator::PrintMessage(
    const Descriptor& message_descriptor) const {
  printer_->Print("class $name$(message.Message):\n", "name",
                  message_descriptor.name());
  printer_->Indent();
  printer_->Print("__metaclass__ = reflection.GeneratedProtocolMessageType\n");
  PrintNestedMessages(message_descriptor);
  map<string, string> m;
  m["descriptor_key"] = kDescriptorKey;
  m["descriptor_name"] = ModuleLevelDescriptorName(message_descriptor);
  printer_->Print(m, "$descriptor_key$ = $descriptor_name$\n");
  printer_->Outdent();
}

// Prints all nested messages within |containing_descriptor|.
// Mutually recursive with PrintMessage().
void Generator::PrintNestedMessages(
    const Descriptor& containing_descriptor) const {
  for (int i = 0; i < containing_descriptor.nested_type_count(); ++i) {
    printer_->Print("\n");
    PrintMessage(*containing_descriptor.nested_type(i));
  }
}

// Recursively fixes foreign fields in all nested types in |descriptor|, then
// sets the message_type and enum_type of all message and enum fields to point
// to their respective descriptors.
void Generator::FixForeignFieldsInDescriptor(
    const Descriptor& descriptor) const {
  for (int i = 0; i < descriptor.nested_type_count(); ++i) {
    FixForeignFieldsInDescriptor(*descriptor.nested_type(i));
  }

  for (int i = 0; i < descriptor.field_count(); ++i) {
    const FieldDescriptor& field_descriptor = *descriptor.field(i);
    FixForeignFieldsInField(&descriptor, field_descriptor, "fields_by_name");
  }
}

// Sets any necessary message_type and enum_type attributes
// for the Python version of |field|.
//
// containing_type may be NULL, in which case this is a module-level field.
//
// python_dict_name is the name of the Python dict where we should
// look the field up in the containing type.  (e.g., fields_by_name
// or extensions_by_name).  We ignore python_dict_name if containing_type
// is NULL.
void Generator::FixForeignFieldsInField(const Descriptor* containing_type,
                                        const FieldDescriptor& field,
                                        const string& python_dict_name) const {
  const string field_referencing_expression = FieldReferencingExpression(
      containing_type, field, python_dict_name);
  map<string, string> m;
  m["field_ref"] = field_referencing_expression;
  const Descriptor* foreign_message_type = field.message_type();
  if (foreign_message_type) {
    m["foreign_type"] = ModuleLevelDescriptorName(*foreign_message_type);
    printer_->Print(m, "$field_ref$.message_type = $foreign_type$\n");
  }
  const EnumDescriptor* enum_type = field.enum_type();
  if (enum_type) {
    m["enum_type"] = ModuleLevelDescriptorName(*enum_type);
    printer_->Print(m, "$field_ref$.enum_type = $enum_type$\n");
  }
}

// Returns the module-level expression for the given FieldDescriptor.
// Only works for fields in the .proto file this Generator is generating for.
//
// containing_type may be NULL, in which case this is a module-level field.
//
// python_dict_name is the name of the Python dict where we should
// look the field up in the containing type.  (e.g., fields_by_name
// or extensions_by_name).  We ignore python_dict_name if containing_type
// is NULL.
string Generator::FieldReferencingExpression(
    const Descriptor* containing_type,
    const FieldDescriptor& field,
    const string& python_dict_name) const {
  // We should only ever be looking up fields in the current file.
  // The only things we refer to from other files are message descriptors.
  GOOGLE_CHECK_EQ(field.file(), file_) << field.file()->name() << " vs. "
                                << file_->name();
  if (!containing_type) {
    return field.name();
  }
  return strings::Substitute(
      "$0.$1['$2']",
      ModuleLevelDescriptorName(*containing_type),
      python_dict_name, field.name());
}

// Prints statements setting the message_type and enum_type fields in the
// Python descriptor objects we've already output in ths file.  We must
// do this in a separate step due to circular references (otherwise, we'd
// just set everything in the initial assignment statements).
void Generator::FixForeignFieldsInDescriptors() const {
  for (int i = 0; i < file_->message_type_count(); ++i) {
    FixForeignFieldsInDescriptor(*file_->message_type(i));
  }
  printer_->Print("\n");
}

// We need to not only set any necessary message_type fields, but
// also need to call RegisterExtension() on each message we're
// extending.
void Generator::FixForeignFieldsInExtensions() const {
  // Top-level extensions.
  for (int i = 0; i < file_->extension_count(); ++i) {
    FixForeignFieldsInExtension(*file_->extension(i));
  }
  // Nested extensions.
  for (int i = 0; i < file_->message_type_count(); ++i) {
    FixForeignFieldsInNestedExtensions(*file_->message_type(i));
  }
}

void Generator::FixForeignFieldsInExtension(
    const FieldDescriptor& extension_field) const {
  GOOGLE_CHECK(extension_field.is_extension());
  // extension_scope() will be NULL for top-level extensions, which is
  // exactly what FixForeignFieldsInField() wants.
  FixForeignFieldsInField(extension_field.extension_scope(), extension_field,
                          "extensions_by_name");

  map<string, string> m;
  // Confusingly, for FieldDescriptors that happen to be extensions,
  // containing_type() means "extended type."
  // On the other hand, extension_scope() will give us what we normally
  // mean by containing_type().
  m["extended_message_class"] = ModuleLevelMessageName(
      *extension_field.containing_type());
  m["field"] = FieldReferencingExpression(extension_field.extension_scope(),
                                          extension_field,
                                          "extensions_by_name");
  printer_->Print(m, "$extended_message_class$.RegisterExtension($field$)\n");
}

void Generator::FixForeignFieldsInNestedExtensions(
    const Descriptor& descriptor) const {
  // Recursively fix up extensions in all nested types.
  for (int i = 0; i < descriptor.nested_type_count(); ++i) {
    FixForeignFieldsInNestedExtensions(*descriptor.nested_type(i));
  }
  // Fix up extensions directly contained within this type.
  for (int i = 0; i < descriptor.extension_count(); ++i) {
    FixForeignFieldsInExtension(*descriptor.extension(i));
  }
}

// Returns a Python expression that instantiates a Python EnumValueDescriptor
// object for the given C++ descriptor.
void Generator::PrintEnumValueDescriptor(
    const EnumValueDescriptor& descriptor) const {
  // TODO(robinson): Fix up EnumValueDescriptor "type" fields.
  // More circular references.  ::sigh::
  string options_string;
  descriptor.options().SerializeToString(&options_string);
  map<string, string> m;
  m["name"] = descriptor.name();
  m["index"] = SimpleItoa(descriptor.index());
  m["number"] = SimpleItoa(descriptor.number());
  m["options"] = OptionsValue("EnumValueOptions", options_string);
  printer_->Print(
      m,
      "descriptor.EnumValueDescriptor(\n"
      "  name='$name$', index=$index$, number=$number$,\n"
      "  options=$options$,\n"
      "  type=None)");
}

string Generator::OptionsValue(
    const string& class_name, const string& serialized_options) const {
  if (serialized_options.length() == 0 || GeneratingDescriptorProto()) {
    return "None";
  } else {
    string full_class_name = "descriptor_pb2." + class_name;
    return "descriptor._ParseOptions(" + full_class_name + "(), '"
        + CEscape(serialized_options)+ "')";
  }
}

// Prints an expression for a Python FieldDescriptor for |field|.
void Generator::PrintFieldDescriptor(
    const FieldDescriptor& field, bool is_extension) const {
  string options_string;
  field.options().SerializeToString(&options_string);
  map<string, string> m;
  m["name"] = field.name();
  m["full_name"] = field.full_name();
  m["index"] = SimpleItoa(field.index());
  m["number"] = SimpleItoa(field.number());
  m["type"] = SimpleItoa(field.type());
  m["cpp_type"] = SimpleItoa(field.cpp_type());
  m["label"] = SimpleItoa(field.label());
  m["default_value"] = StringifyDefaultValue(field);
  m["is_extension"] = is_extension ? "True" : "False";
  m["options"] = OptionsValue("FieldOptions", options_string);
  // We always set message_type and enum_type to None at this point, and then
  // these fields in correctly after all referenced descriptors have been
  // defined and/or imported (see FixForeignFieldsInDescriptors()).
  const char field_descriptor_decl[] =
      "descriptor.FieldDescriptor(\n"
      "  name='$name$', full_name='$full_name$', index=$index$,\n"
      "  number=$number$, type=$type$, cpp_type=$cpp_type$, label=$label$,\n"
      "  default_value=$default_value$,\n"
      "  message_type=None, enum_type=None, containing_type=None,\n"
      "  is_extension=$is_extension$, extension_scope=None,\n"
      "  options=$options$)";
  printer_->Print(m, field_descriptor_decl);
}

// Helper for Print{Fields,Extensions}InDescriptor().
void Generator::PrintFieldDescriptorsInDescriptor(
    const Descriptor& message_descriptor,
    bool is_extension,
    const string& list_variable_name,
    int (Descriptor::*CountFn)() const,
    const FieldDescriptor* (Descriptor::*GetterFn)(int) const) const {
  printer_->Print("$list$=[\n", "list", list_variable_name);
  printer_->Indent();
  for (int i = 0; i < (message_descriptor.*CountFn)(); ++i) {
    PrintFieldDescriptor(*(message_descriptor.*GetterFn)(i),
                         is_extension);
    printer_->Print(",\n");
  }
  printer_->Outdent();
  printer_->Print("],\n");
}

// Prints a statement assigning "fields" to a list of Python FieldDescriptors,
// one for each field present in message_descriptor.
void Generator::PrintFieldsInDescriptor(
    const Descriptor& message_descriptor) const {
  const bool is_extension = false;
  PrintFieldDescriptorsInDescriptor(
      message_descriptor, is_extension, "fields",
      &Descriptor::field_count, &Descriptor::field);
}

// Prints a statement assigning "extensions" to a list of Python
// FieldDescriptors, one for each extension present in message_descriptor.
void Generator::PrintExtensionsInDescriptor(
    const Descriptor& message_descriptor) const {
  const bool is_extension = true;
  PrintFieldDescriptorsInDescriptor(
      message_descriptor, is_extension, "extensions",
      &Descriptor::extension_count, &Descriptor::extension);
}

bool Generator::GeneratingDescriptorProto() const {
  return file_->name() == "google/protobuf/descriptor.proto";
}

// Returns the unique Python module-level identifier given to a descriptor.
// This name is module-qualified iff the given descriptor describes an
// entity that doesn't come from the current file.
template <typename DescriptorT>
string Generator::ModuleLevelDescriptorName(
    const DescriptorT& descriptor) const {
  // FIXME(robinson):
  // We currently don't worry about collisions with underscores in the type
  // names, so these would collide in nasty ways if found in the same file:
  //   OuterProto.ProtoA.ProtoB
  //   OuterProto_ProtoA.ProtoB  # Underscore instead of period.
  // As would these:
  //   OuterProto.ProtoA_.ProtoB
  //   OuterProto.ProtoA._ProtoB  # Leading vs. trailing underscore.
  // (Contrived, but certainly possible).
  //
  // The C++ implementation doesn't guard against this either.  Leaving
  // it for now...
  string name = NamePrefixedWithNestedTypes(descriptor, "_");
  UpperString(&name);
  // Module-private for now.  Easy to make public later; almost impossible
  // to make private later.
  name = "_" + name;
  // We now have the name relative to its own module.  Also qualify with
  // the module name iff this descriptor is from a different .proto file.
  if (descriptor.file() != file_) {
    name = ModuleName(descriptor.file()->name()) + "." + name;
  }
  return name;
}

// Returns the name of the message class itself, not the descriptor.
// Like ModuleLevelDescriptorName(), module-qualifies the name iff
// the given descriptor describes an entity that doesn't come from
// the current file.
string Generator::ModuleLevelMessageName(const Descriptor& descriptor) const {
  string name = NamePrefixedWithNestedTypes(descriptor, ".");
  if (descriptor.file() != file_) {
    name = ModuleName(descriptor.file()->name()) + "." + name;
  }
  return name;
}

// Returns the unique Python module-level identifier given to a service
// descriptor.
string Generator::ModuleLevelServiceDescriptorName(
    const ServiceDescriptor& descriptor) const {
  string name = descriptor.name();
  UpperString(&name);
  name = "_" + name;
  if (descriptor.file() != file_) {
    name = ModuleName(descriptor.file()->name()) + "." + name;
  }
  return name;
}

}  // namespace python
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
