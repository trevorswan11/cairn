const std = @import("std");
const zon = @import("build.zig.zon");

pub const stdx = @import("stdx");

const CDBGenerator = stdx.CDBGenerator;
const LOCCounter = stdx.LOCCounter;
const RemoveDir = stdx.RemoveDir;

pub fn build(b: *std.Build) !void {
    const optimize = b.standardOptimizeOption(.{
        .preferred_optimize_mode = .ReleaseFast,
    });

    const profile = b.option(bool, "profile", "Enable chromium tracing") orelse false;
    const stdx_dep = b.dependency("stdx", .{
        .target = b.graph.host,
        .optimize = optimize,
        .profile = profile,
        .building_for_dep = true,
        .run_cdb_gen = false,
    });

    var compiler_flags: stdx.ArrayList([]const u8) = .fromSlice(b, &stdx.utils.base_cxx_flags);
    compiler_flags.append("-DMAGIC_ENUM_RANGE_MAX=255");
    const dist_flags: []const []const u8 = &.{ "-DNDEBUG", "-DCAIRN_DIST" };

    var package_flags = compiler_flags.clone();
    package_flags.appendSlice(dist_flags);
    stdx.CDBGenerator.addCdbFlags(b, &compiler_flags.wrapped);

    switch (optimize) {
        .Debug => compiler_flags.appendSlice(&.{ "-g", "-DCAIRN_DEBUG" }),
        .ReleaseSafe => compiler_flags.appendSlice(&.{"-DCAIRN_RELEASE"}),
        .ReleaseFast, .ReleaseSmall => compiler_flags.appendSlice(dist_flags),
    }

    const install_tests_only = b.option(
        bool,
        "install-tests-only",
        "Install tests without running them (default: false)",
    ) orelse false;

    const cdb_gen: *CDBGenerator = .init(b);
    var cdb_steps: stdx.ArrayList(*std.Build.Step) = .init(b);
    const artifacts = try addArtifacts(b, .{
        .optimize = optimize,
        .cxx_flags = compiler_flags.items(),
        .cdb_steps = &cdb_steps,
        .install_tests_only = install_tests_only,
        .stdx_dep = stdx_dep,
        .profile = profile,
    });
    for (cdb_steps.items()) |cdb_step| cdb_gen.step.dependOn(cdb_step);

    try addToolingSteps(b, .{
        .cdb_gen = cdb_gen,
        .cppcheck = stdx_dep.artifact("cppcheck"),
    });

    try addPackageStep(b, .{
        .cxx_flags = package_flags.items(),
        .compressor = stdx_dep.artifact("compressor"),
    });

    if (stdx.KcovBuilder.allowedTarget(b.graph.host)) {
        if (artifacts.tests) |tests| {
            var include_patterns: stdx.ArrayList([]const u8) = .init(b);
            const libraries = [_][]const u8{
                "support", "storage", "wal", "txn",
                "sql",     "exec",    "opt", "net",
            };
            for (libraries) |library| {
                include_patterns.append(b.fmt("lib/{s}/src", .{library}));
                include_patterns.append(b.fmt("lib/{s}/include", .{library}));
            }

            var configs: stdx.ArrayList(stdx.steps.RunKcovConfig) = .init(b);
            const test_suites = [_]Test{
                tests.support,     tests.storage, tests.wal, tests.txn,
                tests.sql,         tests.exec,    tests.opt, tests.net,
                tests.integration,
            };
            for (test_suites) |suite| {
                configs.append(.{
                    .artifact = suite.artifact,
                    .include_patterns = include_patterns.items(),
                });
            }

            try stdx.steps.addCoverage(b, .{
                .curl = stdx_dep.artifact("curl"),
                .kcov = stdx_dep.artifact("kcov"),
                .run_configs = configs.items(),
            });
        }
    }
}

