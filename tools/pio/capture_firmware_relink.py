"""Capture the shipping S3 build graph for a reproducible private relink bundle.

Loaded as a pre: script so public source names reach Arduino's cloned compiler
environments. The explicit relink-info target reads existing products only.
Its absolute paths are internal metadata; the bundle producer owns sanitizing.
The framework_archives field also contains the linked NeoPixelBus LGPL library.
application_rebuild captures application objects depending on NeoPixelBus so
the bundle can recompile consumers when a modified library changes its layout.
project_sources captures the original firmware's wider MPL source-delivery set.
"""

import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ENVIRONMENT = "litch-esp32s3"
MAX_NODES = 20000
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_HASH_BYTES = 4 * 1024 * 1024 * 1024
MAX_METADATA_BYTES = 16 * 1024 * 1024


def public_path_maps(project_dir, framework_dir, libdeps_dir):
    # GCC applies the last matching mapping, so narrower roots follow project.
    return [
        {"source": str(Path(root).resolve()), "replacement": replacement}
        for root, replacement in (
            (project_dir, "project"),
            (framework_dir, "arduino"),
            (libdeps_dir, "libraries"),
        )
    ]


def map_load_paths(text, cwd):
    """GNU ld's LOAD records include implicit compiler runtime objects/libs."""
    result = set()
    for line in text.splitlines():
        if line.startswith("LOAD "):
            path = line[5:].strip()
            if not path or "\x00" in path:
                raise ValueError("invalid linker map LOAD record")
            result.add(str((Path(cwd) / path).resolve()))
    if not result:
        raise ValueError("linker map contains no LOAD records")
    return sorted(result)


def node_path(node):
    # PlatformIO's duplicate=False VariantDir nodes name a build-directory
    # source which does not exist on disk; SCons compiles its real srcnode.
    if not node.is_derived():
        node = node.srcnode()
    return str(Path(node.get_abspath()).resolve())


def file_nodes(nodes):
    return [node for node in nodes if hasattr(node, "get_abspath")]


def descendants(nodes):
    """Scan SCons dependencies without scheduling or executing any builder."""
    pending = list(nodes)
    seen = {}
    while pending:
        node = pending.pop()
        if not hasattr(node, "get_abspath"):
            continue
        path = node_path(node)
        if path in seen:
            continue
        if len(seen) >= MAX_NODES:
            raise ValueError("relink dependency graph exceeds its node bound")
        seen[path] = node
        pending.extend(node.children())
    return seen


def command_record(node):
    """Use the executor's own overrides and SCons command substitution."""
    from SCons.Action import CommandGeneratorAction, ListAction
    from SCons.Platform import TempFileMunge
    from SCons.Subst import escape_list

    class InlineResponseFile(TempFileMunge):
        def __call__(self, target, source, env, for_signature):
            # Match SCons' compilation database: expose the full command without
            # creating an ephemeral response file when the linker line is long.
            return self.cmd

    executor = node.get_executor(create=False)
    if executor is None:
        raise ValueError(f"missing build executor: {node}")
    build_env = executor.get_build_env().Override({"TEMPFILE": InlineResponseFile})
    targets = executor.get_all_targets()
    sources = executor.get_all_sources()
    commands = []

    def expand(action, depth=0):
        if depth > 8:
            raise ValueError("relink action nesting exceeds its bound")
        if isinstance(action, ListAction):
            for child in action.list:
                expand(child, depth + 1)
        elif isinstance(action, CommandGeneratorAction):
            # SCons uses the same generator to resolve LazyAction/$*COM values.
            expand(action._generate(targets, sources, build_env, False, executor), depth + 1)
        elif hasattr(action, "process"):
            lines, ignore_errors, _silent = action.process(
                targets, sources, build_env, executor=executor
            )
            if ignore_errors:
                raise ValueError(f"unchecked command cannot certify a relink: {node}")
            for line in lines:
                # Match the POSIX shell decoding used by SCons, including quoted
                # macro definitions and spaces in include/source paths.
                shell_words = escape_list(line, build_env.get("ESCAPE", lambda value: value))
                argv = shlex.split(" ".join(shell_words), posix=True)
                if not argv:
                    continue
                if any(word in ("&&", "||", ";", "|", ">", "<") for word in argv):
                    raise ValueError(f"shell pipeline is not a relink argv: {node}")
                if any(word.startswith("@") for word in argv):
                    raise ValueError(f"unexpanded response file in relink command: {node}")
                tool = build_env.WhereIs(argv[0])
                if tool:
                    argv[0] = str(Path(tool).resolve())
                commands.append(argv)
        # Function pre/post hooks are not compiler/linker argv and are not run.

    for action in executor.get_action_list():
        expand(action)
    if not commands:
        raise ValueError(f"no command action for {node}")
    return {
        "commands": commands,
        "targets": [node_path(item) for item in file_nodes(targets)],
        "sources": [node_path(item) for item in file_nodes(sources)],
    }


