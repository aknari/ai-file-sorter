#!/usr/bin/env bash
#
# Stage a headless AI File Sorter engine bundle for macOS.
#
# The headless target links QtCore/QtNetwork only, so this script starts from the
# desktop bundle's already-staged frameworks, swaps in the headless binary, and
# removes the GUI stack (QtWidgets, QtGui, QtDBus, platform/style/image plugins).
#
# Usage: stage_headless_macos.sh <headless-binary> <base-app-bundle> <output-app-bundle>

set -euo pipefail

HEADLESS_BIN="${1:?usage: stage_headless_macos.sh <headless-binary> <base-app-bundle> <output-app-bundle>}"
BASE_BUNDLE="${2:?missing base app bundle}"
OUT_BUNDLE="${3:?missing output app bundle}"

if [ ! -x "$HEADLESS_BIN" ]; then
    echo "error: headless binary not found or not executable: $HEADLESS_BIN" >&2
    exit 1
fi
if [ ! -d "$BASE_BUNDLE/Contents/Frameworks" ]; then
    echo "error: base bundle has no Contents/Frameworks: $BASE_BUNDLE" >&2
    exit 1
fi

rm -rf "$OUT_BUNDLE"
mkdir -p "$OUT_BUNDLE/Contents/MacOS" "$OUT_BUNDLE/Contents/Frameworks" "$OUT_BUNDLE/Contents/Resources"

cp "$BASE_BUNDLE/Contents/Info.plist" "$OUT_BUNDLE/Contents/Info.plist"
if [ -d "$BASE_BUNDLE/Contents/Resources" ]; then
    cp -R "$BASE_BUNDLE/Contents/Resources/." "$OUT_BUNDLE/Contents/Resources/"
fi
cp -R "$BASE_BUNDLE/Contents/Frameworks/." "$OUT_BUNDLE/Contents/Frameworks/"
chmod -R u+w "$OUT_BUNDLE"

BIN="$OUT_BUNDLE/Contents/MacOS/AIFileSorter"
FRAMEWORKS="$OUT_BUNDLE/Contents/Frameworks"
cp "$HEADLESS_BIN" "$BIN"
chmod +x "$BIN"

# GUI stack plus plugins that require it.
rm -rf "$FRAMEWORKS/QtGui.framework" \
       "$FRAMEWORKS/QtWidgets.framework" \
       "$FRAMEWORKS/QtDBus.framework" \
       "$OUT_BUNDLE/Contents/PlugIns"

# PDFium is looked up next to the executable in the Qt bundle; keep it in Frameworks.
if [ ! -e "$FRAMEWORKS/libpdfium.dylib" ]; then
    pdfium_source="$(find "$BASE_BUNDLE/Contents" -name 'libpdfium.dylib' -maxdepth 3 -print -quit 2>/dev/null || true)"
    if [ -n "$pdfium_source" ]; then
        cp "$pdfium_source" "$FRAMEWORKS/libpdfium.dylib"
    fi
fi

