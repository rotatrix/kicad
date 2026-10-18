#!/usr/bin/env bash
# Native KiCad build with the official dependency builder; no system Python writes.
set -euo pipefail
stage=${1:-all}
arch=${2:-$(uname -m)}
case "$stage" in deps|resources|configure|build|package|test|all) ;; *) echo "Usage: $0 {deps|resources|configure|build|package|test|all} [arm64|x86_64]" >&2; exit 2;; esac
case "$arch" in arm64|x86_64) ;; *) exit 2;; esac
[[ $(uname -s) == Darwin && $(uname -m) == "$arch" ]] || { echo 'Use a native runner (or matching Rosetta shell and Homebrew).' >&2; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
cache="$root/build-tools"
builder="$cache/mac-builder"
deps="$builder/build-$arch-wx3.2"
build="$root/build/macos-$arch"
jobs=${JOBS:-3}
python=${PYTHON:-$(brew --prefix python@3.14)/bin/python3.14}
builder_sha=370400dc2e5cbfcd20b762c02f45730755516e87
wx_url=https://files.pythonhosted.org/packages/22/43/81657a6b126ffc19163500a8184d683cec08eb4e1d06905cd0c371c702d0/wxpython-4.2.5.tar.gz
wx_sha=44e836d1bccd99c38790bb034b6ecf70d9060f6734320560f7c4b0d006144793
wx_src="$cache/wxpython-4.2.5-$arch"
venv="$cache/python-build-$arch"
framework="$cache/python-$arch/Python.framework"
app="$deps/kicad-dest/KiCad.app"
export PATH="$(brew --prefix bison)/bin:$(brew --prefix gettext)/bin:$PATH"
export WX_SKIP_DOXYGEN_VERSION_CHECK=1
mkdir -p "$cache/logs" "$root/artifacts"

prepare_deps() {
    # Install missing Homebrew prerequisites explicitly before invoking this script.
    # See the pinned builder's ci/src/brew_deps.sh; additionally requires swig.
    if [[ ! -d "$builder/.git" ]]; then
        git clone https://gitlab.com/kicad/packaging/kicad-mac-builder.git "$builder"
        git -C "$builder" checkout --detach "$builder_sha"
    fi
    [[ $(git -C "$builder" rev-parse HEAD) == "$builder_sha" ]]
    "$python" "$builder/build.py" --arch "$arch" --wx-version 3.2 \
        --build-dir "$deps" --build-type Release --jobs "$jobs" \
        --no-retry-failed-build --target setup-kicad-dependencies
    # The builder names moving branches; refuse an unreviewed dependency update.
    [[ $(git -C "$deps/wxwidgets/src/wxwidgets" rev-parse HEAD) == 0bb586c9a69cc12cd87caef874880293e55d93db ]]
    [[ $(git -C "$deps/ngspice/src/ngspice" rev-parse HEAD) == a80f6e3e95d51534905b1f23410a951802666656 ]]
    if [[ ! -d "$wx_src" ]]; then
        curl --fail --location "$wx_url" -o "$cache/wxpython-4.2.5.tar.gz"
        echo "$wx_sha  $cache/wxpython-4.2.5.tar.gz" | shasum -a 256 -c -
        mkdir "$wx_src"
        tar -xzf "$cache/wxpython-4.2.5.tar.gz" --strip-components=1 -C "$wx_src"
    fi
    [[ -d "$venv" ]] || "$python" -m venv "$venv"
    "$venv/bin/python" -m pip install 'setuptools==80.9.0' 'wheel==0.45.1' 'sip==6.15.1' 'Cython==3.1.4'
    (
        cd "$wx_src"
        export PATH="$deps/wxwidgets-dest/bin:$PATH"
        "$venv/bin/python" build.py --use_syswx --mac_arch="$arch" --jobs="$jobs" build_py
    )
    "$venv/bin/python" -m pip install 'dyldstyle @ git+https://gitlab.com/adamwwolf/dyldstyle.git@345039910bc57777b1d60c98a5c1877761a4130f'
    # Stage a private framework, replacing Homebrew's escaping site-packages link.
    # Upstream KiCad copies and relocates this framework during cmake --install.
    if [[ ! -d "$framework" ]]; then
        original=$("$python" -c 'import pathlib,sys; print(pathlib.Path(sys.base_prefix).parents[1])')
        [[ "$original" == */Python.framework ]]
        mkdir -p "$(dirname "$framework")"
        ditto "$original" "$framework"
    fi
    pyver=$("$python" -c 'import sys;print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    site="$framework/Versions/$pyver/lib/python$pyver/site-packages"
    if [[ -L "$site" ]]; then rm "$site"; fi
    mkdir -p "$site"
    ditto "$wx_src/wx" "$site/wx"
    PYTHONHOME="$framework/Versions/$pyver" "$python" -c 'import wx; print(wx.version())'
}

configure() {
    pyver=$("$python" -c 'import sys;print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    pyhome="$framework/Versions/$pyver"
    test -d "$pyhome/lib/python$pyver/site-packages/wx"
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$builder/toolchain/kicad-mac-builder.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DKICAD_OPENAXIS=ON -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=ON \
        -DKICAD_SCRIPTING_WXPYTHON=ON \
        -DPYTHON_EXECUTABLE="$python" -DPYTHON_INCLUDE_DIR="$pyhome/include/python$pyver" \
        -DPYTHON_LIBRARY="$pyhome/Python" -DPYTHON_FRAMEWORK="$framework" \
        -DPYTHON_SITE_PACKAGE_PATH="$pyhome/lib/python$pyver/site-packages" \
        -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
}

build_app() {
    cmake --build "$build" --target openaxis_camera_checks openaxis_scheduler_checks --parallel "$jobs"
    ctest --test-dir "$build" -R 'openaxis_(native_camera|wx_scheduler)' --output-on-failure --no-tests=error
    cmake --build "$build" --parallel "$jobs"
}

prepare_resources() {
    # Use the official library targets, pinned to the application's release series.
    # Their outputs are copied exactly as in the builder's kicad.cmake install steps.
    docs="$cache/kicad-doc-10.0-efa5960.tar.gz"
    if [[ ! -f "$docs" ]]; then
        curl --fail --location 'https://gitlab.com/api/v4/projects/15621628/jobs/16981626772/artifacts/archive/kicad-doc-unknown.tar.gz' -o "$docs"
    fi
    echo "f40c61d8c1b2eb9d99c67fc30cd7ab358c7562cc5f2499bcc2f5d2e7e1bb4c6b  $docs" | shasum -a 256 -c -
    cmake -S "$builder/kicad-mac-builder" -B "$deps" \
        -DFOOTPRINTS_TAG=eff413fd489b7ade27fc108781a95c4b28ccf0b7 \
        -DSYMBOLS_TAG=495444d353551ba5f610d47d22c979c50b0bd70c \
        -DTEMPLATES_TAG=3ed4538b0f965d821df63a5fffc4441e723cfe7f \
        -DPACKAGES3D_TAG=ca5b7874dad95c1c36b7ad8db6ca6b82e4a2a89e -DDOCS_TARBALL_URL="$docs"
    cmake --build "$deps" --target footprints symbols templates packages3d docs --parallel "$jobs"
}

package() {
    prepare_resources
    cmake --install "$build"
    for resource in footprints symbols templates packages3d; do
        ditto "$deps/$resource/src/$resource-build/output" "$app/Contents/SharedSupport"
    done
    ditto "$deps/docs/src/docs/share/doc/kicad/help" "$app/Contents/SharedSupport/help"
    mkdir -p "$app/Contents/Resources/Licenses/wxPython"
    cp "$wx_src/LICENSE.txt" "$app/Contents/Resources/Licenses/wxPython/"
    ditto "$wx_src/license" "$app/Contents/Resources/Licenses/wxPython/license"
    mkdir -p "$app/Contents/Resources/Licenses/wxWidgets" "$app/Contents/Resources/Licenses/ngspice"
    cp "$deps/wxwidgets/src/wxwidgets/docs/"{licence,lgpl,gpl}.txt "$app/Contents/Resources/Licenses/wxWidgets/"
    cp "$deps/ngspice/src/ngspice/COPYING" "$app/Contents/Resources/Licenses/ngspice/"
    # Retain Homebrew dependency notices and exact public source references.
    "$python" - "$app/Contents/Resources/Licenses" <<'PYTHON'
import pathlib, shutil, subprocess, sys
out = pathlib.Path(sys.argv[1])
formulae = ['glew', 'opencascade', 'boost', 'harfbuzz', 'cairo', 'libgit2',
            'openssl@3', 'unixodbc', 'protobuf', 'nng', 'zstd', 'libomp',
            'jpeg-turbo', 'python@3.14']
transitive = subprocess.check_output(['brew', 'deps', '--union', '--installed', *formulae], text=True).split()
references = []
for name in sorted(set(formulae + transitive)):
    prefix = pathlib.Path(subprocess.check_output(['brew', '--prefix', name], text=True).strip()).resolve()
    target = out / name
    target.mkdir(exist_ok=True)
    for pattern in ('LICENSE*', 'COPYING*', 'NOTICE*', 'AUTHORS*'):
        for notice in prefix.glob(pattern):
            if notice.is_file():
                shutil.copy2(notice, target / notice.name)
    # Installed recipes/SBOM identify the actual keg, unlike today's formula API.
    for recipe in (prefix / '.brew').glob('*.rb'):
        shutil.copy2(recipe, target / recipe.name)
    if (prefix / 'sbom.spdx.json').exists():
        shutil.copy2(prefix / 'sbom.spdx.json', target / 'sbom.spdx.json')
    references.append(f'{name} {prefix.name}: source URLs and patches in the installed recipe/SBOM')
(out / 'Homebrew-sources.txt').write_text('\n'.join(references) + '\n')
PYTHON
    pyver=$("$python" -c 'import sys;print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    # Python extension modules can load dependencies not used by the KiCad executable.
    # Reuse upstream collection/relocation and inside-out signing after all resources.
    cat > "$cache/finish-bundle-$arch.cmake" <<CMAKE
set(OSX_BUNDLE_INSTALL_LIB_DIR "$app/Contents/Frameworks")
include("$root/cmake/InstallSteps/InstallMacOS.cmake")
file(GLOB_RECURSE python_modules "$app/Contents/Frameworks/Python.framework/*.so")
foreach(module \${python_modules})
    install_runtime_deps("" "\${module}" "")
endforeach()
include("$root/cmake/InstallSteps/RefixupMacOS.cmake")
refix_kicad_bundle("$app")
CMAKE
    cmake -P "$cache/finish-bundle-$arch.cmake"
    "$venv/bin/wrangle-bundle" --fix --python-version "$pyver" "$app"
    cat > "$cache/sign-bundle-$arch.cmake" <<CMAKE
include("$root/cmake/InstallSteps/SignMacOS.cmake")
sign_kicad_bundle("$app" "-" OFF OFF "")
verify_signing("$app")
CMAKE
    cmake -P "$cache/sign-bundle-$arch.cmake"
    sha=$(git -C "$root" rev-parse HEAD)
    COPYFILE_DISABLE=1 tar -czf "$root/artifacts/macos-app-$arch-$sha.tar.gz" -C "$(dirname "$app")" KiCad.app
    "$python" "$root/tools/test-macos-package.py" "$root/artifacts/macos-app-$arch-$sha.tar.gz" "$arch"
}
case "$stage" in
    deps) prepare_deps;;
    resources) prepare_resources;;
    configure) configure;;
    build) build_app;;
    package) package;;
    test) "$python" "$root/tools/test-macos-package.py" "$root/artifacts/macos-app-$arch-$(git -C "$root" rev-parse HEAD).tar.gz" "$arch";;
    all) prepare_deps; configure; build_app; package;;
esac
