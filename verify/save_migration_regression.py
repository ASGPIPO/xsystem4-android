#!/usr/bin/env python3
"""Run the production Android save migration method on real temporary files.

Requires JDK 17 and Kotlin 1.7.20 jars already in the Gradle cache.
No Android, network, game data, or display is used. Missing prerequisites fail.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "project/app/src/main/java/io/github/kichikuou/xsystem4/GameList.kt"
text = SOURCE.read_text()
matches = re.findall(
    r"(?ms)^    private fun moveSaveDirectories\(\) \{\n.*?^    \}", text
)
if len(matches) != 1:
    raise RuntimeError("Expected exactly one production moveSaveDirectories method")
method = matches[0]
print(f"Production source: {SOURCE}", flush=True)
print(f"Method SHA256: {hashlib.sha256(method.encode()).hexdigest()}", flush=True)
if os.stat("/tmp").st_dev == os.stat("/dev/shm").st_dev:
    raise RuntimeError("Cross-device regression requires different /tmp and /dev/shm devices")
print(
    f"Cross-device fixture: /dev/shm dev={os.stat('/dev/shm').st_dev}, "
    f"/tmp dev={os.stat('/tmp').st_dev}", flush=True
)

cache = Path(os.environ.get("GRADLE_USER_HOME", str(Path.home() / ".gradle")))
cache /= "caches/modules-2/files-2.1"
def cached_jar(group, artifact, version):
    jars = sorted((cache / group / artifact / version).glob(f"*/{artifact}-{version}.jar"))
    if len(jars) != 1:
        raise RuntimeError(f"Expected one cached {artifact}-{version}.jar, found {jars}")
    return str(jars[0])

stdlib = cached_jar("org.jetbrains.kotlin", "kotlin-stdlib", "1.7.20")
compiler_cp = [
    cached_jar("org.jetbrains.kotlin", artifact, "1.7.20")
    for artifact in (
        "kotlin-compiler-embeddable", "kotlin-stdlib", "kotlin-reflect",
        "kotlin-script-runtime",
    )
]
compiler_cp.append(cached_jar("org.jetbrains.intellij.deps", "trove4j", "1.0.20200330"))
compiler_cp.append(cached_jar("org.jetbrains", "annotations", "13.0"))
java = Path(os.environ.get("JAVA_HOME", "/opt/jdk-17")) / "bin/java"
if not java.is_file():
    raise RuntimeError(f"Missing JDK java: {java}")

harness = r"""
@file:Suppress("UNUSED_PARAMETER")
import java.io.File
import java.io.IOException
import java.nio.file.Files
import kotlin.system.exitProcess

data class Item(val name: String, val path: File, val homedir: File, val error: String? = null)
object Log {
    val warnings = arrayListOf<String>()
    fun i(tag: String, message: String): Int = 0
    fun w(tag: String, message: String): Int {
        warnings.add(message)
        return 0
    }
    fun w(tag: String, message: String, error: Throwable): Int = w(tag, message)
}
class GameList(private val items: List<Item>) {
    fun migrate() = moveSaveDirectories()
__PRODUCTION_METHOD__
}

fun payload(seed: Int) = ByteArray(257) { ((it * 31 + seed) % 256).toByte() }
fun save(base: File, name: String, bytes: ByteArray): File {
    val file = File(base, name)
    check(file.parentFile.mkdirs() || file.parentFile.isDirectory)
    file.writeBytes(bytes)
    return file
}
fun expectBytes(file: File, expected: ByteArray) {
    check(file.isFile) { "Missing save: $file" }
    check(file.readBytes().contentEquals(expected)) { "Save bytes changed: $file" }
}
fun expectWarning() {
    check(Log.warnings.isNotEmpty()) { "Migration failure/conflict must warn" }
}
fun temporary(parent: String = "/tmp", run: (File) -> Unit) {
    val root = Files.createTempDirectory(File(parent).toPath(), "save-migration-test-").toFile()
    try {
        run(root)
    } finally {
        check(root.deleteRecursively()) { "Could not clean private fixture: $root" }
    }
}
fun item(root: File): Item {
    val homedir = File(root, "legacy")
    val destination = File(root, "installed")
    check(destination.mkdirs())
    return Item("fixture-game", destination, homedir)
}
fun oldPath(item: Item) = File(item.homedir, item.name)
fun migrate(item: Item) = GameList(listOf(item)).migrate()