const ArtifactConfig = struct {
    /// Used for the directory lookup and artifact name
    name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    cxx_flags: []const []const u8,
    cdb_steps: ?*stdx.ArrayList(*std.Build.Step),
    config_h: *std.Build.Step.ConfigHeader,
    stdx_dep: *std.Build.Dependency,
    auto_install: bool,
    /// The library's include and source dirs are implicitly included
    include_paths: []const std.Build.LazyPath = &.{},
    system_include_paths: []const std.Build.LazyPath = &.{},
    /// libstdx is implicitly linked
    link_libraries: []const *std.Build.Step.Compile = &.{},
    profile: bool = false,
    install_dir: ?[]const u8 = null,
    install_only: bool = false,
    libtesthelpers: ?*std.Build.Step.Compile = null,

    /// Creates a new configuration with a new name, paths, and link libraries
    pub fn with(self: *const ArtifactConfig, name: []const u8, altered: struct {
        include_paths: []const std.Build.LazyPath = &.{},
        system_include_paths: []const std.Build.LazyPath = &.{},
        link_libraries: []const *std.Build.Step.Compile = &.{},
    }) ArtifactConfig {
        return .{
            .name = name,
            .target = self.target,
            .optimize = self.optimize,
            .cxx_flags = self.cxx_flags,
            .cdb_steps = self.cdb_steps,
            .config_h = self.config_h,
            .stdx_dep = self.stdx_dep,
            .auto_install = self.auto_install,
            .include_paths = altered.include_paths,
            .system_include_paths = altered.system_include_paths,
            .link_libraries = altered.link_libraries,
            .profile = self.profile,
            .install_dir = self.install_dir,
            .install_only = self.install_only,
            .libtesthelpers = self.libtesthelpers,
        };
    }
};

const Library = struct {
    const library_root = "lib/";
    const include_root = "include/";
    const src_root = "src/";

    b: *std.Build,
    config_h: *std.Build.Step.ConfigHeader,

    include: []const u8,
    src: []const u8,
    files: []const []const u8,

    step: *std.Build.Step,
    artifact: *std.Build.Step.Compile,

    pub fn init(b: *std.Build, config: ArtifactConfig) Library {
        const include = b.pathJoin(&.{ library_root, config.name, include_root });
        const src = b.pathJoin(&.{ library_root, config.name, src_root });
        const src_paths = stdx.utils.collectFiles(
            b,
            src,
            .{ .allowed_extensions = &.{".cc"} },
        ) catch @panic("OOM");

        var link_libraries: stdx.ArrayList(*std.Build.Step.Compile) = .fromSlice(b, config.link_libraries);
        link_libraries.append(config.stdx_dep.artifact("stdx"));
        
        var include_paths: stdx.ArrayList(std.Build.LazyPath) = .fromSlice(b, config.include_paths);
        include_paths.appendSlice(&.{ b.path(include), b.path(src) });

        const lib = b.addLibrary(.{
            .name = config.name,
            .root_module = stdx.utils.createModule(b, .{
                .target = config.target,
                .optimize = config.optimize,
                .include_paths = include_paths.items(),
                .system_include_paths = config.system_include_paths,
                .cxx = .{
                    .files = src_paths,
                    .flags = config.cxx_flags,
                },
                .config_headers = &.{config.config_h},
                .link_libraries = link_libraries.items(),
            }),
        });
        lib.installHeadersDirectory(b.path(include), "", .{ .include_extensions = &.{".hh"} });
        if (config.cdb_steps) |cdb_steps| cdb_steps.append(&lib.step);
        if (config.auto_install) b.installArtifact(lib);

        return .{
            .b = b,
            .config_h = config.config_h,
            .include = include,
            .src = src,
            .files = stdx.utils.collectFiles(
                b,
                include,
                .{ .allowed_extensions = &.{".hh"}, .extra_files = src_paths },
            ) catch @panic("OOM"),
            .step = &lib.step,
            .artifact = lib,
        };
    }
};