def single_command(node):
    record = command_record(node)
    if len(record["commands"]) != 1:
        raise ValueError(f"expected one compiler/link command: {node}")
    record["argv"] = record.pop("commands")[0]
    return record


class InputFiles:
    def __init__(self):
        self.records = {}
        self.total_bytes = 0

    def add(self, filename):
        path = Path(filename).resolve()
        key = str(path)
        if key in self.records:
            return self.records[key]
        if len(self.records) >= MAX_NODES or not path.is_file():
            raise ValueError(f"missing or excessive relink input: {path}")
        size = path.stat().st_size
        if size > MAX_FILE_BYTES or self.total_bytes + size > MAX_HASH_BYTES:
            raise ValueError("relink inputs exceed their byte bound")
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        self.total_bytes += size
        record = {"path": key, "size": size, "sha256": digest.hexdigest()}
        self.records[key] = record
        return record


def compile_record(obj, files, project, dependencies=None):
    sources = file_nodes(obj.sources)
    if len(sources) != 1:
        raise ValueError(f"ambiguous compile sources: {obj}")
    source_path = node_path(sources[0])
    object_path = node_path(obj)
    depfile = Path(object_path).with_suffix(".d")
    command = single_command(obj)
    if dependencies is None:
        dependencies = compiler_dependencies(depfile, project, object_path, source_path)
    for dependency in dependencies:
        files.add(dependency)
    source_record = files.add(source_path)
    object_record = files.add(object_path)
    return {
        "source": source_path, "object": object_path,
        "argv": command["argv"], "dependencies": dependencies,
        "source_sha256": source_record["sha256"],
        "object_sha256": object_record["sha256"],
        "dependency_file": files.add(depfile),
    }


def compiler_dependencies(depfile, project, object_path, source_path):
    """Read GCC's actual includes, not SCons' conditional-header superset."""
    if not depfile.is_file() or depfile.stat().st_size > MAX_METADATA_BYTES:
        raise ValueError(f"missing or oversized compiler dependency file: {depfile}")
    text = depfile.read_text(encoding="utf-8").replace("\\\r\n", " ").replace("\\\n", " ")
    # The rule separator is the first unescaped colon. A literal colon in a
    # POSIX path is Make-escaped, like spaces and '#' in dependency operands.
    escaped = False
    separator = None
    for index, character in enumerate(text):
        if character == ":" and not escaped:
            separator = index
            break
        escaped = character == "\\" and not escaped
    if separator is None:
        raise ValueError(f"invalid compiler dependency rule: {depfile}")
    targets = shlex.split(text[:separator], posix=True)
    dependencies = shlex.split(text[separator + 1:], posix=True)
    target_paths = {str((project / path).resolve()) for path in targets}
    paths = sorted({str((project / path).resolve()) for path in dependencies})
    if target_paths != {object_path} or source_path not in paths:
        raise ValueError(f"compiler dependency rule does not match its object/source: {depfile}")
    if len(paths) > MAX_NODES or any(not Path(path).is_file() for path in paths):
        raise ValueError(f"compiler dependency rule names missing or excessive inputs: {depfile}")
    return paths


def complete_compile_dependencies(record, project_dir, temporary_dir):
    """Producer-only preprocessing includes system headers omitted by -MMD.

    Keep the captured compiler's defines, include order and language/target
    options. Only output/dependency actions change; existing products and the
    captured record are untouched. relink-info never calls this function.
    """
    project = Path(project_dir).resolve()
    object_path = str((project / record["object"]).resolve())
    source_path = str((project / record["source"]).resolve())
    original = record["argv"]
    if not original or len(original) > MAX_NODES or "-c" not in original:
        raise ValueError("expected a bounded captured compiler command")
    argv = [original[0]]
    operands = {"-o", "-MF", "-MT", "-MQ", "-MJ"}
    actions = {"-c", "-S", "-E", "-M", "-MM", "-MD", "-MMD", "-MP", "-MG"}
    index = 1
    while index < len(original):
        argument = original[index]
        if argument in operands:
            if index + 1 == len(original):
                raise ValueError(f"missing captured compiler option operand: {argument}")
            index += 2
            continue
        if argument.startswith(("@", "-save-temps", "-Wp,-M")):
            raise ValueError("unsupported compiler output/response-file option")
        if argument not in actions and not any(
            argument.startswith(option) and argument != option for option in operands
        ):
            argv.append(argument)
        index += 1
    with tempfile.TemporaryDirectory(prefix="compiler-deps-", dir=temporary_dir) as directory:
        depfile = Path(directory) / "dependencies.d"
        # -MQ is -MT with Make quoting, so paths containing spaces stay one target.
        argv.extend(["-M", "-MF", str(depfile), "-MQ", object_path])
        with (Path(directory) / "stderr.txt").open("w+b") as errors:
            try:
                result = subprocess.run(
                    argv, cwd=project, stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=errors, timeout=60, check=False,
                )
            except subprocess.TimeoutExpired as error:
                raise ValueError(f"compiler dependency pass timed out: {source_path}") from error
            if result.returncode:
                errors.seek(0)
                detail = errors.read(16384).decode("utf-8", errors="replace")
                raise ValueError(f"compiler dependency pass failed: {source_path}\n{detail}")
        return compiler_dependencies(depfile, project, object_path, source_path)