fun main() {
    var passed = 0
    var failed = 0
    fun test(name: String, run: () -> Unit) {
        Log.warnings.clear()
        try {
            run()
            passed++
            println("PASS $name")
        } catch (error: Throwable) {
            failed++
            println("FAIL $name: ${error.javaClass.simpleName}: ${error.message}")
        }
    }

    test("same-name destination preserves both save byte streams") {
        temporary { root ->
            val game = item(root)
            val legacy = save(oldPath(game), "SaveData/00.asd", payload(1))
            val current = save(game.path, "SaveData/00.asd", payload(2))
            migrate(game)
            expectBytes(current, payload(2))
            expectBytes(legacy, payload(1))
            expectWarning()
        }
    }
    test("nonconflicting migration moves bytes and removes empty legacy container") {
        temporary { root ->
            val game = item(root)
            save(oldPath(game), "SaveData/00.asd", payload(3))
            save(oldPath(game), "config.dat", payload(4))
            migrate(game)
            expectBytes(File(game.path, "SaveData/00.asd"), payload(3))
            expectBytes(File(game.path, "config.dat"), payload(4))
            check(!oldPath(game).exists()) { "Empty legacy container was not removed" }
            check(Log.warnings.isEmpty()) { "Successful migration warned" }
        }
    }
    test("real cross-device rename failure preserves the legacy save") {
        temporary("/dev/shm") { sourceRoot ->
            temporary { destinationRoot ->
                val probeSource = save(sourceRoot, "probe", payload(5))
                val probeTarget = File(destinationRoot, "probe")
                check(!probeSource.renameTo(probeTarget)) { "Expected real cross-device rename failure" }
                expectBytes(probeSource, payload(5))
                check(!probeTarget.exists()) { "Failed rename created destination" }
                val game = Item("fixture-game", destinationRoot, sourceRoot)
                val legacy = save(oldPath(game), "SaveData/00.asd", payload(6))
                migrate(game)
                expectBytes(legacy, payload(6))
                check(!File(game.path, "SaveData").exists()) { "Failed rename created save directory" }
                expectWarning()
            }
        }
    }
    test("partial migration retains skipped saves and repeated migration is idempotent") {
        temporary { root ->
            val game = item(root)
            val legacy = save(oldPath(game), "SaveData/00.asd", payload(7))
            val current = save(game.path, "SaveData/00.asd", payload(8))
            save(oldPath(game), "extra-save/01.asd", payload(9))
            migrate(game)
            expectBytes(File(game.path, "extra-save/01.asd"), payload(9))
            check(!File(oldPath(game), "extra-save").exists()) { "Movable source remained" }
            expectBytes(legacy, payload(7))
            expectBytes(current, payload(8))
            expectWarning()
            Log.warnings.clear()
            migrate(game)
            expectBytes(legacy, payload(7))
            expectBytes(current, payload(8))
            expectBytes(File(game.path, "extra-save/01.asd"), payload(9))
            expectWarning()
        }
    }
    test("source equal to destination preserves saves") {
        temporary { root ->
            val homedir = File(root, "legacy")
            val directory = File(homedir, "fixture-game")
            val file = save(directory, "SaveData/00.asd", payload(10))
            migrate(Item("fixture-game", directory, homedir))
            expectBytes(file, payload(10))
        }
    }
    test("destination symlink alias preserves source saves") {
        temporary { root ->
            val homedir = File(root, "legacy")
            val directory = File(homedir, "fixture-game")
            val file = save(directory, "SaveData/00.asd", payload(11))
            val alias = File(root, "installed-alias")
            Files.createSymbolicLink(alias.toPath(), directory.toPath())
            migrate(Item("fixture-game", alias, homedir))
            expectBytes(file, payload(11))
            check(alias.isDirectory) { "Destination alias was broken" }
        }
    }
    test("invalid canonical destination preserves legacy saves without crashing") {
        temporary { root ->
            val homedir = File(root, "legacy")
            val directory = File(homedir, "fixture-game")
            val file = save(directory, "SaveData/00.asd", payload(12))
            val invalid = File(root, "destination" + 0.toChar())
            var canonicalFailed = false
            try {
                invalid.canonicalFile
            } catch (error: IOException) {
                canonicalFailed = true
            }
            check(canonicalFailed) { "Expected real canonical path resolution failure" }
            migrate(Item("fixture-game", invalid, homedir))
            expectBytes(file, payload(12))
            expectWarning()
        }
    }
    test("empty directory equal to destination is not removed") {
        temporary { root ->
            val homedir = File(root, "legacy")
            val directory = File(homedir, "fixture-game")
            check(directory.mkdirs())
            migrate(Item("fixture-game", directory, homedir))
            check(directory.isDirectory) { "Migration removed its destination" }
        }
    }
    println("RESULT passed=$passed failed=$failed")
    if (failed != 0) exitProcess(1)
}
""".replace("__PRODUCTION_METHOD__", method)

with tempfile.TemporaryDirectory(prefix="save-migration-jvm-") as workdir:
    work = Path(workdir)
    fixture = work / "MigrationHarness.kt"
    fixture.write_text(harness)
    output = work / "classes"
    subprocess.run([
        str(java), "-cp", os.pathsep.join(compiler_cp),
        "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
        "-no-stdlib", "-no-reflect", "-classpath", stdlib,
        "-d", str(output), str(fixture),
    ], check=True)
    result = subprocess.run([
        str(java), "-cp", os.pathsep.join((str(output), stdlib)), "MigrationHarnessKt",
    ])
    raise SystemExit(result.returncode)