const Test = struct {
    const tests_root = "tests/";

    step: *std.Build.Step,
    artifact: *std.Build.Step.Compile,

    pub fn init(b: *std.Build, config: ArtifactConfig) Test {
        const tests_dir = b.pathJoin(&.{ tests_root, config.name });
        const libstdx = config.stdx_dep.artifact("stdx");

        var link_libraries: stdx.ArrayList(*std.Build.Step.Compile) = .fromSlice(b, config.link_libraries);
        link_libraries.append(libstdx);
        if (config.libtesthelpers) |lib| link_libraries.append(lib);

        var include_paths: stdx.ArrayList(std.Build.LazyPath) = .fromSlice(b, config.include_paths);
        include_paths.appendSlice(&.{ b.path(tests_dir), b.path("tests/helpers") });

        const step_name = b.fmt("test-{s}", .{config.name});
        const desc = b.fmt("Build/run {s} tests", .{config.name});

        const test_artifact = stdx.builders.strappedTest(b, .{
            .target = config.target,
            .optimize = config.optimize,
            .stdx = .{
                .dep = config.stdx_dep
            },
            .cxx_files = stdx.utils.collectFiles(b, tests_dir, .{}) catch @panic("OOM"),
            .cxx_flags = config.cxx_flags,
            .profile = config.profile,
            .include_paths = include_paths.items(),
            .link_libraries = link_libraries.items(),
            .config_headers = &.{config.config_h},
            .executable_config = .{
                .name = config.name,
                .behavior = .{
                    .installable = .{
                        .cmd_name = step_name,
                        .cmd_desc = desc,
                        .install_dir = config.install_dir,
                        .install_only = config.install_only,
                    },
                },
            },
        });
        if (config.cdb_steps) |cdb| cdb.append(&test_artifact.step);

        return .{
            .step = &test_artifact.step,
            .artifact = test_artifact,
        };
    }
};

const Tests = struct {
    support: Test,
    storage: Test,
    wal: Test,
    txn: Test,
    sql: Test,
    exec: Test,
    opt: Test,
    net: Test,
    integration: Test,

    pub fn configure(
        self: *const Tests,
        b: *std.Build,
        install_dir: ?[]const u8,
        install_only: bool,
    ) !void {
        const unit_suites = [_]Test{
            self.support, self.storage, self.wal, self.txn,
            self.sql,     self.exec,    self.opt, self.net,
        };

        const test_step = b.step("test", "Build/run all unit tests");
        for (unit_suites) |suite| {
            _ = stdx.utils.ExecutableBehavior.installArtifact(
                b,
                suite.artifact,
                test_step,
                install_dir,
                install_only,
            );
        }

        const test_all_step = b.step("test-all", "Build/run all tests, including integration");
        test_all_step.dependOn(test_step);

        _ = stdx.utils.ExecutableBehavior.installArtifact(
            b,
            self.integration.artifact,
            test_all_step,
            install_dir,
            install_only,
        );
    }
};

const version_str = zon.version;
const version = std.SemanticVersion.parse(version_str) catch @compileError("Malformed version");

