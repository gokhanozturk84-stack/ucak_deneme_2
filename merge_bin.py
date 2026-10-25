"""Derleme sonrasi: bootloader + partitions + boot_app0 + uygulama -> tek dosya.

Cikti: <proje>/firmware_merged_<env>.bin  (0x0 adresine yazilir)
"""
import os
import subprocess

Import("env")  # noqa: F821  (PlatformIO tarafindan saglanir)


def merge_bin(source, target, env):  # noqa: ARG001
    build_dir = env.subst("$BUILD_DIR")
    app = os.path.join(build_dir, env.subst("${PROGNAME}.bin"))
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")

    platform = env.PioPlatform()
    fw_dir = platform.get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(fw_dir, "tools", "partitions", "boot_app0.bin")
    esptool = os.path.join(platform.get_package_dir("tool-esptoolpy"), "esptool.py")

    out = os.path.join(env.subst("$PROJECT_DIR"),
                       "firmware_merged_%s.bin" % env.subst("$PIOENV"))

    cmd = [
        env.subst("$PYTHONEXE"), esptool,
        "--chip", "esp32c3",
        "merge_bin",
        "-o", out,
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "4MB",
        "0x0", bootloader,       # ESP32-C3'te bootloader 0x0'dadir
        "0x8000", partitions,
        "0xe000", boot_app0,
        "0x10000", app,
    ]
    print("Birlesik .bin olusturuluyor:", out)
    subprocess.check_call(cmd)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)  # noqa: F821
