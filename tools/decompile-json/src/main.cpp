/*
 * Copyright (c) 2020 Trail of Bits, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <cstdint>
#include <ios>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <magic_enum.hpp>
#include "anvill/Version.h"

// clang-format off
#include <remill/BC/Compat/CTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>

// clang-format on

#include <remill/Arch/Arch.h>
#include <remill/Arch/Name.h>
#include <remill/BC/Compat/Error.h>
#include <remill/BC/IntrinsicTable.h>
#include <remill/BC/Lifter.h>
#include <remill/BC/Util.h>
#include <remill/OS/OS.h>

#include <anvill/ABI.h>
#include <anvill/Lifters/EntityLifter.h>
#include <anvill/Providers/MemoryProvider.h>
#include <anvill/Providers/TypeProvider.h>

#include "anvill/Decl.h"
#include "anvill/Optimize.h"
#include "anvill/Program.h"
#include "anvill/TypeParser.h"
#include "anvill/Util.h"

DECLARE_string(arch);
DECLARE_string(os);

DEFINE_string(spec, "", "Path to a JSON specification of code to decompile.");
DEFINE_string(ir_out, "", "Path to file where the LLVM IR should be saved.");
DEFINE_string(bc_out, "",
              "Path to file where the LLVM bitcode should be "
              "saved.");

static void SetVersion(void) {
  std::stringstream ss;
  auto vs = anvill::version::GetVersionString();
  if (0 == vs.size()) {
    vs = "unknown";
  }

  ss << vs << "\n";
  if (!anvill::version::HasVersionData()) {
    ss << "No extended version information found!\n";
  } else {
    ss << "Commit Hash: " << anvill::version::GetCommitHash() << "\n";
    ss << "Commit Date: " << anvill::version::GetCommitDate() << "\n";
    ss << "Last commit by: " << anvill::version::GetAuthorName() << " ["
       << anvill::version::GetAuthorEmail() << "]\n";
    ss << "\n";
    if (anvill::version::HasUncommittedChanges()) {
      ss << "Uncommitted changes were present during build.\n";
    } else {
      ss << "All changes were committed prior to building.\n";
    }
  }
  google::SetVersionString(ss.str());
}

#if __has_include(<llvm/Support/JSON.h>)
#  include <llvm/Support/JSON.h>

namespace {

// Parse the location of a value. This applies to both parameters and
// return values.
static bool ParseValue(const remill::Arch *arch, anvill::ValueDecl &decl,
                       llvm::json::Object *obj, const char *desc) {

  auto maybe_reg = obj->getString("register");
  if (maybe_reg) {
    decl.reg = arch->RegisterByName(maybe_reg->str());
    if (!decl.reg) {
      LOG(ERROR) << "Unable to locate register '" << maybe_reg->str()
                 << "' used for storing " << desc << ".";
      return false;
    }
  }

  if (auto mem_obj = obj->getObject("memory")) {
    maybe_reg = mem_obj->getString("register");
    if (maybe_reg) {
      decl.mem_reg = arch->RegisterByName(maybe_reg->str());
      if (!decl.mem_reg) {
        LOG(ERROR) << "Unable to locate memory base register '"
                   << maybe_reg->str() << "' used for storing " << desc << ".";
        return false;
      }
    }

    auto maybe_offset = mem_obj->getInteger("offset");
    if (maybe_offset) {
      decl.mem_offset = *maybe_offset;
    }
  }

  if (decl.reg && decl.mem_reg) {
    LOG(ERROR) << "A " << desc << " cannot be resident in both a register "
               << "and a memory location.";
    return false;
  } else if (!decl.reg && !decl.mem_reg) {
    LOG(ERROR)
        << "A " << desc << " must be resident in either a register or "
        << "a memory location (defined in terms of a register and offset).";
    return false;
  }

  return true;
}

// Parse a parameter from the JSON spec. Parameters should have names,
// as that makes the bitcode slightly easier to read, but names are
// not required. They must have types, and these types should be mostly
// reflective of what you would see if you compiled C/C++ source code to
// LLVM bitcode, and inspected the type of the corresponding parameter in
// the bitcode.
static bool ParseParameter(const remill::Arch *arch, llvm::LLVMContext &context,
                           anvill::ParameterDecl &decl,
                           llvm::json::Object *obj) {

  auto maybe_name = obj->getString("name");
  if (maybe_name) {
    decl.name = maybe_name->str();
  } else {
    LOG(WARNING) << "Missing function parameter name.";
  }

  auto maybe_type_str = obj->getString("type");
  if (!maybe_type_str) {
    LOG(ERROR) << "Missing 'type' field in function parameter.";
    return false;
  }

  auto maybe_type = anvill::ParseType(context, *maybe_type_str);
  if (remill::IsError(maybe_type)) {
    LOG(ERROR) << remill::GetErrorString(maybe_type);
    return false;
  }

  decl.type = remill::GetReference(maybe_type);
  return ParseValue(arch, decl, obj, "function parameter");
}

//
static bool ParseTypedRegister(
    const remill::Arch *arch, llvm::LLVMContext &context,
    std::unordered_map<uint64_t, std::vector<anvill::TypedRegisterDecl>>
        &reg_map,
    llvm::json::Object *obj) {

  auto maybe_address = obj->getInteger("address");
  if (!maybe_address) {
    LOG(ERROR) << "Missing 'address' field in typed register.";
    return false;
  }
  anvill::TypedRegisterDecl decl;
  auto maybe_value = obj->getInteger("value");
  if (maybe_value) {
    decl.value = *maybe_value;
  }

  auto maybe_type_str = obj->getString("type");
  if (!maybe_type_str) {
    LOG(ERROR) << "Missing 'type' field in typed register.";
    return false;
  }

  auto maybe_type = anvill::ParseType(context, *maybe_type_str);
  if (remill::IsError(maybe_type)) {
    LOG(ERROR) << remill::GetErrorString(maybe_type);
    return false;
  }

  decl.type = remill::GetReference(maybe_type);

  auto register_name = obj->getString("register");
  if (!register_name) {
    LOG(ERROR) << "Missing 'register' field in typed register";
    return false;
  }

  auto maybe_reg = arch->RegisterByName(register_name->str());
  if (!maybe_reg) {
    LOG(ERROR) << "Unable to locate register '" << register_name->str()
               << "' for typed register information:"
               << " at '" << std::hex << *maybe_address << std::dec << "'";
    return false;
  }

  decl.reg = maybe_reg;
  reg_map[*maybe_address].emplace_back(std::move(decl));
  return true;
}

// Parse a return value from the JSON spec.
static bool ParseReturnValue(const remill::Arch *arch,
                             llvm::LLVMContext &context,
                             anvill::ValueDecl &decl, llvm::json::Object *obj) {

  auto maybe_type_str = obj->getString("type");
  if (!maybe_type_str) {
    LOG(ERROR) << "Missing 'type' field in function return value.";
    return false;
  }

  auto maybe_type = anvill::ParseType(context, *maybe_type_str);
  if (remill::IsError(maybe_type)) {
    LOG(ERROR) << remill::GetErrorString(maybe_type);
    return false;
  }

  decl.type = remill::GetReference(maybe_type);
  return ParseValue(arch, decl, obj, "function return value");
}

// Try to unserialize function info from a JSON specification. These
// are really function prototypes / declarations, and not any isntruction
// data (that is separate, if present).
static bool ParseFunction(const remill::Arch *arch, llvm::LLVMContext &context,
                          anvill::Program &program, llvm::json::Object *obj) {

  anvill::FunctionDecl decl;

  auto maybe_ea = obj->getInteger("address");
  if (!maybe_ea) {
    LOG(ERROR) << "Missing function address in specification";
    return false;
  }

  decl.arch = arch;
  decl.address = static_cast<uint64_t>(*maybe_ea);

  if (auto params = obj->getArray("parameters")) {
    for (llvm::json::Value &maybe_param : *params) {
      if (auto param_obj = maybe_param.getAsObject()) {
        auto &pv = decl.params.emplace_back();
        if (!ParseParameter(arch, context, pv, param_obj)) {
          return false;
        }
      } else {
        LOG(ERROR) << "Non-object value in 'parameters' array of "
                   << "function at address '" << std::hex << decl.address
                   << std::dec << "'";
        return false;
      }
    }
  }

  if (auto register_info = obj->getArray("register_info")) {
    for (llvm::json::Value &maybe_reg : *register_info) {
      if (auto reg_obj = maybe_reg.getAsObject()) {

        // decl.register_info.emplace_back();
        // Parse the register info!
        if (!ParseTypedRegister(arch, context, decl.reg_info, reg_obj)) {
          return false;
        }
      } else {
        LOG(ERROR) << "Non-object value in 'register_info' array of "
                   << "function at address '" << decl.address << std::dec
                   << "'";
      }
    }
  }

  // Get the return address location.
  if (auto ret_addr = obj->getObject("return_address")) {
    if (!ParseValue(arch, decl.return_address, ret_addr, "return address")) {
      return false;
    }
  } else {
    LOG(ERROR) << "Non-present or non-object 'return_address' in function "
               << "specification at '" << std::hex << decl.address << std::dec
               << "'";
    return false;
  }

  // Parse the value of the stack pointer on exit from the function, which is
  // defined in terms of `reg + offset` for a value of a register `reg`
  // on entry to the function.
  if (auto ret_sp = obj->getObject("return_stack_pointer")) {
    auto maybe_reg = ret_sp->getString("register");
    if (maybe_reg) {
      decl.return_stack_pointer = arch->RegisterByName(maybe_reg->str());
      if (!decl.return_stack_pointer) {
        LOG(ERROR) << "Unable to locate register '" << maybe_reg->str()
                   << "' used computing the exit value of the "
                   << "stack pointer in function specification at '" << std::hex
                   << decl.address << std::dec << "'";
        return false;
      }
    } else {
      LOG(ERROR)
          << "Non-present or non-string 'register' in 'return_stack_pointer' "
          << "object of function specification at '" << std::hex << decl.address
          << std::dec << "'";
      return false;
    }

    auto maybe_offset = ret_sp->getInteger("offset");
    if (maybe_offset) {
      decl.return_stack_pointer_offset = *maybe_offset;
    }
  } else {
    LOG(ERROR)
        << "Non-present or non-object 'return_stack_pointer' in function "
        << "specification at '" << std::hex << decl.address << std::dec << "'";
    return false;
  }

  if (auto returns = obj->getArray("return_values")) {
    for (llvm::json::Value &maybe_ret : *returns) {
      if (auto ret_obj = maybe_ret.getAsObject()) {
        auto &rv = decl.returns.emplace_back();
        if (!ParseReturnValue(arch, context, rv, ret_obj)) {
          return false;
        }
      } else {
        LOG(ERROR) << "Non-object value in 'return_values' array of "
                   << "function at address '" << std::hex << decl.address
                   << std::dec << "'";
        return false;
      }
    }
  }

  if (auto maybe_is_noreturn = obj->getBoolean("is_noreturn")) {
    decl.is_noreturn = *maybe_is_noreturn;
  }

  if (auto maybe_is_variadic = obj->getBoolean("is_variadic")) {
    decl.is_variadic = *maybe_is_variadic;
  }

  if (auto maybe_cc = obj->getInteger("calling_convention")) {
    decl.calling_convention = static_cast<llvm::CallingConv::ID>(*maybe_cc);
  }

  auto err = program.DeclareFunction(decl);
  if (remill::IsError(err)) {
    LOG(ERROR) << remill::GetErrorString(err);
    return false;
  }

  return true;
}

// Try to unserialize variable information.
static bool ParseVariable(const remill::Arch *arch, llvm::LLVMContext &context,
                          anvill::Program &program, llvm::json::Object *obj) {
  anvill::GlobalVarDecl decl;

  auto maybe_ea = obj->getInteger("address");
  if (!maybe_ea) {
    LOG(ERROR) << "Missing global variable address in specification";
    return false;
  }

  decl.address = static_cast<uint64_t>(*maybe_ea);

  auto maybe_type_str = obj->getString("type");
  if (!maybe_type_str) {
    LOG(ERROR) << "Missing 'type' field in global variable.";
    return false;
  }

  auto maybe_type = anvill::ParseType(context, *maybe_type_str);
  if (remill::IsError(maybe_type)) {
    LOG(ERROR) << remill::GetErrorString(maybe_type);
    return false;
  }

  decl.type = remill::GetReference(maybe_type);
  auto err = program.DeclareVariable(decl);
  if (remill::IsError(err)) {
    LOG(ERROR) << remill::GetErrorString(err);
    return false;
  }

  return true;
}

// Parse a memory range.
static bool ParseRange(anvill::Program &program, llvm::json::Object *obj) {

  auto maybe_ea = obj->getInteger("address");
  if (!maybe_ea) {
    LOG(ERROR) << "Missing address in memory range specification";
    return false;
  }

  anvill::ByteRange range;
  range.address = static_cast<uint64_t>(*maybe_ea);

  auto perm = obj->getBoolean("is_writeable");
  if (perm) {
    range.is_writeable = *perm;
  }

  perm = obj->getBoolean("is_executable");
  if (perm) {
    range.is_executable = *perm;
  }

  auto maybe_bytes = obj->getString("data");
  if (!maybe_bytes) {
    LOG(ERROR) << "Missing byte string in memory range specification "
               << "at address '" << std::hex << range.address << std::dec
               << '.';
    return false;
  }

  const llvm::StringRef &bytes = *maybe_bytes;
  if (bytes.size() % 2) {
    LOG(ERROR) << "Length of byte string in memory range specification "
               << "at address '" << std::hex << range.address << std::dec
               << "' must have an even number of characters.";
    return false;
  }

  std::vector<uint8_t> decoded_bytes;
  decoded_bytes.reserve(bytes.size() / 2);

  // Parse out the hex-encoded byte sequence.
  for (auto i = 0ul; i < bytes.size(); i += 2) {
    char nibbles[3] = {bytes[i], bytes[i + 1], '\0'};
    char *parsed_to = nullptr;
    auto byte_val = strtol(nibbles, &parsed_to, 16);

    if (parsed_to != &(nibbles[2])) {
      LOG(ERROR) << "Invalid hex byte value '" << nibbles << "' in memory "
                 << "range specification at address '" << std::hex
                 << range.address << std::dec << "'.";
      return false;
    }

    decoded_bytes.push_back(static_cast<uint8_t>(byte_val));
  }

  range.begin = decoded_bytes.data();
  range.end = decoded_bytes.data() + decoded_bytes.size();

  auto err = program.MapRange(range);
  if (remill::IsError(err)) {
    LOG(ERROR) << remill::GetErrorString(err);
    return false;
  }

  return true;
}

static bool ParseControlFlowRedirection(anvill::Program &program,
                                        llvm::json::Array &redirection_list) {

  auto index{0U};

  std::stringstream buffer;

  for (const llvm::json::Value &list_entry : redirection_list) {
    auto address_pair = list_entry.getAsArray();
    if (address_pair == nullptr) {
      LOG(ERROR) << "Non-JSON list entry in 'control_flow_redirections' array of spec file '"
                 << FLAGS_spec << "'";

      return false;
    }

    const auto &source_address_obj = address_pair->operator[](0);
    auto opt_source_address = source_address_obj.getAsInteger();
    if (!opt_source_address) {
      LOG(ERROR) << "Invalid integer value in source address for the #"
                 << index << " of the control_flow_redirections in the following spec file: '"
                 << FLAGS_spec << "'";

      return false;
    }

    const auto &dest_address_obj = address_pair->operator[](1);
    auto opt_dest_address = dest_address_obj.getAsInteger();
    if (!opt_dest_address) {
      LOG(ERROR) << "Invalid integer value in destination address for the #"
                 << index << " of the control_flow_redirections in the following spec file: '"
                 << FLAGS_spec << "'";

      return false;
    }

    auto source_address = opt_source_address.getValue();
    auto dest_address = opt_dest_address.getValue();

    buffer << "  " << std::hex << source_address << " -> " << dest_address << "\n";

    program.AddControlFlowRedirection(source_address, dest_address);

    ++index;
  }

  auto redirection_output = buffer.str();
  if (!redirection_output.empty()) {
    std::cout << "Control flow redirections:\n" << redirection_output;
  }

  return true;
}

// Parse the core data out of a JSON specification, and do a small
// amount of validation. A JSON spec contains the following:
//
//  - For each function:
//    - Function name (if any)
//    - Address.
//    - For each argument:
//    - - Argument name
//    - - Location specifier, which is a register name or a stack pointer displacement.
//    - - Type.
//    - For each return value
//    - - Location specifier
//    - - Type.
//
//  - For each global variable:
//    - Variable name (if any)
//    - Type.
//    - Address.
//
//  - For each memory range:
//    - Starting address. No alignment restrictions apply.
//    - Permissions (is_readable, is_writeable, is_executable).
//    - Data (hex-encoded byte string).
static bool ParseSpec(const remill::Arch *arch, llvm::LLVMContext &context,
                      anvill::Program &program, llvm::json::Object *spec) {

  auto num_funcs = 0;
  if (auto funcs = spec->getArray("functions")) {
    for (llvm::json::Value &func : *funcs) {
      if (auto func_obj = func.getAsObject()) {
        if (!ParseFunction(arch, context, program, func_obj)) {
          return false;
        } else {
          ++num_funcs;
        }
      } else {
        LOG(ERROR) << "Non-JSON object in 'functions' array of spec file '"
                   << FLAGS_spec << "'";
        return false;
      }
    }
  } else if (spec->find("functions") != spec->end()) {
    LOG(ERROR) << "Non-JSON array value for 'functions' in spec file '"
               << FLAGS_spec << "'";
    return false;
  }

  if (auto redirection_list = spec->getArray("control_flow_redirections")) {
    if (!ParseControlFlowRedirection(program, *redirection_list)) {
      LOG(ERROR)
          << "Failed to parse the 'control_flow_redirections' section in spec file '"
          << FLAGS_spec << "'";

      return false;
    }

  } else if (spec->find("control_flow_redirections") != spec->end()) {
    LOG(ERROR)
        << "Non-JSON array value for 'control_flow_redirections' in spec file '"
        << FLAGS_spec << "'";
    return false;
  }

  if (auto vars = spec->getArray("variables")) {
    for (llvm::json::Value &var : *vars) {
      if (auto var_obj = var.getAsObject()) {
        if (!ParseVariable(arch, context, program, var_obj)) {
          return false;
        }
      } else {
        LOG(ERROR) << "Non-JSON object in 'variables' array of spec file '"
                   << FLAGS_spec << "'";
        return false;
      }
    }
  } else if (spec->find("variables") != spec->end()) {
    LOG(ERROR) << "Non-JSON array value for 'variables' in spec file '"
               << FLAGS_spec << "'";
    return false;
  }

  if (auto ranges = spec->getArray("memory")) {
    for (llvm::json::Value &range : *ranges) {
      if (auto range_obj = range.getAsObject()) {
        if (!ParseRange(program, range_obj)) {
          return false;
        }
      } else {
        LOG(ERROR) << "Non-JSON object in 'bytes' array of spec file '"
                   << FLAGS_spec << "'";
        return false;
      }
    }
  } else if (spec->find("memory") != spec->end()) {
    LOG(ERROR) << "Non-JSON array value for 'memory' in spec file '"
               << FLAGS_spec << "'";
    return false;
  }

  if (auto symbols = spec->getArray("symbols")) {
    for (llvm::json::Value &maybe_ea_name : *symbols) {
      if (auto ea_name = maybe_ea_name.getAsArray(); ea_name) {
        if (ea_name->size() != 2) {
          LOG(ERROR) << "Symbol entry doesn't have two values in spec file '"
                     << FLAGS_spec << "'";
          return false;
        }
        auto &maybe_ea = ea_name->operator[](0);
        auto &maybe_name = ea_name->operator[](1);

        if (auto ea = maybe_ea.getAsInteger(); ea) {
          if (auto name = maybe_name.getAsString(); name) {
            program.AddNameToAddress(name->str(),
                                     static_cast<uint64_t>(ea.getValue()));
          } else {
            LOG(ERROR)
                << "Second value in symbol entry must be a string in spec file '"
                << FLAGS_spec << "'";
            return false;
          }
        } else {
          LOG(ERROR)
              << "First value in symbol entry must be an integer in spec file '"
              << FLAGS_spec << "'";
          return false;
        }
      } else {
        LOG(ERROR)
            << "Expected array entries inside of 'symbols' array in spec file '"
            << FLAGS_spec << "'";
        return false;
      }
    }
  } else if (spec->find("symbols") != spec->end()) {
    LOG(ERROR) << "Non-JSON array value for 'symbols' in spec file '"
               << FLAGS_spec << "'";
    return false;
  }

  return true;
}

}  // namespace

int main(int argc, char *argv[]) {

  // get version string from git, and put as output to --version
  // from gflags
  SetVersion();
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  if (FLAGS_spec.empty()) {
    LOG(ERROR)
        << "Please specify a path to a JSON specification file in --spec.";
    return EXIT_FAILURE;
  }

  if (FLAGS_spec == "/dev/stdin") {
    FLAGS_spec = "-";
  }

  auto maybe_buff = llvm::MemoryBuffer::getFileOrSTDIN(FLAGS_spec);
  if (remill::IsError(maybe_buff)) {
    LOG(ERROR) << "Unable to read JSON spec file '" << FLAGS_spec
               << "': " << remill::GetErrorString(maybe_buff);
    return EXIT_FAILURE;
  }

  const std::unique_ptr<llvm::MemoryBuffer> &buff =
      remill::GetReference(maybe_buff);
  auto maybe_json = llvm::json::parse(buff->getBuffer());
  if (remill::IsError(maybe_json)) {
    LOG(ERROR) << "Unable to parse JSON spec file '" << FLAGS_spec
               << "': " << remill::GetErrorString(maybe_json);
    return EXIT_FAILURE;
  }

  llvm::json::Value &json = remill::GetReference(maybe_json);
  const auto spec = json.getAsObject();
  if (!spec) {
    LOG(ERROR) << "JSON spec file '" << FLAGS_spec
               << "' must contain a single object.";
    return EXIT_FAILURE;
  }

  // Take the architecture and OS names out of the JSON spec, and
  // fall back on the command-line flags if those are missing.
  auto maybe_arch = spec->getString("arch");
  auto arch_str = FLAGS_arch;
  if (maybe_arch) {
    arch_str = maybe_arch->str();
  }

  auto maybe_os = spec->getString("os");
  auto os_str = FLAGS_os;
  if (maybe_os) {
    os_str = maybe_os->str();
  }

  llvm::LLVMContext context;
  llvm::Module module("lifted_code", context);

  // Get a unique pointer to a remill architecture object. The architecture
  // object knows how to deal with everything for this specific architecture,
  // such as semantics, register,  etc.
  auto arch = remill::Arch::Build(&context, remill::GetOSName(os_str),
                                  remill::GetArchName(arch_str));
  if (!arch) {
    return EXIT_FAILURE;
  }

  anvill::Program program;
  auto memory = anvill::MemoryProvider::CreateProgramMemoryProvider(program);
  auto types =
      anvill::TypeProvider::CreateProgramTypeProvider(context, program);

  auto ctrl_flow_provider_res = anvill::IControlFlowProvider::Create(program);
  if (!ctrl_flow_provider_res.Succeeded()) {
    auto error = ctrl_flow_provider_res.TakeError();

    std::cerr << "Failed to create the control flow provider: "
              << magic_enum::enum_name(error) << "\n";

    return EXIT_FAILURE;
  }

  anvill::LifterOptions
      options(arch.get(), module,ctrl_flow_provider_res.TakeValue());

  // NOTE(pag): Unfortunately, we need to load the semantics module first,
  //            which happens deep inside the `EntityLifter`. Only then does
  //            Remill properly know about register information, which
  //            subsequently allows it to parse value decls in specs :-(
  anvill::EntityLifter lifter(options, memory, types);

  // Parse the spec, which contains as much or as little details about what is
  // being lifted as the spec generator desired and put it into an
  // anvill::Program object, which is effectively a representation of the spec
  if (!ParseSpec(arch.get(), context, program, spec)) {
    return EXIT_FAILURE;
  }

  program.ForEachVariable([&](const anvill::GlobalVarDecl *decl) {
    (void) lifter.LiftEntity(*decl);
    return true;
  });

  // Lift functions.
  program.ForEachFunction([&](const anvill::FunctionDecl *decl) {
    (void) lifter.LiftEntity(*decl);
    return true;
  });

  // Verify the module
  if (!remill::VerifyModule(&module)) {
    std::string json_outs;
    if (llvm::json::fromJSON(json, json_outs)) {
      std::cerr << "Couldn't verify module produced from spec:\n"
                << json_outs << '\n';

    } else {
      std::cerr << "Couldn't verify module produced from spec:\n"
                << buff->getBuffer().str() << '\n';
    }
    return EXIT_FAILURE;
  }

  // OLD: Apply optimizations.
  anvill::OptimizeModule(lifter, arch.get(), program, module, options);

  // Apply symbol names to functions if we have the names.
  program.ForEachNamedAddress([&](uint64_t addr, const std::string &name,
                                  const anvill::FunctionDecl *fdecl,
                                  const anvill::GlobalVarDecl *vdecl) {
    if (vdecl) {
      if (auto var = lifter.DeclareEntity(*vdecl)) {
        var->setName(name);
      }
    } else if (fdecl) {
      if (auto func = lifter.DeclareEntity(*fdecl)) {
        func->setName(name);
      }
    }
    return true;
  });

  // Clean up by initializing variables.
  for (auto &var : module.globals()) {
    if (!var.isDeclaration()) {
      continue;
    }
    const auto name = var.getName();
    if (name.startswith(anvill::kAnvillNamePrefix)) {
      var.setInitializer(llvm::Constant::getNullValue(var.getValueType()));
      var.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
  }

  int ret = EXIT_SUCCESS;

  if (!FLAGS_ir_out.empty()) {
    if (!remill::StoreModuleIRToFile(&module, FLAGS_ir_out, true)) {
      std::cerr << "Could not save LLVM IR to " << FLAGS_ir_out << '\n';
      ret = EXIT_FAILURE;
    }
  }
  if (!FLAGS_bc_out.empty()) {
    if (!remill::StoreModuleToFile(&module, FLAGS_bc_out, true)) {
      std::cerr << "Could not save LLVM bitcode to " << FLAGS_bc_out << '\n';
      ret = EXIT_FAILURE;
    }
  }

  return ret;
}

#else
int main(int argc, char *argv[]) {
  SetVersion();
  google::ParseCommandLineFlags(&argc, &argv, true);
  std::cerr << "LLVM JSON API is not available in this version of LLVM\n";
  return EXIT_FAILURE;
}
#endif
