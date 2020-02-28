// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "compiler/compiler.h"
#include "maintenance/controller.h"

#include "common/cmd_utils.h"
#include "common/debug.h"
#include "common/expected.h"

#include "db/models.h"
#include "inode2filename/inode.h"
#include "inode2filename/search_directories.h"

#include <android-base/file.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>
#include <string>
#include <sys/wait.h>

namespace iorap::maintenance {

// Gets the path of output compiled trace.
db::CompiledTraceFileModel CalculateNewestFilePath(
    const std::string& package_name,
    const std::string& activity_name,
    int version) {
   db::VersionedComponentName versioned_component_name{
     package_name, activity_name, version};

   db::CompiledTraceFileModel output_file =
       db::CompiledTraceFileModel::CalculateNewestFilePath(versioned_component_name);

   return output_file;
}

using ArgString = const char*;

static constexpr const char kCommandFileName[] = "/system/bin/iorap.cmd.compiler";

int Exec::Execve(const std::string& pathname,
                 std::vector<std::string>& argv_vec,
                 char *const envp[]) {
  std::unique_ptr<ArgString[]> argv_ptr =
      common::VecToArgv(kCommandFileName, argv_vec);

  return execve(pathname.c_str(), (char**)argv_ptr.get(), envp);
}

pid_t Exec::Fork() {
  return fork();
}

// Represents the parameters used when fork+exec compiler.
struct CompilerForkParameters {
  std::vector<std::string> input_pbs;
  std::vector<uint64_t> timestamp_limit_ns;
  std::string output_proto;
  ControllerParameters controller_params;