rewrite_dependency() {
    local dependency="$1"
    local target=""

    case "$dependency" in
        /opt/*|/usr/local/*)
            if [[ "$dependency" == *".framework/"* ]]; then
                local framework
                framework="$(printf '%s' "$dependency" | sed -E 's#.*/([^/]+\.framework)/.*#\1#')"
                target="@rpath/$framework/Versions/A/${framework%.framework}"
            else
                target="@rpath/$(basename "$dependency")"
            fi
            ;;
        ./libpdfium.dylib|@rpath/libpdfium.dylib)
            target="@rpath/libpdfium.dylib"
            ;;
        *)
            return 0
            ;;
    esac

    if [[ "$target" == @rpath/*.framework/* ]]; then
        local framework
        framework="$(printf '%s' "$target" | sed -E 's#@rpath/([^/]+\.framework)/.*#\1#')"
        if [ ! -e "$FRAMEWORKS/$framework/Versions/A/${framework%.framework}" ]; then
            echo "error: missing bundled framework $framework" >&2
            exit 1
        fi
    else
        local file="${target#@rpath/}"
        if [ ! -e "$FRAMEWORKS/$file" ]; then
            echo "error: missing bundled library $file" >&2
            exit 1
        fi
    fi

    install_name_tool -change "$dependency" "$target" "$BIN"
}

while IFS= read -r dependency; do
    rewrite_dependency "$dependency"
done < <(otool -L "$BIN" | awk '/^\t/{print $1}')

if ! otool -l "$BIN" | grep -q "@executable_path/../Frameworks"; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$BIN"
fi

# The bundle must not fall back to a Homebrew Qt install on this machine.
while IFS= read -r rpath; do
    case "$rpath" in
        /opt/*|/usr/local/*) install_name_tool -delete_rpath "$rpath" "$BIN" 2>/dev/null || true ;;
    esac
done < <(otool -l "$BIN" | awk '/LC_RPATH/{found=1} found && /path /{print $2; found=0}')

# Drop frameworks and dylibs the headless binary never reaches (QtNetwork,
# image/OpenGL libraries, text shaping pulled in by the GUI stack, etc.).
python3 - "$OUT_BUNDLE" <<'PY'
import os
import shutil
import subprocess
import sys

bundle = sys.argv[1]
frameworks = os.path.join(bundle, "Contents", "Frameworks")
executable = os.path.join(bundle, "Contents", "MacOS", "AIFileSorter")


def dependencies(path):
    result = subprocess.run(["otool", "-L", path], capture_output=True, text=True)
    for line in result.stdout.splitlines()[1:]:
        fields = line.strip().split(" ")
        if fields and fields[0]:
            yield fields[0]


def resolve(dependency, current):
    if dependency.startswith("@rpath/"):
        relative = dependency[len("@rpath/"):]
        if ".framework/" in relative:
            head = relative.split("/", 1)[0]
            name = head[: -len(".framework")]
            return os.path.join(frameworks, head, "Versions", "Current", name)
        return os.path.join(frameworks, relative)
    if dependency.startswith("@loader_path/"):
        return os.path.normpath(os.path.join(os.path.dirname(current),
                                             dependency[len("@loader_path/"):]))
    if dependency.startswith("./"):
        return os.path.normpath(os.path.join(os.path.dirname(current), dependency[2:]))
    return None


keep = set()
pending = [executable]
while pending:
    current = pending.pop()
    if not os.path.exists(current):
        continue
    real = os.path.realpath(current)
    if real in keep:
        continue
    keep.add(real)
    for dependency in dependencies(current):
        target = resolve(dependency, current)
        if target and os.path.exists(target):
            pending.append(target)

def is_kept(path):
    real = os.path.realpath(path)
    if real in keep:
        return True
    prefix = real + os.sep
    return any(item.startswith(prefix) for item in keep)


removed = []
for name in sorted(os.listdir(frameworks)):
    path = os.path.join(frameworks, name)
    if is_kept(path):
        continue
    if os.path.isdir(path):
        shutil.rmtree(path)
    else:
        os.remove(path)
    removed.append(name)

print("Pruned: " + (", ".join(removed) if removed else "nothing"))

# Fail loudly if anything still resolves outside the bundle.
missing = []
for root, _dirs, files in os.walk(frameworks):
    for name in files:
        candidate = os.path.join(root, name)
        if os.path.islink(candidate) or not os.path.isfile(candidate):
            continue
        for dependency in dependencies(candidate):
            if dependency.startswith("@rpath/"):
                target = os.path.join(frameworks, dependency[len("@rpath/"):])
                if not os.path.exists(target):
                    missing.append(dependency)
if missing:
    print("error: unresolved bundled dependencies: " + ", ".join(sorted(set(missing))), file=sys.stderr)
    sys.exit(1)
PY

codesign --force --deep --sign - "$OUT_BUNDLE" >/dev/null

echo "Headless engine bundle: $OUT_BUNDLE"
du -sh "$OUT_BUNDLE"
otool -L "$BIN" | awk '/^\t/{print $1}' | grep -E "Qt|homebrew" || true
