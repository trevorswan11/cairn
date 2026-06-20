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

    var compiler_flags: std.ArrayList([]const u8) = .empty;
    try compiler_flags.appendSlice(b.allocator, &stdx.utils.base_cxx_flags);
    try compiler_flags.append(b.allocator, "-DMAGIC_ENUM_RANGE_MAX=255");
    const dist_flags: []const []const u8 = &.{ "-DNDEBUG", "-DCAIRN_DIST" };

    var package_flags = try compiler_flags.clone(b.allocator);
    try package_flags.appendSlice(b.allocator, dist_flags);

    try compiler_flags.appendSlice(b.allocator, &.{
        "-gen-cdb-fragment-path",
        b.cache_root.join(b.allocator, &.{CDBGenerator.cdb_frags_dirname}) catch @panic("OOM"),
    });

    switch (optimize) {
        .Debug => try compiler_flags.appendSlice(b.allocator, &.{ "-g", "-DCAIRN_DEBUG" }),
        .ReleaseSafe => try compiler_flags.appendSlice(b.allocator, &.{"-DCAIRN_RELEASE"}),
        .ReleaseFast, .ReleaseSmall => try compiler_flags.appendSlice(b.allocator, dist_flags),
    }

    const install_tests_only = b.option(
        bool,
        "install-tests-only",
        "Install tests without running them (default: false)",
    ) orelse false;

    const cdb_gen: *CDBGenerator = .init(b);
    var cdb_steps: std.ArrayList(*std.Build.Step) = .empty;
    const artifacts = try addArtifacts(b, .{
        .optimize = optimize,
        .cxx_flags = compiler_flags.items,
        .cdb_steps = &cdb_steps,
        .install_tests_only = install_tests_only,
        .stdx_dep = stdx_dep,
        .profile = profile,
    });
    for (cdb_steps.items) |cdb_step| cdb_gen.step.dependOn(cdb_step);
    _ = artifacts;

    const cppcheck = stdx_dep.artifact("cppcheck");
    try addToolingSteps(b, .{
        .cdb_gen = cdb_gen,
        .cppcheck = cppcheck,
    });

    try addPackageStep(b, .{
        .cxx_flags = package_flags.items,
        .compressor = stdx_dep.artifact("compressor"),
    });
}

const ProjectPaths = struct {
    const Project = struct {
        inc: []const u8,
        src: []const u8,
        tests: []const u8,

        pub fn files(self: *const Project, b: *std.Build) ![][]const u8 {
            return std.mem.concat(b.allocator, []const u8, &.{
                try stdx.utils.collectFiles(b, self.inc, .{ .allowed_extensions = &.{".hh"} }),
                try stdx.utils.collectFiles(b, self.src, .{ .allowed_extensions = &.{".cc"} }),
                try stdx.utils.collectFiles(b, self.tests, .{ .allowed_extensions = &.{ ".hh", ".cc" } }),
            });
        }
    };
};

const version_str = zon.version;
const version = std.SemanticVersion.parse(version_str) catch @compileError("Malformed version");

const TestArtifacts = struct {};

const Artifacts = struct {
    libsupport: *std.Build.Step.Compile,
    libcompiler: *std.Build.Step.Compile,
    libdriver: *std.Build.Step.Compile,
    cairn: *std.Build.Step.Compile,
    cairnctl: *std.Build.Step.Compile,
    tests: ?TestArtifacts,
};

fn addArtifacts(b: *std.Build, config: struct {
    target: ?std.Build.ResolvedTarget = null,
    optimize: std.builtin.OptimizeMode,
    cxx_flags: []const []const u8,
    cdb_steps: ?*std.ArrayList(*std.Build.Step),
    behavior: ?stdx.utils.ExecutableBehavior = null,
    auto_install: bool = true,
    packaging: bool = false,
    install_tests_only: bool = true,
    stdx_dep: *std.Build.Dependency,
    profile: bool = false,
}) !Artifacts {
    _ = b;
    _ = config;
    unreachable;
}

const counted_extensions = [_][]const u8{ ".cc", ".hh", ".inc", ".zig" };

fn addToolingSteps(b: *std.Build, config: struct {
    cdb_gen: *CDBGenerator,
    cppcheck: *std.Build.Step.Compile,
}) !void {
    _ = b;
    _ = config;
}

const target_queries = [_]std.Target.Query{
    .{ .cpu_arch = .x86_64, .os_tag = .macos },
    .{ .cpu_arch = .aarch64, .os_tag = .macos },

    .{ .cpu_arch = .x86, .os_tag = .linux },
    .{ .cpu_arch = .x86_64, .os_tag = .linux },
    .{ .cpu_arch = .aarch64, .os_tag = .linux },
    .{ .cpu_arch = .powerpc, .os_tag = .linux },
    .{ .cpu_arch = .powerpc64, .os_tag = .linux },
    .{ .cpu_arch = .powerpc64le, .os_tag = .linux },
    .{ .cpu_arch = .riscv32, .os_tag = .linux },
    .{ .cpu_arch = .riscv64, .os_tag = .linux },
    .{ .cpu_arch = .loongarch64, .os_tag = .linux },

    .{ .cpu_arch = .x86_64, .os_tag = .freebsd },
    .{ .cpu_arch = .aarch64, .os_tag = .freebsd },
    .{ .cpu_arch = .powerpc64, .os_tag = .freebsd },
    .{ .cpu_arch = .powerpc64le, .os_tag = .freebsd },
    .{ .cpu_arch = .riscv64, .os_tag = .freebsd },

    .{ .cpu_arch = .x86, .os_tag = .netbsd },
    .{ .cpu_arch = .x86_64, .os_tag = .netbsd },
    .{ .cpu_arch = .aarch64, .os_tag = .netbsd },

    .{ .cpu_arch = .x86, .os_tag = .windows },
    .{ .cpu_arch = .x86_64, .os_tag = .windows },
    .{ .cpu_arch = .aarch64, .os_tag = .windows },
};

fn addPackageStep(b: *std.Build, config: struct {
    cxx_flags: []const []const u8,
    compressor: *std.Build.Step.Compile,
}) !void {
    _ = b;
    _ = config;
}