  CompilerForkParameters(const std::vector<compiler::CompilationInput>& perfetto_traces,
                         const std::string& output_proto,
                         ControllerParameters controller_params) :
    output_proto(output_proto), controller_params(controller_params) {
        for (compiler::CompilationInput perfetto_trace : perfetto_traces) {
          input_pbs.push_back(perfetto_trace.filename);
          timestamp_limit_ns.push_back(perfetto_trace.timestamp_limit_ns);
        }
  }
};

std::vector<std::string> MakeCompilerParams(const CompilerForkParameters& params) {
    std::vector<std::string> argv;
    ControllerParameters controller_params = params.controller_params;

    common::AppendArgsRepeatedly(argv, params.input_pbs);
    common::AppendArgsRepeatedly(argv, "--timestamp_limit_ns", params.timestamp_limit_ns);

    if (controller_params.output_text) {
      argv.push_back("--output-text");
    }

    common::AppendArgs(argv, "--output-proto", params.output_proto);

    if (controller_params.inode_textcache) {
      common::AppendArgs(argv, "--inode-textcache", *controller_params.inode_textcache);
    }

    if (controller_params.verbose) {
      argv.push_back("--verbose");
    }

    return argv;
}

bool StartViaFork(const CompilerForkParameters& params) {
  const ControllerParameters& controller_params = params.controller_params;
  pid_t child = controller_params.exec->Fork();

  if (child == -1) {
    LOG(FATAL) << "Failed to fork a process for compilation";
  } else if (child > 0) {  // we are the caller of this function
    LOG(DEBUG) << "forked into a process for compilation , pid = " << child;

    int wstatus;
    waitpid(child, /*out*/&wstatus, /*options*/0);
    if (!WIFEXITED(wstatus)) {
      LOG(ERROR) << "Child terminated abnormally, status: " << WEXITSTATUS(wstatus);
      return false;
    }
    LOG(DEBUG) << "Child terminated, status: " << WEXITSTATUS(wstatus);

    return true;
  } else {
    // we are the child that was forked.
    std::vector<std::string> argv_vec = MakeCompilerParams(params);
    std::unique_ptr<ArgString[]> argv_ptr =
        common::VecToArgv(kCommandFileName, argv_vec);

    std::stringstream argv; // for debugging.
    for (std::string arg : argv_vec) {
      argv  << arg << ' ';
    }
    LOG(DEBUG) << "fork+exec: " << kCommandFileName << " " << argv.str();

    controller_params.exec->Execve(kCommandFileName,
                                          argv_vec,
                                         /*envp*/nullptr);
    // This should never return.
  }
  return false;
}

// Gets the perfetto trace infos in the histories.
std::vector<compiler::CompilationInput> GetPerfettoTraceInfo(
    const db::DbHandle& db,
    const std::vector<db::AppLaunchHistoryModel>& histories) {
  std::vector<compiler::CompilationInput> perfetto_traces;

  for(db::AppLaunchHistoryModel history : histories) {
    // Get perfetto trace.
    std::optional<db::RawTraceModel> raw_trace =
        db::RawTraceModel::SelectByHistoryId(db, history.id);
    if (!raw_trace) {
      LOG(ERROR) << "Cannot find raw trace for history_id: "
                 << history.id;
      continue;
    }

    uint64_t timestamp_limit = std::numeric_limits<uint64_t>::max();
    // Get corresponding timestamp limit.
    if (history.report_fully_drawn_ns) {
      timestamp_limit = *history.report_fully_drawn_ns;
    } else if (history.total_time_ns) {
      timestamp_limit = *history.total_time_ns;
    } else {
      LOG(ERROR) << " No timestamp exists. Using the max value.";
    }
    perfetto_traces.push_back({raw_trace->file_path, timestamp_limit});
  }
  return perfetto_traces;
}

// Helper struct for printing vector.
template <class T>
struct VectorPrinter {
  std::vector<T>& values;
};

std::ostream& operator<<(std::ostream& os,
                      const struct compiler::CompilationInput& perfetto_trace) {
  os << "file_path: " << perfetto_trace.filename << " "
     << "timestamp_limit: " << perfetto_trace.timestamp_limit_ns;
  return os;
}

template <class T>
std::ostream& operator<<(std::ostream& os, const struct VectorPrinter<T>& printer) {
  os << "[\n";
  for (T i : printer.values) {
    os << i << ",\n";
  }
  os << "]\n";
  return os;
}

// Compiled the perfetto traces for an activity.
bool CompileActivity(const db::DbHandle& db,
                     int package_id,
                     const std::string& package_name,
                     const std::string& activity_name,
                     int version,
                     const ControllerParameters& params) {
  db::CompiledTraceFileModel output_file =
      CalculateNewestFilePath(package_name, activity_name, version);

  std::string file_path = output_file.FilePath();

  if (!params.recompile && std::filesystem::exists(file_path)) {
    LOG(DEBUG) << "compiled trace exists in " << file_path;
    return true;
  }

  std::optional<db::ActivityModel> activity =
      db::ActivityModel::SelectByNameAndPackageId(db, activity_name.c_str(), package_id);
  if (!activity) {
    LOG(ERROR) << "Cannot find activity for package_id: " << package_id
               <<" activity_name: " <<activity_name;
    return false;
  }

  int activity_id = activity->id;

  std::vector<db::AppLaunchHistoryModel> histories =
      db::AppLaunchHistoryModel::SelectActivityHistoryForCompile(db, activity_id);

  {
    std::vector<compiler::CompilationInput> perfetto_traces =
        GetPerfettoTraceInfo(db, histories);

    if (perfetto_traces.size() < params.min_traces) {
      LOG(DEBUG) << "The number of perfetto traces is " << perfetto_traces.size()
                 <<", which is less than " << params.min_traces;
      return false;
    }

    // Show the compilation config.
    LOG(DEBUG) << "Try to compiled package_id: " << package_id
               << " package_name: " << package_name
               << " activity_name: " << activity_name
               << " version: " << version
               << " file_path: " << file_path
               << " verbose: " << params.verbose
               << " perfetto_traces: "
               << VectorPrinter<compiler::CompilationInput>{perfetto_traces};
    if (params.inode_textcache) {
      LOG(DEBUG) << "inode_textcache: " << *params.inode_textcache;
    }

    CompilerForkParameters compiler_params{perfetto_traces, file_path, params};

    if (!output_file.MkdirWithParents()) {
      LOG(ERROR) << "Compile activity failed. Failed to mkdirs " << file_path;
      return false;
    }

    if (!StartViaFork(compiler_params)) {
      LOG(ERROR) << "Compilation failed for package_id:" << package_id
                 <<" activity_name: " <<activity_name;
      return false;
    }

  }

  std::optional<db::PrefetchFileModel> compiled_trace =
      db::PrefetchFileModel::Insert(db, activity_id, file_path);
  if (!compiled_trace) {
    LOG(ERROR) << "Cannot insert compiled trace activity_id: " << activity_id
               << " file_path: " << file_path;
    return false;
  }
  return true;
}

// Compiled the perfetto traces for activities in an package.
bool CompilePackage(const db::DbHandle& db,
                    const std::string& package_name,
                    int version,
                    const ControllerParameters& params) {
  std::optional<db::PackageModel> package =
        db::PackageModel::SelectByNameAndVersion(db, package_name.c_str(), version);

  if (!package) {
    LOG(ERROR) << "Cannot find package for package_name: "
               << package_name
               << " and version "
               << version;
    return false;
  }

  std::vector<db::ActivityModel> activities =
      db::ActivityModel::SelectByPackageId(db, package->id);

  bool ret = true;
  for (db::ActivityModel activity : activities) {
    if (!CompileActivity(db, package->id, package->name, activity.name, version, params)) {
      ret = false;
    }
  }
  return ret;
}

// Compiled the perfetto traces for packages in a device.
bool CompileAppsOnDevice(const db::DbHandle& db, const ControllerParameters& params) {
  std::vector<db::PackageModel> packages = db::PackageModel::SelectAll(db);
  bool ret = true;
  for (db::PackageModel package : packages) {
    if (!CompilePackage(db, package.name, package.version, params)) {
      ret = false;
    }
  }

  return ret;
}

bool Compile(const std::string& db_path, const ControllerParameters& params) {
  iorap::db::SchemaModel db_schema = db::SchemaModel::GetOrCreate(db_path);
  db::DbHandle db{db_schema.db()};
  return CompileAppsOnDevice(db, params);
}

bool Compile(const std::string& db_path,
             const std::string& package_name,
             int version,
             const ControllerParameters& params) {
  iorap::db::SchemaModel db_schema = db::SchemaModel::GetOrCreate(db_path);
  db::DbHandle db{db_schema.db()};
  return CompilePackage(db, package_name, version, params);
}

bool Compile(const std::string& db_path,
             const std::string& package_name,
             const std::string& activity_name,
             int version,
             const ControllerParameters& params) {
  iorap::db::SchemaModel db_schema = db::SchemaModel::GetOrCreate(db_path);
  db::DbHandle db{db_schema.db()};

  std::optional<db::PackageModel> package =
      db::PackageModel::SelectByNameAndVersion(db, package_name.c_str(), version);

  if (!package) {
    LOG(ERROR) << "Cannot find package with name "
               << package_name
               << " and version "
               << version;
    return false;
  }
  return CompileActivity(db, package->id, package_name, activity_name, version, params);
}

}  // namespace iorap::maintenance
