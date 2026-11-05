"""Inyecta FIRMWARE_VERSION desde version.txt (unica fuente de la version)."""
import os
Import("env")  # noqa: F821  (lo provee PlatformIO/SCons)

path = os.path.join(env["PROJECT_DIR"], "version.txt")  # noqa: F821
with open(path, encoding="utf-8") as f:
    version = f.read().strip()

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version))])  # noqa: F821
print("FIRMWARE_VERSION =", version)