def application_rebuild_records(graph, project, library_header, files):
    records = []
    for path, obj in sorted(graph.items()):
        if Path(path).suffix != ".o" or not obj.has_builder():
            continue
        sources = file_nodes(obj.sources)
        if not any(Path(node_path(source)).is_relative_to(project / "src") for source in sources):
            continue
        if len(sources) != 1:
            raise ValueError(f"ambiguous application compile sources: {obj}")
        source_path = node_path(sources[0])
        if Path(source_path).suffix.lower() not in (".c", ".cc", ".cpp", ".cxx", ".c++"):
            # Assembly sources do not instantiate NeoPixelBus's C++ templates
            # and the platform's assembler does not emit GCC dependency files.
            continue
        depfile = Path(path).with_suffix(".d")
        dependencies = compiler_dependencies(depfile, project, path, source_path)
        if str(library_header.resolve()) in dependencies:
            records.append(compile_record(obj, files, project, dependencies))
    return records


def project_source_records(graph, project, files):
    """Bind application and selected library source without rebuild actions."""
    roots = (project / "src", project / "lib/foxie_embedded/src",
             project / "lib/litch-max/src", project / "lib/litch-max/vendor/mcu-max/src")
    vendor_roots = (project / "lib/SparkFun_Ambient_Light_Sensor_Arduino_Library",
                    project / "lib/PCA95x5")
    records = []
    for path, obj in sorted(graph.items()):
        if Path(path).suffix != ".o" or not obj.has_builder():
            continue
        sources = file_nodes(obj.sources)
        for source in sources:
            source_path = Path(node_path(source))
            if (any(source_path.is_relative_to(project / owner) for owner in ("src", "lib"))
                    and not any(source_path.is_relative_to(root) for root in roots + vendor_roots)):
                raise ValueError(f"unreviewed local compiled source owner: {source_path}")
        if not any(Path(node_path(source)).is_relative_to(root)
                   for source in sources for root in roots):
            continue
        if len(sources) != 1:
            raise ValueError(f"ambiguous original compile source: {obj}")
        source = node_path(sources[0])
        if Path(source).suffix in (".S", ".s"):
            # PlatformIO's assembler emits no .d; retain its source-node graph.
            dependencies = [name for name in descendants(sources) if Path(name).is_file()]
        else:
            depfile = Path(path).with_suffix(".d")
            dependencies = compiler_dependencies(depfile, project, path, source)
            files.add(depfile)
        selected = sorted(name for name in dependencies
                          if Path(name).is_relative_to(project)
                          and not Path(name).is_relative_to(project / ".pio"))
        for name in selected:
            files.add(name)
        files.add(path)
        records.append({"source": source, "object": path, "dependencies": selected})
    if not records:
        raise ValueError("relink graph contains no original firmware sources")
    return records