fn addArtifacts(b: *std.Build, config: struct {
    target: ?std.Build.ResolvedTarget = null,
    optimize: std.builtin.OptimizeMode,
    cxx_flags: []const []const u8,
    cdb_steps: ?*stdx.ArrayList(*std.Build.Step),
    exe_override_behavior: ?stdx.utils.ExecutableBehavior = null,
    auto_install: bool = true,
    packaging: bool = false,
    install_tests_only: bool = false,
    stdx_dep: *std.Build.Dependency,
    profile: bool = false,
}) !struct {
    libexec: Library,
    libnet: Library,
    libopt: Library,
    libsql: Library,
    libstorage: Library,
    libsupport: Library,
    libtxn: Library,
    libwal: Library,
    cairnd: *std.Build.Step.Compile,
    cairnctl: *std.Build.Step.Compile,
    tests: ?Tests,
} {
    const target = config.target orelse b.graph.host;
    const config_h = b.addConfigHeader(.{ .include_path = "cairn/config.h" }, .{
        .CAIRN_VERSION_STR = version_str,
        .CAIRN_VERSION_MAJOR = @as(i64, version.major),
        .CAIRN_VERSION_MINOR = @as(i64, version.minor),
        .CAIRN_VERSION_PATCH = @as(i64, version.patch),
        .CAIRN_VERSION_PRE = version.pre orelse "",
        .CAIRN_GIT_INFO = stdx.utils.getGitInfo(b),
        .CAIRN_WINDOWS = target.result.os.tag == .windows,
        .CAIRN_LINUX = target.result.os.tag == .linux,
        .CAIRN_APPLE = target.result.os.tag == .macos,
    });

    const base_lib_config: ArtifactConfig = .{
        .name = undefined,
        .target = target,
        .optimize = config.optimize,
        .cxx_flags = config.cxx_flags,
        .cdb_steps = config.cdb_steps,
        .config_h = config_h,
        .stdx_dep = config.stdx_dep,
        .auto_install = config.auto_install,
        .profile = config.profile,
    };

    const libsupport: Library = .init(b, base_lib_config.with("support", .{}));
    const libstorage: Library = .init(b, base_lib_config.with("storage", .{}));
    const libexec: Library = .init(b, base_lib_config.with("exec", .{}));
    const libnet: Library = .init(b, base_lib_config.with("net", .{}));
    const libopt: Library = .init(b, base_lib_config.with("opt", .{}));
    const libsql: Library = .init(b, base_lib_config.with("sql", .{}));
    const libtxn: Library = .init(b, base_lib_config.with("txn", .{}));
    const libwal: Library = .init(b, base_lib_config.with("wal", .{}));

    const link_libraries = [_]*std.Build.Step.Compile{
        libexec.artifact,                 libnet.artifact,     libopt.artifact, libsql.artifact,
        libstorage.artifact,              libsupport.artifact, libtxn.artifact, libwal.artifact,
        config.stdx_dep.artifact("stdx"),
    };

    const cairnd = stdx.utils.createExecutable(b, .{
        .target = target,
        .optimize = config.optimize,
        .cxx = .{
            .files = &.{"cairn/cairnd.cc"},
            .flags = config.cxx_flags,
        },
        .link_libraries = &link_libraries,
    }, .{
        .name = "cairnd",
        .behavior = config.exe_override_behavior orelse .{
            .installable = .{
                .cmd_name = "run-cairnd",
                .cmd_desc = "Run cairnd with provided command line arguments",
            },
        },
    });
    if (config.auto_install) b.installArtifact(cairnd);
    if (config.cdb_steps) |cdb_steps| cdb_steps.append(&cairnd.step);

    const cairnctl = stdx.utils.createExecutable(b, .{
        .target = target,
        .optimize = config.optimize,
        .cxx = .{
            .files = &.{"cairn/cairnctl.cc"},
            .flags = config.cxx_flags,
        },
        .link_libraries = &link_libraries,
    }, .{
        .name = "cairnctl",
        .behavior = config.exe_override_behavior orelse .{
            .installable = .{
                .cmd_name = "run-cairnctl",
                .cmd_desc = "Run cairnctl with provided command line arguments",
            },
        },
    });
    if (config.auto_install) b.installArtifact(cairnctl);
    if (config.cdb_steps) |cdb_steps| cdb_steps.append(&cairnctl.step);

    var tests: ?Tests = null;
    if (config.target == null) {
        const test_install_dir: ?[]const u8 = if (config.auto_install) "tests" else null;

        var testhelpers_config = base_lib_config.with("testhelpers", .{
            .link_libraries = &.{config.stdx_dep.artifact("catch2")},
        });
        testhelpers_config.auto_install = false;
        const libtesthelpers: Library = .init(b, testhelpers_config);

        const base_test_config: ArtifactConfig = .{
            .name = undefined,
            .target = target,
            .optimize = config.optimize,
            .cxx_flags = config.cxx_flags,
            .cdb_steps = config.cdb_steps,
            .config_h = config_h,
            .stdx_dep = config.stdx_dep,
            .auto_install = config.auto_install,
            .profile = config.profile,
            .install_dir = test_install_dir,
            .install_only = config.install_tests_only,
            .libtesthelpers = libtesthelpers.artifact,
        };

        const test_support: Test = .init(b, base_test_config.with("support", .{}));
        const test_storage: Test = .init(b, base_test_config.with("storage", .{}));
        const test_wal: Test = .init(b, base_test_config.with("wal", .{}));
        const test_txn: Test = .init(b, base_test_config.with("txn", .{}));
        const test_sql: Test = .init(b, base_test_config.with("sql", .{}));
        const test_exec: Test = .init(b, base_test_config.with("exec", .{}));
        const test_opt: Test = .init(b, base_test_config.with("opt", .{}));
        const test_net: Test = .init(b, base_test_config.with("net", .{}));
        const test_integration: Test = .init(b, base_test_config.with("integration", .{}));

        tests = .{
            .support = test_support,
            .storage = test_storage,
            .wal = test_wal,
            .txn = test_txn,
            .sql = test_sql,
            .exec = test_exec,
            .opt = test_opt,
            .net = test_net,
            .integration = test_integration,
        };
        try tests.?.configure(b, test_install_dir, config.install_tests_only);
    }

    return .{
        .libsupport = libsupport,
        .libstorage = libstorage,
        .libexec = libexec,
        .libnet = libnet,
        .libopt = libopt,
        .libsql = libsql,
        .libtxn = libtxn,
        .libwal = libwal,
        .cairnd = cairnd,
        .cairnctl = cairnctl,
        .tests = tests,
    };
}

