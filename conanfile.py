from pathlib import Path

from conan import ConanFile
from conan.tools.gnu import PkgConfigDeps
from conan.tools.meson import MesonToolchain


class BilliardsConan(ConanFile):
    name = "billiards"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"

    default_options = {
        "krisp/*:build_applications": False,
        "krisp/*:build_tests": False,
    }

    def requirements(self):
        self.requires("krisp/0.1.0")

    def layout(self):
        self.folders.build = "build/debug"
        self.folders.generators = "build/conan"

    def generate(self):
        MesonToolchain(self).generate()
        PkgConfigDeps(self).generate()

        krisp = self.dependencies["krisp"]
        runtime_dir = Path(krisp.cpp_info.builddirs[0])
        if not runtime_dir.is_absolute():
            runtime_dir = Path(krisp.package_folder) / runtime_dir
        native_file = Path(self.generators_folder) / "krisp-runtime.ini"
        native_file.parent.mkdir(parents=True, exist_ok=True)
        native_file.write_text(
            "[project options]\n"
            f"krisp_runtime_directory = '{runtime_dir.as_posix()}'\n",
            encoding="utf-8",
        )