def capture(env, path_maps):
    if env.subst("$PIOENV") != ENVIRONMENT or os.name != "posix":
        raise ValueError("relink-info requires the shipping S3 environment on a POSIX host")
    project = Path(env.subst("$PROJECT_DIR")).resolve()
    build = Path(env.subst("$BUILD_DIR")).resolve()
    framework = Path(path_maps[1]["source"])
    rebuildable_roots = (
        framework / "cores", framework / "libraries",
        Path(path_maps[2]["source"]) / "NeoPixelBus" / "src",
    )
    name = env.subst("$PROGNAME")
    elf = env.File(str(build / (name + ".elf")))
    binary = env.File(str(build / (name + ".bin")))
    map_path = build / (name + ".map")
    files = InputFiles()
    artifacts = {"elf": files.add(node_path(elf)), "bin": files.add(node_path(binary))}
    map_record = files.add(map_path)
    link = single_command(elf)
    elf_to_bin = single_command(binary)
    graph = descendants([elf, binary])
    # A metadata-only alias must never claim newly configured commands built
    # stale objects. Its failure asks the build owner to update the normal lane.
    for path, node in graph.items():
        if node.has_builder() and Path(path).suffix in (".o", ".a", ".elf", ".bin"):
            if not Path(path).is_file() or node.changed():
                raise ValueError(f"build product is stale; build before relink-info: {path}")

    loaded = map_load_paths(map_path.read_text(encoding="utf-8"), project)
    link["inputs"] = sorted(set(loaded) | {
        node_path(node) for node in file_nodes(elf.children())
        if Path(node_path(node)).is_file()
    })
    for path in link["inputs"]:
        files.add(path)

    archives = []
    for path in loaded:
        if not path.endswith(".a"):
            continue
        archive = env.File(path)
        if not archive.has_builder():
            continue
        objects = []
        for obj in file_nodes(archive.sources):
            sources = file_nodes(obj.sources)
            selected = [source for source in sources if any(
                Path(node_path(source)).is_relative_to(root) for root in rebuildable_roots
            )]
            if not selected:
                continue
            if len(selected) != 1 or len(sources) != 1:
                raise ValueError(f"ambiguous LGPL compile sources: {obj}")
            objects.append(compile_record(obj, files, project))
        if objects:
            archive_record = dict(files.add(path))
            archive_record["commands"] = command_record(archive)["commands"]
            archive_record["objects"] = sorted(objects, key=lambda item: item["object"])
            archives.append(archive_record)
    if not archives or not any(Path(item["source"]).is_relative_to(framework / "cores")
                               for archive in archives for item in archive["objects"]):
        raise ValueError("relink graph contains no Arduino core compile actions")

    application_rebuild = application_rebuild_records(
        graph, project, rebuildable_roots[2] / "NeoPixelBus.h", files
    )
    project_sources = project_source_records(graph, project, files)
    renderer_source = str(project / "src" / "neopixel_led_renderer.cpp")
    if not any(item["source"] == renderer_source for item in application_rebuild):
        raise ValueError("relink graph contains no NeoPixelBus application renderer")

    commands = [link["argv"], elf_to_bin["argv"]]
    for archive in archives:
        commands.extend(archive["commands"])
        commands.extend(item["argv"] for item in archive["objects"])
    commands.extend(item["argv"] for item in application_rebuild)
    for argv in commands:
        for token in argv:
            # Capture executable/script operands, without recording ENV/secrets.
            operand = Path(token)
            if operand.is_absolute() and operand.is_file():
                files.add(operand)
    files.add(project / "platformio.ini")
    # SCons executes extra scripts without defining Python's __file__.
    files.add(project / "tools" / "pio" / "capture_firmware_relink.py")
    metadata = {
        "schema_version": 1, "environment": ENVIRONMENT,
        "project_dir": str(project), "build_dir": str(build),
        "framework_dir": str(framework), "cwd": str(project),
        "path_maps": path_maps, "artifacts": artifacts, "linker_map": map_record,
        "link": link, "elf_to_bin": elf_to_bin,
        "framework_archives": sorted(archives, key=lambda item: item["path"]),
        "application_rebuild": application_rebuild,
        "project_sources": project_sources,
        "inputs": [files.records[path] for path in sorted(files.records)],
    }
    encoded = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(encoded) > MAX_METADATA_BYTES:
        raise ValueError("relink metadata exceeds its byte bound")
    destination = build / "firmware_relink.json"
    temporary = destination.with_suffix(".json.tmp")
    try:
        temporary.write_bytes(encoded)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Captured relink commands for {len(archives)} LGPL archives: {destination}")


def install(env):
    from SCons.Errors import UserError
    from SCons.Script import COMMAND_LINE_TARGETS

    if env.subst("$PIOENV") != ENVIRONMENT:
        if "relink-info" in COMMAND_LINE_TARGETS:
            raise UserError("relink-info supports only the shipping S3 environment")
        return
    framework = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if not framework:
        raise UserError("Arduino framework package is required for public source paths")
    mappings = public_path_maps(
        env.subst("$PROJECT_DIR"), framework,
        env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV"),
    )
    env.AppendUnique(CCFLAGS=[
        f"-ffile-prefix-map={item['source']}={item['replacement']}" for item in mappings
    ])

    def collect(source, target, env):
        del source, target
        try:
            capture(env, mappings)
        except (OSError, ValueError) as error:
            raise UserError(str(error)) from error

    env.AddCustomTarget(
        "relink-info", [], collect,
        title="Capture shipping firmware relink metadata",
        description="Read current S3 products and build commands without rebuilding",
    )


if "Import" in globals():
    Import("env")  # type: ignore[name-defined]  # SCons injects this entry point.
    install(env)  # type: ignore[name-defined]