const counted_extensions = [_][]const u8{ ".cc", ".hh", ".inc", ".zig" };

fn addToolingSteps(b: *std.Build, config: struct {
    cdb_gen: *CDBGenerator,
    cppcheck: *std.Build.Step.Compile,
}) !void {
    const tooling_paths: stdx.steps.FmtPaths = .{
        .cxx = blk: {
            var paths: stdx.ArrayList([]const u8) = .init(b);
            try stdx.utils.collectFilesInto(b, "lib", .{ .allowed_extensions = &.{ ".hh", ".cc" } }, &paths);
            try stdx.utils.collectFilesInto(b, "tests", .{ .allowed_extensions = &.{ ".hh", ".cc" } }, &paths);
            try stdx.utils.collectFilesInto(b, "cairn", .{}, &paths);
            try stdx.utils.collectFilesInto(b, "fuzz", .{}, &paths);
            break :blk paths.items();
        },
        .zig = &.{"build.zig"},
    };

    _ = stdx.steps.addFmt(b, .{
        .paths = tooling_paths,
        .formatter = .{ .version = "21.1.8" },
    }) catch {};

    _ = stdx.steps.addCppcheck(b, .{
        .cppcheck = config.cppcheck,
        .cdb_gen = config.cdb_gen,
    });

    var counted_files: stdx.ArrayList([]const u8) = .init(b);
    counted_files.appendSlice(tooling_paths.cxx);
    counted_files.appendSlice(tooling_paths.zig);
    _ = LOCCounter.init(b, counted_files.items());
}

fn addPackageStep(b: *std.Build, config: struct {
    cxx_flags: []const []const u8,
    compressor: *std.Build.Step.Compile,
}) !void {
    const packager: *stdx.Packager = .init(b, .{
        .compressor = config.compressor,
    });

    for (stdx.Packager.base_target_queries) |query| {
        const target = b.resolveTargetQuery(query);
        const stdx_dep = b.dependency("stdx", .{
            .target = target,
            .optimize = .ReleaseFast,
            .building_for_dep = true,
            .run_cdb_gen = false,
            .packaging = true,
        });

        const artifacts = try addArtifacts(b, .{
            .target = target,
            .optimize = .ReleaseFast,
            .cxx_flags = config.cxx_flags,
            .cdb_steps = null,
            .exe_override_behavior = .standalone,
            .auto_install = false,
            .packaging = true,
            .stdx_dep = stdx_dep,
        });

        stdx.Packager.configureExe(b, target, version_str, artifacts.cairnd);
        stdx.Packager.configureExe(b, target, version_str, artifacts.cairnctl);

        const package_artifact_dirname = b.fmt("cairn-{s}-{s}", .{ try query.zigTriple(b.allocator), version_str });
        const copy_paths = [_]stdx.Packager.CopyPath{
            .{ .source = artifacts.cairnd.getEmittedBin(), .destination = b.fmt("bin/{s}", .{artifacts.cairnd.out_filename}) },
            .{ .source = artifacts.cairnctl.getEmittedBin(), .destination = b.fmt("bin/{s}", .{artifacts.cairnctl.out_filename}) },
            .{ .source = b.path("LICENSE"), .destination = "LICENSE" },
            .{ .source = b.path("README.md"), .destination = "README.md" },
            .{ .source = b.path(".github/CHANGELOG.md"), .destination = "CHANGELOG.md" },
        };

        packager.addArchives(.{
            .target = target,
            .copy_paths = &copy_paths,
            .output_dir_basename = package_artifact_dirname,
        });
    }
}
